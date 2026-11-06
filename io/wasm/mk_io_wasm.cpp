// mediakit io for the browser (Emscripten): camera and microphone through getUserMedia, speaker through Web Audio.
// There are no threads: a JS timer grabs the <video> into a canvas at the configured fps, and AudioWorklets
// move 10 ms blocks of audio; both call back into C++ on the main thread, whenever the program is waiting
// (mk::Window::run, emscripten_sleep). open() awaits the browser's permission prompt, so programs are
// linked with -sASYNCIFY (CMake adds it). Frames are BGRA.
// Browsers start audio only after the user has clicked or typed on the page: until then the microphone
// delivers nothing and the speaker is silent (both start on the first click / key).
#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <sstream>

#include "mk_io.h"

EM_JS_DEPS(mk_io_wasm, "$UTF8ToString,$stringToNewUTF8,malloc,free");

// All browser state lives in Module.mkCams[id]; C++ keeps only the id.
EM_ASYNC_JS(int, mk_js_cam_open, (int id, const char* device, int width, int height), {
    Module.mkCams = Module.mkCams || {};
    const cam = { running: false, timer: 0, error: "" };
    Module.mkCams[id] = cam;
    try {
        if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia)
            throw new Error('getUserMedia is not available (the page must be https:// or http://localhost)');
        const video = { width: { ideal: width }, height: { ideal: height } };
        const dev = UTF8ToString(device);
        if (dev) video.deviceId = { exact: dev };
        cam.stream = await navigator.mediaDevices.getUserMedia({ video: video, audio: false });
        cam.video = document.createElement('video');
        cam.video.muted = true;
        cam.video.playsInline = true;
        cam.video.srcObject = cam.stream;
        await cam.video.play();
        for (let i = 0; i < 300 && !cam.video.videoWidth; ++i) await new Promise(r => setTimeout(r, 10));
        if (!cam.video.videoWidth) throw new Error('the camera sent no picture');
        cam.canvas = document.createElement('canvas');
        cam.ctx = cam.canvas.getContext('2d', { willReadFrequently: true });
        return 1;
    } catch (e) {
        cam.error = (e && e.name ? e.name + ': ' : "") + (e && e.message ? e.message : String(e));
        if (cam.stream) cam.stream.getTracks().forEach(t => t.stop());
        cam.stream = null;
        return 0;
    }
});

EM_JS(char*, mk_js_cam_info, (int id), {
    // "width\theight\tlabel" or "\t\terror"
    const cam = Module.mkCams && Module.mkCams[id];
    if (!cam) return stringToNewUTF8('\t\tno camera');
    if (!cam.stream) return stringToNewUTF8('\t\t' + cam.error);
    const label = cam.stream.getVideoTracks()[0].label || 'camera';
    return stringToNewUTF8(cam.video.videoWidth + '\t' + cam.video.videoHeight + '\t' + label);
});

// fps ticks on the wall clock; a late tick (slow callback) skips ahead instead of bursting.
// test: no camera, C++ draws the frame.
EM_JS(void, mk_js_cam_start, (int id, double fps, int test), {
    Module.mkCams = Module.mkCams || {};
    const cam = Module.mkCams[id] || (Module.mkCams[id] = { error: "" });
    const period = 1000 / fps;
    const t0 = performance.now();
    let n = 0;
    const tick = () => {
        if (!cam.running) return;
        let ptr = 0, w = 0, h = 0;
        if (!test) {
            const v = cam.video;
            w = v.videoWidth, h = v.videoHeight;
            if (w && h) {
                if (cam.canvas.width != w || cam.canvas.height != h) cam.canvas.width = w, cam.canvas.height = h;
                cam.ctx.drawImage(v, 0, 0, w, h);
                const img = cam.ctx.getImageData(0, 0, w, h);
                ptr = _malloc(img.data.length);
                HEAPU8.set(img.data, ptr);
            }
        }
        if (test || ptr) _mk_wasm_camera_frame(id, ptr, w, h);
        if (ptr) _free(ptr);
        const now = performance.now();
        n = Math.max(n + 1, Math.floor((now - t0) / period));
        if (cam.running) cam.timer = setTimeout(tick, Math.max(0, t0 + n * period - now));
    };
    cam.running = true;
    cam.timer = setTimeout(tick, 0);
});

EM_JS(void, mk_js_cam_stop, (int id), {
    const cam = Module.mkCams && Module.mkCams[id];
    if (!cam) return;
    cam.running = false;
    clearTimeout(cam.timer);
});

EM_JS(void, mk_js_cam_close, (int id), {
    const cam = Module.mkCams && Module.mkCams[id];
    if (!cam) return;
    cam.running = false;
    clearTimeout(cam.timer);
    if (cam.stream) cam.stream.getTracks().forEach(t => t.stop());
    delete Module.mkCams[id];
});

// "deviceId\tlabel\n" per device of a kind ("videoinput", "audioinput", "audiooutput").
// Labels stay empty until the page has been given camera / microphone permission.
EM_ASYNC_JS(char*, mk_js_list_devices, (const char* kind), {
    const k = UTF8ToString(kind);
    let out = "";
    try {
        const devs = await navigator.mediaDevices.enumerateDevices();
        for (const d of devs)
            if (d.kind == k) out += d.deviceId + '\t' + (d.label || k) + '\n';
    } catch (e) {
    }
    return stringToNewUTF8(out);
});

// ---------------------------------------------------------------- audio (Web Audio + AudioWorklet)

// The two worklet processors, and resuming a context on the first click when the browser holds it back.
EM_JS(void, mk_js_audio_init, (), {
    if (Module.mkAudio) return;
    Module.mkAudio = {};
    const procs = function() {
        /* microphone: interleaved s16 blocks of `chunk` frames, with the context time just after the block */
        class Mic extends AudioWorkletProcessor {
            constructor(o) {
                super();
                this.ch = o.processorOptions.channels;
                this.chunk = o.processorOptions.chunk;
                this.buf = new Int16Array(this.chunk * this.ch);
                this.n = 0;
            }
            process(inputs) {
                const inp = inputs[0];
                if (!inp || !inp.length) return true;
                const frames = inp[0].length;
                for (let i = 0; i < frames; ++i) {
                    for (let c = 0; c < this.ch; ++c) {
                        const v = inp[Math.min(c, inp.length - 1)][i];
                        this.buf[this.n * this.ch + c] = v <= -1 ? -32768 : v >= 1 ? 32767 : Math.round(v * 32767);
                    }
                    if (++this.n == this.chunk) {
                        this.port.postMessage({ s: this.buf, t: currentTime + (i + 1) / sampleRate }, [this.buf.buffer]);
                        this.buf = new Int16Array(this.chunk * this.ch);
                        this.n = 0;
                    }
                }
                return true;
            }
        }
        registerProcessor("mk-mic", Mic);
        /* speaker: plays queued interleaved s16 blocks, reports how many real frames it has played */
        class Spk extends AudioWorkletProcessor {
            constructor() {
                super();
                this.q = [];
                this.off = 0;
                this.played = 0;
                this.ticks = 0;
                this.idle = true;
                this.port.onmessage = e => {
                    const m = e.data;
                    if (m.clear) this.q = [], this.off = 0, this.played = m.base;
                    else this.q.push(m.s);
                };
            }
            process(inputs, outputs) {
                const out = outputs[0], ch = out.length, frames = out[0].length;
                let i = 0;
                while (i < frames && this.q.length) {
                    const s = this.q[0], n = s.length / ch;
                    const take = Math.min(frames - i, n - this.off);
                    for (let k = 0; k < take; ++k)
                        for (let c = 0; c < ch; ++c) out[c][i + k] = s[(this.off + k) * ch + c] / 32768;
                    i += take;
                    this.off += take;
                    this.played += take;
                    if (this.off == n) this.q.shift(), this.off = 0;
                }
                for (; i < frames; ++i)
                    for (let c = 0; c < ch; ++c) out[c][i] = 0;
                const idle = !this.q.length;
                if (++this.ticks % 4 == 0 || idle != this.idle)
                    this.port.postMessage({ played: this.played, t: currentTime + frames / sampleRate, idle: idle });
                this.idle = idle;
                return true;
            }
        }
        registerProcessor("mk-spk", Spk);
    };
    Module.mkAudioUrl = URL.createObjectURL(new Blob(["(" + procs.toString() + ")();"], { type: "text/javascript" }));
    const gestures = ["pointerdown", "keydown", "touchend"];
    Module.mkAudioRun = (ctx, want) => {
        ctx.mkWant = want;
        if (!want) {
            if (ctx.state == "running") ctx.suspend();
            return;
        }
        const kick = () => {
            if (ctx.mkWant && ctx.state == "suspended") ctx.resume();
        };
        if (!ctx.mkKick) {
            ctx.mkKick = kick;
            for (const t of gestures) document.addEventListener(t, kick, true);
            setTimeout(() => {
                if (ctx.mkWant && ctx.state == "suspended") console.warn("mediakit: click the page to start the audio");
            }, 500);
        }
        kick();
    };
    Module.mkAudioClose = a => {
        a.running = false;
        if (a.timer) clearInterval(a.timer), a.timer = 0;
        if (a.stream) a.stream.getTracks().forEach(t => t.stop());
        if (a.ctx) {
            if (a.ctx.mkKick)
                for (const t of gestures) document.removeEventListener(t, a.ctx.mkKick, true);
            a.ctx.close();
        }
        a.stream = a.ctx = a.node = null;
    };
});

// Returns the sample rate the browser runs the microphone at (0: failed, the error is in mk_js_audio_label).
EM_ASYNC_JS(int, mk_js_mic_open, (int id, const char* device, int rate, int channels), {
    const a = { error: "", ch: channels, running: false };
    Module.mkAudio[id] = a;
    try {
        if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia)
            throw new Error("getUserMedia is not available (the page must be https:// or http://localhost)");
        const audio = { channelCount: { ideal: channels }, echoCancellation: false, noiseSuppression: false, autoGainControl: false };
        const dev = UTF8ToString(device);
        if (dev) audio.deviceId = { exact: dev };
        a.stream = await navigator.mediaDevices.getUserMedia({ audio: audio, video: false });
        try {
            a.ctx = new AudioContext({ sampleRate: rate });
            a.src = a.ctx.createMediaStreamSource(a.stream);
        } catch (e) {  /* Firefox connects a microphone only at the device rate; C++ resamples */
            if (a.ctx) a.ctx.close();
            a.ctx = new AudioContext();
            a.src = a.ctx.createMediaStreamSource(a.stream);
        }
        a.ctx.suspend();
        await a.ctx.audioWorklet.addModule(Module.mkAudioUrl);
        const r = a.ctx.sampleRate;
        a.node = new AudioWorkletNode(a.ctx, "mk-mic", { processorOptions: { channels: channels, chunk: Math.round(r / 100) } });
        const mute = a.ctx.createGain();
        mute.gain.value = 0;
        a.src.connect(a.node);
        a.node.connect(mute);
        mute.connect(a.ctx.destination);
        a.node.port.onmessage = e => {
            if (!a.running) return;
            const s = e.data.s, frames = s.length / a.ch;
            /* context time -> performance.now(), the clock mk::now_us() reads in the browser */
            const ts = a.ctx.getOutputTimestamp ? a.ctx.getOutputTimestamp() : {};
            const end_ms = ts.performanceTime ? ts.performanceTime + (e.data.t - ts.contextTime) * 1000 : performance.now();
            const p = _malloc(s.length * 2);
            HEAP16.set(s, p >> 1);
            _mk_wasm_mic_audio(id, p, frames, r, end_ms - frames * 1000 / r);
            _free(p);
        };
        a.label = a.stream.getAudioTracks()[0].label || "microphone";
        return r;
    } catch (e) {
        a.error = (e && e.name ? e.name + ': ' : "") + (e && e.message ? e.message : String(e));
        Module.mkAudioClose(a);
        return 0;
    }
});

// test: no microphone, C++ makes the tone; a timer asks for it in 10 ms blocks on the wall clock.
EM_JS(void, mk_js_mic_start, (int id, int test, int rate), {
    const a = Module.mkAudio[id] || (Module.mkAudio[id] = { error: "" });
    a.running = true;
    if (!test) {
        Module.mkAudioRun(a.ctx, true);
        return;
    }
    const chunk = Math.round(rate / 100), t0 = performance.now();
    let sent = 0;
    a.timer = setInterval(() => {
        while (a.running && (sent + chunk) * 1000 / rate <= performance.now() - t0) {
            _mk_wasm_mic_audio(id, 0, chunk, rate, t0 + sent * 1000 / rate);
            sent += chunk;
        }
    }, 10);
});

EM_JS(void, mk_js_mic_stop, (int id), {
    const a = Module.mkAudio[id];
    if (!a) return;
    a.running = false;
    if (a.timer) clearInterval(a.timer), a.timer = 0;
    if (a.ctx) Module.mkAudioRun(a.ctx, false);
});

EM_JS(void, mk_js_audio_close, (int id), {
    const a = Module.mkAudio && Module.mkAudio[id];
    if (!a) return;
    Module.mkAudioClose(a);
    delete Module.mkAudio[id];
});

// The device label once open, the error otherwise.
EM_JS(char*, mk_js_audio_label, (int id), {
    const a = Module.mkAudio && Module.mkAudio[id];
    return stringToNewUTF8(!a ? "not open" : a.ctx ? a.label : a.error);
});

// Returns 1 when the speaker is ready (suspended until start()).
EM_ASYNC_JS(int, mk_js_spk_open, (int id, const char* device, int rate, int channels), {
    const a = { error: "", played: 0, t: 0, idle: true, label: "default" };
    Module.mkAudio[id] = a;
    try {
        if (typeof AudioContext == "undefined") throw new Error("Web Audio is not available");
        a.ctx = new AudioContext({ sampleRate: rate, latencyHint: "interactive" });
        a.ctx.suspend();
        const dev = UTF8ToString(device);
        if (dev) {
            if (!a.ctx.setSinkId) throw new Error("this browser cannot choose the audio output");
            await a.ctx.setSinkId(dev);
            a.label = dev;
        }
        await a.ctx.audioWorklet.addModule(Module.mkAudioUrl);
        a.node = new AudioWorkletNode(a.ctx, "mk-spk", { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [channels] });
        a.node.connect(a.ctx.destination);
        a.node.port.onmessage = e => {
            a.played = e.data.played, a.t = e.data.t, a.idle = e.data.idle;
        };
        return 1;
    } catch (e) {
        a.error = (e && e.name ? e.name + ': ' : "") + (e && e.message ? e.message : String(e));
        Module.mkAudioClose(a);
        return 0;
    }
});

EM_JS(void, mk_js_spk_run, (int id, int run), {
    const a = Module.mkAudio[id];
    if (a && a.ctx) Module.mkAudioRun(a.ctx, !!run);
});

EM_JS(void, mk_js_spk_write, (int id, const int16_t* s, int count), {
    const a = Module.mkAudio[id];
    if (!a || !a.node) return;
    const buf = HEAP16.slice(s >> 1, (s >> 1) + count);
    a.node.port.postMessage({ s: buf }, [buf.buffer]);
    a.idle = false;
});

EM_JS(void, mk_js_spk_clear, (int id, double base), {
    const a = Module.mkAudio[id];
    if (!a || !a.node) return;
    a.node.port.postMessage({ clear: true, base: base });
    a.played = base, a.idle = true;
});

// Frames (counted from open) heard by now: what the worklet last reported, moved on by the time since,
// less the output latency. Can be fractional, or behind the last clear; C++ clamps it.
EM_JS(double, mk_js_spk_heard, (int id), {
    const a = Module.mkAudio[id];
    if (!a || !a.ctx) return 0;
    const r = a.ctx.sampleRate;
    let p = a.played;
    if (!a.idle && a.ctx.state == "running") p += Math.max(0, a.ctx.currentTime - a.t) * r;
    return p - ((a.ctx.outputLatency || 0) + (a.ctx.baseLatency || 0)) * r;
});

namespace mk {

namespace {

std::map<int, Camera::Impl*>& cameras() {
    static std::map<int, Camera::Impl*> m;
    return m;
}

std::string take(char* s) {
    std::string r = s ? s : "";
    std::free(s);
    return r;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream in(s);
    while (std::getline(in, item, sep)) out.push_back(item);
    return out;
}

// Same idea as TestSource (moving gradient with a white box), drawn straight into BGRA.
void draw_test_frame(VideoFrame& f, int64_t i) {
    for (int y = 0; y < f.height; ++y) {
        uint8_t* row = f.data[0] + (size_t)y * f.stride[0];
        for (int x = 0; x < f.width; ++x) {
            const uint8_t v = (uint8_t)(16 + ((x + y + i * 4) % 200));
            row[4 * x] = v, row[4 * x + 1] = (uint8_t)(v / 2 + 40), row[4 * x + 2] = (uint8_t)(255 - v), row[4 * x + 3] = 255;
        }
    }
    const int bs = f.height / 6, bx = (int)((i * 7) % std::max(1, f.width - bs)), by = f.height / 3;
    for (int y = by; y < by + bs && y < f.height; ++y) std::memset(f.data[0] + (size_t)y * f.stride[0] + (size_t)bx * 4, 255, (size_t)bs * 4);
}

std::vector<DeviceInfo> list_devices(const char* kind) {
    std::vector<DeviceInfo> out;
    for (const std::string& line : split(take(mk_js_list_devices(kind)), '\n')) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        DeviceInfo d;
        d.id = line.substr(0, tab);
        d.name = line.substr(tab + 1);
        out.push_back(d);
    }
    return out;
}

// Linear resampler for interleaved s16, keeping its phase across blocks (as the Linux speaker does).
struct Resampler {
    int from = 0;
    double pos = 1;
    std::vector<int16_t> last;

    void reset() { from = 0, last.clear(); }
    void run(const int16_t* x, int n, int ch, int in_rate, int out_rate, std::vector<int16_t>& out) {
        if (n <= 0) return;
        if (in_rate == out_rate) {
            out.insert(out.end(), x, x + (size_t)n * ch);
            return;
        }
        if (from != in_rate || last.size() != (size_t)ch) from = in_rate, pos = 1, last.assign(x, x + ch);
        const double step = (double)in_rate / out_rate;
        while (pos < n) {
            const int i0 = (int)pos;
            const double t = pos - i0;
            for (int c = 0; c < ch; ++c) {
                const double a = i0 == 0 ? last[c] : x[(size_t)(i0 - 1) * ch + c], b = x[(size_t)i0 * ch + c];
                out.push_back((int16_t)std::lround(a + (b - a) * t));
            }
            pos += step;
        }
        pos -= n;
        last.assign(x + (size_t)(n - 1) * ch, x + (size_t)n * ch);
    }
};

// Cameras, microphones and speakers share one id space: they share Module.mkCams / Module.mkAudio keys.
int next_device_id() {
    static int next = 1;
    return next++;
}

}  // namespace

struct Camera::Impl {
    int id = 0;
    bool test = false, opened = false, running = false;
    int width = 0, height = 0;
    double fps = 0;
    std::string name, err;
    std::function<void(const VideoFrame&)> cb;
    int64_t frames = 0;
};

}  // namespace mk

extern "C" EMSCRIPTEN_KEEPALIVE void mk_wasm_camera_frame(int id, const uint8_t* rgba, int w, int h) {
    auto it = mk::cameras().find(id);
    if (it == mk::cameras().end() || !it->second->running || !it->second->cb) return;
    mk::Camera::Impl& s = *it->second;
    mk::VideoFrame f;
    if (rgba) {
        f.alloc(w, h, mk::PixelFormat::BGRA);
        const size_t n = (size_t)w * h;
        uint8_t* d = f.data[0];
        for (size_t k = 0; k < n; ++k, rgba += 4, d += 4) d[0] = rgba[2], d[1] = rgba[1], d[2] = rgba[0], d[3] = 255;
        s.width = w, s.height = h;
    } else {
        f.alloc(s.width, s.height, mk::PixelFormat::BGRA);
        mk::draw_test_frame(f, s.frames);
    }
    f.pts_us = mk::now_us();
    ++s.frames;
    s.cb(f);
}

namespace mk {

std::vector<DeviceInfo> list_cameras() { return list_devices("videoinput"); }
std::vector<DeviceInfo> list_microphones() { return list_devices("audioinput"); }
std::vector<DeviceInfo> list_speakers() { return list_devices("audiooutput"); }

// Browsers do not list modes; ask for a size in CameraConfig and the camera picks the closest it has.
std::vector<CameraMode> list_camera_modes(const std::string& device_id) {
    if (device_id == "test") return {{1280, 720, 30, "BGRA"}, {640, 480, 30, "BGRA"}};
    return {};
}

Camera::Camera() : p_(new Impl) { p_->id = next_device_id(); }

Camera::~Camera() { close(); }

bool Camera::open(const CameraConfig& cfg) {
    close();
    Impl& s = *p_;
    s.err.clear();
    s.fps = cfg.fps > 0 ? cfg.fps : 30;
    s.test = cfg.device_id == "test";
    if (s.test) {
        s.width = cfg.width, s.height = cfg.height;
        s.name = "test pattern";
        s.opened = true;
        return true;
    }
    const int ok = mk_js_cam_open(s.id, cfg.device_id.c_str(), cfg.width, cfg.height);
    const std::vector<std::string> info = split(take(mk_js_cam_info(s.id)), '\t');
    if (!ok || info.size() < 3) {
        s.err = info.size() >= 3 ? info[2] : "camera open failed";
        return false;
    }
    s.width = std::atoi(info[0].c_str()), s.height = std::atoi(info[1].c_str());
    s.name = info[2];
    s.opened = true;
    return true;
}

bool Camera::start(std::function<void(const VideoFrame&)> on_frame) {
    Impl& s = *p_;
    if (!s.opened) {
        s.err = "not open";
        return false;
    }
    stop();
    s.cb = std::move(on_frame);
    s.running = true;
    cameras()[s.id] = &s;
    mk_js_cam_start(s.id, s.fps, s.test ? 1 : 0);
    return true;
}

void Camera::stop() {
    Impl& s = *p_;
    if (!s.running) return;
    s.running = false;
    mk_js_cam_stop(s.id);
    cameras().erase(s.id);
}

void Camera::close() {
    stop();
    if (p_->opened && !p_->test) mk_js_cam_close(p_->id);
    p_->opened = false;
}

int Camera::width() const { return p_->width; }
int Camera::height() const { return p_->height; }
double Camera::fps() const { return p_->fps; }
std::string Camera::format() const { return p_->test ? "BGRA" : "RGBA"; }
std::string Camera::name() const { return p_->name; }
const std::string& Camera::error() const { return p_->err; }

// ---------------------------------------------------------------- microphone

struct Microphone::Impl {
    int id = 0;
    MicrophoneConfig cfg;
    bool test = false, opened = false, running = false;
    int js_rate = 0;  // the rate the browser runs at (resampled to cfg.sample_rate)
    std::string name, err;
    std::function<void(const AudioFrame&)> cb;
    Resampler rs;
    int64_t test_n = 0;  // test tone: samples made so far
};

namespace {
std::map<int, Microphone::Impl*>& microphones() {
    static std::map<int, Microphone::Impl*> m;
    return m;
}
}  // namespace

}  // namespace mk

// A block of the microphone (s == null: the test tone is wanted); pts_ms is on the performance.now() clock.
extern "C" EMSCRIPTEN_KEEPALIVE void mk_wasm_mic_audio(int id, const int16_t* s, int frames, int rate, double pts_ms) {
    auto it = mk::microphones().find(id);
    if (it == mk::microphones().end() || !it->second->running || !it->second->cb) return;
    mk::Microphone::Impl& m = *it->second;
    const int ch = m.cfg.channels;
    mk::AudioFrame a;
    a.sample_rate = m.cfg.sample_rate, a.channels = ch;
    a.pts_us = (int64_t)(pts_ms * 1000);
    if (!s) {  // same tone as TestSource: 440 Hz with a click every second
        a.samples.resize((size_t)frames * ch);
        for (int k = 0; k < frames; ++k, ++m.test_n) {
            double v = 0.3 * std::sin(2 * 3.14159265358979 * 440 * (double)m.test_n / rate);
            if (m.test_n % rate < rate / 50) v += 0.5 * ((m.test_n / 20) % 2 ? 1 : -1);
            for (int c = 0; c < ch; ++c) a.samples[(size_t)k * ch + c] = (int16_t)(v * 32767 * 0.8);
        }
    } else {
        m.rs.run(s, frames, ch, rate, m.cfg.sample_rate, a.samples);
    }
    if (!a.samples.empty()) m.cb(a);
}

namespace mk {

Microphone::Microphone() : p_(new Impl) { p_->id = next_device_id(); }
Microphone::~Microphone() { close(); }

bool Microphone::open(const MicrophoneConfig& cfg) {
    close();
    Impl& s = *p_;
    s.cfg = cfg;
    if (s.cfg.sample_rate <= 0) s.cfg.sample_rate = 48000;
    if (s.cfg.channels <= 0) s.cfg.channels = 2;
    s.err.clear();
    s.test = cfg.device_id == "test";
    if (s.test) {
        s.name = "test tone";
        s.opened = true;
        return true;
    }
    mk_js_audio_init();
    s.js_rate = mk_js_mic_open(s.id, cfg.device_id.c_str(), s.cfg.sample_rate, s.cfg.channels);
    const std::string label = take(mk_js_audio_label(s.id));
    if (!s.js_rate) {
        s.err = label.empty() ? "microphone open failed" : label;
        mk_js_audio_close(s.id);
        return false;
    }
    s.name = label;
    s.opened = true;
    return true;
}

bool Microphone::start(std::function<void(const AudioFrame&)> on_audio) {
    Impl& s = *p_;
    if (!s.opened) {
        if (s.err.empty()) s.err = "microphone not open";
        return false;
    }
    stop();
    s.cb = std::move(on_audio);
    s.rs.reset();
    s.test_n = 0;
    s.running = true;
    microphones()[s.id] = &s;
    if (s.test) mk_js_audio_init();
    mk_js_mic_start(s.id, s.test ? 1 : 0, s.cfg.sample_rate);
    return true;
}

void Microphone::stop() {
    Impl& s = *p_;
    if (!s.running) return;
    s.running = false;
    mk_js_mic_stop(s.id);
    microphones().erase(s.id);
}

void Microphone::close() {
    stop();
    if (p_->opened) mk_js_audio_close(p_->id);
    p_->opened = false;
}

int Microphone::sample_rate() const { return p_->cfg.sample_rate; }
int Microphone::channels() const { return p_->cfg.channels; }
std::string Microphone::name() const { return p_->name; }
const std::string& Microphone::error() const { return p_->err; }

// ---------------------------------------------------------------- speaker

// The worklet counts the frames it has played since open; C++ maps that count back to media time with
// the start of every block written.
struct Speaker::Impl {
    int id = 0;
    SpeakerConfig cfg;
    bool opened = false, running = false;
    std::string err;
    Resampler rs;
    int64_t total = 0;  // frames handed to the worklet since open
    int64_t base = 0;   // total at the last clear(): the frames before it were dropped
    std::deque<std::pair<int64_t, int64_t>> blocks;  // (first frame, its pts_us) of each write since the clear

    int64_t heard() const {  // frames heard, in [base, total]
        const double h = opened ? mk_js_spk_heard(id) : 0;
        return std::max<int64_t>(base, std::min<int64_t>(total, (int64_t)h));
    }
};

Speaker::Speaker() : p_(new Impl) { p_->id = next_device_id(); }
Speaker::~Speaker() { close(); }

bool Speaker::open(const SpeakerConfig& cfg) {
    close();
    Impl& s = *p_;
    s.cfg = cfg;
    if (s.cfg.sample_rate <= 0) s.cfg.sample_rate = 48000;
    if (s.cfg.channels <= 0) s.cfg.channels = 2;
    s.err.clear();
    mk_js_audio_init();
    if (!mk_js_spk_open(s.id, cfg.device_id.c_str(), s.cfg.sample_rate, s.cfg.channels)) {
        s.err = take(mk_js_audio_label(s.id));
        mk_js_audio_close(s.id);
        return false;
    }
    s.opened = true;
    s.total = s.base = 0;
    s.blocks.clear();
    s.rs.reset();
    return true;
}

bool Speaker::start() {
    Impl& s = *p_;
    if (!s.opened) {
        if (s.err.empty()) s.err = "speaker not open";
        return false;
    }
    s.running = true;
    mk_js_spk_run(s.id, 1);
    return true;
}

void Speaker::write(const AudioFrame& f) {
    Impl& s = *p_;
    if (!s.opened || f.samples.empty() || f.channels <= 0) return;
    const int ch = s.cfg.channels, n = f.sample_count();
    std::vector<int16_t> mixed((size_t)n * ch), out;
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < ch; ++c) mixed[(size_t)i * ch + c] = f.samples[(size_t)i * f.channels + std::min(c, f.channels - 1)];
    s.rs.run(mixed.data(), n, ch, f.sample_rate, s.cfg.sample_rate, out);
    if (out.empty()) return;
    // keep only the blocks that can still be playing
    const int64_t h = s.heard();
    while (s.blocks.size() > 1 && s.blocks[1].first <= h) s.blocks.pop_front();
    s.blocks.push_back({s.total, f.pts_us});
    s.total += (int64_t)(out.size() / ch);
    mk_js_spk_write(s.id, out.data(), (int)out.size());
}

int64_t Speaker::played_pts_us() const {
    const Impl& s = *p_;
    if (s.blocks.empty()) return -1;
    const int64_t h = s.heard();
    if (h <= s.blocks.front().first) return -1;  // nothing heard yet
    auto it = std::upper_bound(s.blocks.begin(), s.blocks.end(), h,
                               [](int64_t v, const std::pair<int64_t, int64_t>& b) { return v < b.first; });
    --it;
    return it->second + (h - it->first) * 1000000 / s.cfg.sample_rate;
}

int64_t Speaker::queued_us() const {
    const Impl& s = *p_;
    return s.opened ? (s.total - s.heard()) * 1000000 / s.cfg.sample_rate : 0;
}

void Speaker::clear() {
    Impl& s = *p_;
    if (!s.opened) return;
    s.base = s.total;
    s.blocks.clear();
    s.rs.reset();
    mk_js_spk_clear(s.id, (double)s.base);
}

void Speaker::stop() {
    Impl& s = *p_;
    if (!s.running) return;
    s.running = false;
    mk_js_spk_run(s.id, 0);
}

void Speaker::close() {
    stop();
    if (p_->opened) mk_js_audio_close(p_->id);
    p_->opened = false;
    p_->blocks.clear();
}

const std::string& Speaker::error() const { return p_->err; }

}  // namespace mk
