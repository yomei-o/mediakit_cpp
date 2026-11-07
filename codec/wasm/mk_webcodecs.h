// The browser's H.264 encoder and decoder (WebCodecs) for mk_codec.cpp in the wasm build. Included by
// mk_codec.cpp only (EM_JS defines the functions here).
// WebCodecs works asynchronously: encoded chunks and decoded frames come back through callbacks, are queued
// in Module.mkWc[id], and the C++ side takes what is ready on its next encode / decode call -- as with the
// hardware encoders on the other systems, output lags input by a few frames.
#pragma once
#include <emscripten.h>

EM_JS_DEPS(mk_webcodecs, "$UTF8ToString,$stringToNewUTF8,malloc,free");

// Opens an encoder; picks the H.264 profile/level for the size and tries hardware first when asked.
// Returns 1, or 0 with the reason in mk_wc_error().
EM_ASYNC_JS(int, mk_wc_venc_open, (int id, int w, int h, int fps, int bitrate, int hw), {
    Module.mkWc = Module.mkWc || {};
    const e = { out: [], error: "", name: "" };
    Module.mkWc[id] = e;
    try {
        if (typeof VideoEncoder == "undefined") throw new Error("WebCodecs is not available in this browser");
        const mbs = Math.ceil(w / 16) * Math.ceil(h / 16), rate = mbs * fps;
        /* H.264 levels: max macroblocks per frame, per second */
        const levels = [[1620, 40500, "1e"], [3600, 108000, "1f"], [5120, 216000, "20"], [8192, 245760, "28"],
                        [8704, 522240, "2a"], [22080, 589824, "32"], [36864, 983040, "33"], [36864, 2073600, "34"]];
        let level = "34";
        for (const l of levels)
            if (mbs <= l[0] && rate <= l[1]) {
                level = l[2];
                break;
            }
        let cfg = null;
        for (const accel of hw ? ["prefer-hardware", "no-preference"] : ["prefer-software", "no-preference"])
            for (const profile of ["42e0", "4d00", "6400"]) {
                if (cfg) break;
                const c = { codec: "avc1." + profile + level, width: w, height: h, bitrate: bitrate, framerate: fps,
                            hardwareAcceleration: accel, latencyMode: "realtime", avc: { format: "annexb" } };
                try {
                    if ((await VideoEncoder.isConfigSupported(c)).supported) cfg = c;
                } catch (x) {
                }
            }
        if (!cfg) throw new Error("the browser cannot encode H.264 at " + w + "x" + h);
        e.enc = new VideoEncoder({
            output: chunk => {
                const d = new Uint8Array(chunk.byteLength);
                chunk.copyTo(d);
                e.out.push({ d: d, t: chunk.timestamp, k: chunk.type == "key" });
            },
            error: x => {
                e.error = String(x && x.message ? x.message : x);
            }
        });
        e.enc.configure(cfg);
        e.name = "webcodecs " + cfg.codec + (cfg.hardwareAcceleration == "prefer-hardware" ? " hw" : "");
        return 1;
    } catch (x) {
        e.error = String(x && x.message ? x.message : x);
        return 0;
    }
});

// fmt 0 I420, 1 NV12, 2 BGRA. Returns 1 queued, 0 dropped (the encoder is behind), -1 error.
EM_JS(int, mk_wc_venc_encode, (int id, int fmt, int w, int h, const uint8_t* p0, int s0, const uint8_t* p1, int s1,
                               const uint8_t* p2, int s2, double ts, int key), {
    const e = Module.mkWc[id];
    if (!e || !e.enc || e.enc.state != "configured" || e.error) return -1;
    if (e.enc.encodeQueueSize > 4) return 0;
    const cw = (w + 1) >> 1, ch = (h + 1) >> 1;
    const planes = fmt == 0 ? [[p0, s0, w, h], [p1, s1, cw, ch], [p2, s2, cw, ch]]
                 : fmt == 1 ? [[p0, s0, w, h], [p1, s1, cw * 2, ch]] : [[p0, s0, w * 4, h]];
    let size = 0;
    for (const p of planes) size += p[2] * p[3];
    const buf = new Uint8Array(size);
    let off = 0;
    for (const p of planes) {
        if (p[1] == p[2]) {
            buf.set(HEAPU8.subarray(p[0], p[0] + p[2] * p[3]), off);
            off += p[2] * p[3];
        } else {
            for (let y = 0; y < p[3]; ++y, off += p[2]) buf.set(HEAPU8.subarray(p[0] + y * p[1], p[0] + y * p[1] + p[2]), off);
        }
    }
    const frame = new VideoFrame(buf, { format: ["I420", "NV12", "BGRA"][fmt], codedWidth: w, codedHeight: h, timestamp: ts });
    e.enc.encode(frame, { keyFrame: !!key });
    frame.close();
    return 1;
});

EM_JS(int, mk_wc_venc_backlog, (int id), {
    const e = Module.mkWc[id];
    return e && e.enc ? e.enc.encodeQueueSize : 0;
});

// Waits for everything given to the encoder to come out.
EM_ASYNC_JS(int, mk_wc_venc_flush, (int id), {
    const e = Module.mkWc[id];
    if (!e || !e.enc || e.enc.state != "configured") return 0;
    try {
        await e.enc.flush();
        return 1;
    } catch (x) {
        e.error = String(x && x.message ? x.message : x);
        return 0;
    }
});

// The next encoded chunk (malloc'd; null when none is ready).
EM_JS(uint8_t*, mk_wc_venc_next, (int id, int* size, double* ts, int* key), {
    const e = Module.mkWc[id];
    if (!e || !e.out.length) return 0;
    const o = e.out.shift();
    const p = _malloc(o.d.length);
    HEAPU8.set(o.d, p);
    HEAP32[size >> 2] = o.d.length;
    HEAPF64[ts >> 3] = o.t;
    HEAP32[key >> 2] = o.k ? 1 : 0;
    return p;
});

// A decoder; it is configured at the first keyframe (the codec string comes from its SPS).
// Decoded frames are copied out of the browser asynchronously and queued in order.
EM_JS(int, mk_wc_vdec_open, (int id), {
    if (typeof VideoDecoder == "undefined") return 0;
    Module.mkWc = Module.mkWc || {};
    const e = { out: [], error: "", configured: false, pending: 0, flushed: false, flushing: false };
    Module.mkWc[id] = e;
    e.dec = new VideoDecoder({
        output: f => {
            const o = { ready: false, t: f.timestamp };
            e.out.push(o);
            const r = f.visibleRect;
            o.w = r.width, o.h = r.height;
            if (f.format == "I420" || f.format == "NV12") {
                o.fmt = f.format == "I420" ? 0 : 1;
                o.buf = new Uint8Array(f.allocationSize({ rect: r }));
                ++e.pending;
                f.copyTo(o.buf, { rect: r }).then(layout => {
                    o.layout = layout;
                    o.ready = true;
                }).catch(x => {
                    e.error = String(x && x.message ? x.message : x);
                    o.drop = o.ready = true;
                }).finally(() => {
                    f.close();
                    --e.pending;
                });
            } else { /* another layout (or a GPU-only frame): draw it and read RGBA back */
                const c = e.canvas || (e.canvas = new OffscreenCanvas(o.w, o.h));
                if (c.width != o.w || c.height != o.h) c.width = o.w, c.height = o.h;
                const ctx = e.ctx || (e.ctx = c.getContext("2d", { willReadFrequently: true }));
                ctx.drawImage(f, 0, 0);
                f.close();
                o.fmt = 2;
                o.buf = ctx.getImageData(0, 0, o.w, o.h).data;
                o.layout = [{ offset: 0, stride: o.w * 4 }];
                o.ready = true;
            }
        },
        error: x => {
            e.error = String(x && x.message ? x.message : x);
        }
    });
    return 1;
});

// Returns 1 accepted (or skipped while waiting for the first keyframe), 0 error.
EM_JS(int, mk_wc_vdec_decode, (int id, const uint8_t* d, int n, double ts, int key, const char* codec), {
    const e = Module.mkWc[id];
    if (!e || e.error || e.dec.state == "closed") return 0;
    if (!e.configured) {
        if (!key) return 1;
        try {
            e.dec.configure({ codec: UTF8ToString(codec), optimizeForLatency: true });
        } catch (x) {
            e.error = String(x && x.message ? x.message : x);
            return 0;
        }
        e.configured = true;
    }
    if (e.flushing) return 1; /* drain() in progress: the caller reads on after it */
    e.flushed = false;
    try {
        e.dec.decode(new EncodedVideoChunk({ type: key ? "key" : "delta", timestamp: ts, data: HEAPU8.slice(d, d + n) }));
    } catch (x) {
        e.error = String(x && x.message ? x.message : x);
        return 0;
    }
    return 1;
});

// The next decoded frame in order (malloc'd pixels; null when none is ready).
// info: fmt (0 I420, 1 NV12, 2 RGBA), w, h, then offset/stride of up to three planes.
EM_JS(uint8_t*, mk_wc_vdec_next, (int id, int* info, double* ts), {
    const e = Module.mkWc[id];
    while (e && e.out.length && e.out[0].ready && e.out[0].drop) e.out.shift();
    if (!e || !e.out.length || !e.out[0].ready) return 0;
    const o = e.out.shift();
    const p = _malloc(o.buf.length);
    HEAPU8.set(o.buf, p);
    const v = [o.fmt, o.w, o.h];
    for (let i = 0; i < 3; ++i) v.push(o.layout[i] ? o.layout[i].offset : 0, o.layout[i] ? o.layout[i].stride : 0);
    for (let i = 0; i < 9; ++i) HEAP32[(info >> 2) + i] = v[i];
    HEAPF64[ts >> 3] = o.t;
    return p;
});

// Flushes without waiting; returns 1 once every frame given to the decoder is out (and resets for the next).
EM_JS(int, mk_wc_vdec_drain, (int id), {
    const e = Module.mkWc[id];
    if (!e || e.error || !e.configured || e.dec.state != "configured") return 1;
    if (e.flushed && !e.pending) {
        e.flushed = false;
        return 1;
    }
    if (!e.flushing) {
        e.flushing = true;
        e.dec.flush().catch(x => {
            e.error = String(x && x.message ? x.message : x);
        }).finally(() => {
            e.flushing = false;
            e.flushed = true;
        });
    }
    return 0;
});

// Waits until every frame given to the decoder is out.
EM_ASYNC_JS(void, mk_wc_vdec_flush, (int id), {
    const e = Module.mkWc[id];
    if (!e || e.error || !e.configured || e.dec.state != "configured") return;
    try {
        await e.dec.flush();
    } catch (x) {
        e.error = String(x && x.message ? x.message : x);
    }
    while (e.pending) await new Promise(r => setTimeout(r, 1));
});

EM_JS(char*, mk_wc_error, (int id), {
    const e = Module.mkWc && Module.mkWc[id];
    return stringToNewUTF8(e ? e.error : "");
});

EM_JS(char*, mk_wc_name, (int id), {
    const e = Module.mkWc && Module.mkWc[id];
    return stringToNewUTF8(e ? e.name : "");
});

EM_JS(void, mk_wc_close, (int id), {
    const e = Module.mkWc && Module.mkWc[id];
    if (!e) return;
    for (const c of [e.enc, e.dec])
        if (c && c.state != "closed") try {
                c.close();
            } catch (x) {
            }
    delete Module.mkWc[id];
});
