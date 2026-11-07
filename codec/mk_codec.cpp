// mediakit codec layer on FFmpeg (LGPL build: no x264, no GPL parts).
#include "mk_codec.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/channel_layout.h"
#include "libavutil/hwcontext.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
#ifdef __ANDROID__
#include "libavcodec/jni_avcodec.h"
#endif
}
#ifdef __EMSCRIPTEN__
#include "wasm/mk_webcodecs.h"
#endif

namespace mk {

// MK_FF_LOG=debug / verbose / trace turns FFmpeg's own logging up; handy when a platform codec
// misbehaves. Applied the first time anything in the codec layer is opened.
void apply_log_level() {
    static const bool once = [] {
        const char* v = getenv("MK_FF_LOG");
        if (!v) return true;
        if (!strcmp(v, "trace")) av_log_set_level(AV_LOG_TRACE);
        else if (!strcmp(v, "debug")) av_log_set_level(AV_LOG_DEBUG);
        else if (!strcmp(v, "verbose")) av_log_set_level(AV_LOG_VERBOSE);
        else if (!strcmp(v, "quiet")) av_log_set_level(AV_LOG_QUIET);
        return true;
    }();
    (void)once;
}

bool set_java_vm(void* vm) {
#ifdef __ANDROID__
    return av_jni_set_java_vm(vm, nullptr) == 0;
#else
    (void)vm;
    return false;
#endif
}

namespace {

const AVRational kUs = {1, 1000000};

std::string av_err(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

AVPixelFormat to_av(PixelFormat f) {
    switch (f) {
        case PixelFormat::NV12: return AV_PIX_FMT_NV12;
        case PixelFormat::I420: return AV_PIX_FMT_YUV420P;
        case PixelFormat::YUY2: return AV_PIX_FMT_YUYV422;
        case PixelFormat::BGRA: return AV_PIX_FMT_BGRA;
        case PixelFormat::RGB24: return AV_PIX_FMT_RGB24;
        case PixelFormat::MJPEG: return AV_PIX_FMT_NONE;
    }
    return AV_PIX_FMT_NONE;
}

bool from_av(int f, PixelFormat& out) {
    switch (f) {
        case AV_PIX_FMT_NV12: out = PixelFormat::NV12; return true;
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P: out = PixelFormat::I420; return true;
        case AV_PIX_FMT_YUYV422: out = PixelFormat::YUY2; return true;
        case AV_PIX_FMT_BGRA: out = PixelFormat::BGRA; return true;
        case AV_PIX_FMT_RGB24: out = PixelFormat::RGB24; return true;
        default: return false;
    }
}

int plane_count(PixelFormat f) {
    return f == PixelFormat::I420 ? 3 : f == PixelFormat::NV12 ? 2 : 1;
}

// Iterate the NAL units of an Annex-B buffer: fn(nal_start, nal_size) with the start code stripped.
template <class F>
void for_each_nal(const uint8_t* d, size_t n, F fn) {
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && d[end - 1] == 0) --end;  // trailing zero of a 4-byte start code
                fn(d + start, end - start);
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX && start < n) fn(d + start, n - start);
}

// Copy an AVFrame (video) into a VideoFrame.
bool frame_from_av(const AVFrame* f, VideoFrame& out, std::string& err) {
    PixelFormat pf;
    if (!from_av(f->format, pf)) {
        err = std::string("unsupported decoded pixel format ") + av_get_pix_fmt_name((AVPixelFormat)f->format);
        return false;
    }
    out.alloc(f->width, f->height, pf);
    const int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    switch (pf) {
        case PixelFormat::I420:
            out.copy_plane(0, f->data[0], f->linesize[0], f->width, f->height);
            out.copy_plane(1, f->data[1], f->linesize[1], cw, ch);
            out.copy_plane(2, f->data[2], f->linesize[2], cw, ch);
            break;
        case PixelFormat::NV12:
            out.copy_plane(0, f->data[0], f->linesize[0], f->width, f->height);
            out.copy_plane(1, f->data[1], f->linesize[1], cw * 2, ch);
            break;
        default: out.copy_plane(0, f->data[0], f->linesize[0], out.stride[0], f->height); break;
    }
    return true;
}

}  // namespace

std::vector<uint8_t> h264_extract_parameter_sets(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    for_each_nal(data, size, [&](const uint8_t* nal, size_t n) {
        int type = n ? (nal[0] & 0x1f) : 0;
        if (type == 7 || type == 8) {
            static const uint8_t sc[4] = {0, 0, 0, 1};
            out.insert(out.end(), sc, sc + 4);
            out.insert(out.end(), nal, nal + n);
        }
    });
    return out;
}

bool h264_is_keyframe(const uint8_t* data, size_t size) {
    bool key = false;
    for_each_nal(data, size, [&](const uint8_t* nal, size_t n) {
        if (n && (nal[0] & 0x1f) == 5) key = true;
    });
    return key;
}

// ---------------------------------------------------------------- FrameConverter

struct FrameConverter::Impl {
    SwsContext* sws = nullptr;
    int sw = 0, sh = 0, dw = 0, dh = 0;
    AVPixelFormat sf = AV_PIX_FMT_NONE, df = AV_PIX_FMT_NONE;
    // MJPEG input
    AVCodecContext* jpeg = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    VideoFrame decoded;
    ~Impl() {
        sws_freeContext(sws);
        avcodec_free_context(&jpeg);
        av_packet_free(&pkt);
        av_frame_free(&frame);
    }
    bool decode_jpeg(const VideoFrame& in) {
        if (!jpeg) {
            const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
            if (!c) return false;
            jpeg = avcodec_alloc_context3(c);
            jpeg->thread_count = 1;
            if (avcodec_open2(jpeg, c, nullptr) < 0) return false;
            pkt = av_packet_alloc();
            frame = av_frame_alloc();
        }
        av_new_packet(pkt, in.stride[0]);
        memcpy(pkt->data, in.data[0], in.stride[0]);
        int r = avcodec_send_packet(jpeg, pkt);
        av_packet_unref(pkt);
        if (r < 0 || avcodec_receive_frame(jpeg, frame) < 0) return false;
        // JPEG is 4:2:2 or 4:2:0 full range: hand it to swscale as it is
        decoded.width = frame->width, decoded.height = frame->height;
        bool ok = true;
        std::string e;
        if (frame->format == AV_PIX_FMT_YUVJ420P || frame->format == AV_PIX_FMT_YUV420P) ok = frame_from_av(frame, decoded, e);
        else ok = false;
        if (!ok) {
            // other JPEG layouts (4:2:2 ...): convert to I420 directly from the AVFrame
            SwsContext* c = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format, frame->width, frame->height,
                                           AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
            if (!c) return false;
            decoded.alloc(frame->width & ~1, frame->height & ~1, PixelFormat::I420);
            uint8_t* dst[4] = {decoded.data[0], decoded.data[1], decoded.data[2], nullptr};
            int ds[4] = {decoded.stride[0], decoded.stride[1], decoded.stride[2], 0};
            sws_scale(c, frame->data, frame->linesize, 0, frame->height, dst, ds);
            sws_freeContext(c);
        }
        decoded.pts_us = in.pts_us;
        av_frame_unref(frame);
        return true;
    }
};

FrameConverter::FrameConverter() : p_(new Impl) {}
FrameConverter::~FrameConverter() = default;

bool FrameConverter::convert(const VideoFrame& in_frame, VideoFrame& out, PixelFormat format, int width, int height) {
    if (in_frame.empty()) return false;
    if (format == PixelFormat::MJPEG) return false;  // no JPEG encoding here
    const VideoFrame* srcf = &in_frame;
    if (in_frame.format == PixelFormat::MJPEG) {
        if (!p_->decode_jpeg(in_frame)) return false;
        srcf = &p_->decoded;
    }
    const VideoFrame& in = *srcf;
    const int dw = width > 0 ? width : in.width, dh = height > 0 ? height : in.height;
    const AVPixelFormat sf = to_av(in.format), df = to_av(format);
    if (!p_->sws || p_->sw != in.width || p_->sh != in.height || p_->sf != sf || p_->dw != dw || p_->dh != dh || p_->df != df) {
        sws_freeContext(p_->sws);
        // Same size: fast point sampling is exact; scaling: bilinear.
        int flags = (dw == in.width && dh == in.height) ? SWS_POINT : SWS_BILINEAR;
        p_->sws = sws_getContext(in.width, in.height, sf, dw, dh, df, flags | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
        p_->sw = in.width, p_->sh = in.height, p_->sf = sf, p_->dw = dw, p_->dh = dh, p_->df = df;
        if (!p_->sws) return false;
    }
    out.alloc(dw, dh, format);
    out.pts_us = in.pts_us;
    const uint8_t* src[4] = {in.data[0], in.data[1], in.data[2], nullptr};
    int sst[4] = {in.stride[0], in.stride[1], in.stride[2], 0};
    uint8_t* dst[4] = {out.data[0], out.data[1], out.data[2], nullptr};
    int dstr[4] = {out.stride[0], out.stride[1], out.stride[2], 0};
    sws_scale(p_->sws, src, sst, 0, in.height, dst, dstr);
    return true;
}

// ---------------------------------------------------------------- VideoEncoder

struct VideoEncoder::Impl {
    VideoEncoderConfig cfg;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    FrameConverter conv;
    VideoFrame tmp;
    std::vector<uint8_t> extradata;
    std::string name, err;
    bool force_key = false;
    int64_t last_pts = INT64_MIN;
    int wc = 0;          // browser: WebCodecs encoder id (0: FFmpeg)
    int64_t frames = 0;  // given to the WebCodecs encoder (it does not place keyframes by itself)

    void close() {
        avcodec_free_context(&ctx);
        av_frame_free(&frame);
        av_packet_free(&pkt);
#ifdef __EMSCRIPTEN__
        if (wc) mk_wc_close(wc), wc = 0;
#endif
    }

    // Keyframes carry SPS/PPS: learn them, or put them in front when the encoder keeps them aside.
    void add_packet(Packet&& p, std::vector<Packet>& out) {
        if (p.keyframe) {
            std::vector<uint8_t> ps = h264_extract_parameter_sets(p.data.data(), p.data.size());
            if (ps.empty() && !extradata.empty()) {
                // Some encoders keep SPS/PPS only in extradata: put them in front of every keyframe
                // so each packet stream is decodable on its own (streaming, file cut points).
                p.data.insert(p.data.begin(), extradata.begin(), extradata.end());
            } else if (extradata.empty()) {
                extradata = ps;
            }
        }
        out.push_back(std::move(p));
    }

#ifdef __EMSCRIPTEN__
    bool try_open_webcodecs() {
        static int next_id = 1;
        wc = next_id++;
        if (!mk_wc_venc_open(wc, cfg.width, cfg.height, cfg.fps, cfg.bitrate, cfg.hardware ? 1 : 0)) {
            err = "webcodecs: " + take_js(mk_wc_error(wc));
            mk_wc_close(wc), wc = 0;
            return false;
        }
        name = take_js(mk_wc_name(wc));
        frames = 0;
        return true;
    }
    static std::string take_js(char* s) {
        std::string r = s ? s : "";
        free(s);
        return r;
    }
    void receive_webcodecs(std::vector<Packet>& out) {
        int size = 0, key = 0;
        double ts = 0;
        while (uint8_t* d = mk_wc_venc_next(wc, &size, &ts, &key)) {
            Packet p;
            p.type = MediaType::Video;
            p.data.assign(d, d + size);
            free(d);
            p.pts_us = p.dts_us = (int64_t)ts;  // no B-frames in realtime mode
            p.keyframe = key || h264_is_keyframe(p.data.data(), p.data.size());
            add_packet(std::move(p), out);
        }
    }
#endif

    bool try_open(const char* enc_name) {
        const AVCodec* codec = avcodec_find_encoder_by_name(enc_name);
        if (!codec) return false;
        ctx = avcodec_alloc_context3(codec);
        ctx->width = cfg.width;
        ctx->height = cfg.height;
        ctx->time_base = kUs;
        ctx->framerate = {cfg.fps, 1};
        ctx->bit_rate = cfg.bitrate;
        ctx->gop_size = cfg.gop;
        ctx->max_b_frames = 0;  // low latency, pts == dts
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        if (codec->pix_fmts) {
            bool nv12 = false;
            for (const AVPixelFormat* f = codec->pix_fmts; *f != AV_PIX_FMT_NONE; ++f) nv12 = nv12 || *f == AV_PIX_FMT_NV12;
            if (!nv12) ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        }
        // SPS/PPS into extradata (MP4 needs them). Not for MediaCodec: the flag makes FFmpeg put an
        // extract_extradata bitstream filter in front of the encoder, and no packet ever comes out
        // of it. MediaCodec prepends its codec-config buffer to the first packet anyway, so the
        // parameter sets arrive in band and Mp4Writer learns them from the first keyframe.
        if (strcmp(enc_name, "h264_mediacodec")) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* opts = nullptr;
        if (!strcmp(enc_name, "h264_mf")) {
            av_dict_set(&opts, "hw_encoding", cfg.hardware ? "1" : "0", 0);
            av_dict_set(&opts, "rate_control", "cbr", 0);
            av_dict_set(&opts, "scenario", "camera_record", 0);
        } else if (!strcmp(enc_name, "h264_videotoolbox")) {
            av_dict_set(&opts, "realtime", "1", 0);
        } else if (!strcmp(enc_name, "libx264")) {
            av_dict_set(&opts, "preset", "veryfast", 0);
            av_dict_set(&opts, "tune", "zerolatency", 0);
            av_dict_set(&opts, "forced-idr", "1", 0);
        } else if (!strcmp(enc_name, "libopenh264")) {
            av_dict_set(&opts, "allow_skip_frames", "0", 0);
        }
        int r = avcodec_open2(ctx, codec, &opts);
        av_dict_free(&opts);
        if (r < 0) {
            err = std::string(enc_name) + ": " + av_err(r);
            avcodec_free_context(&ctx);
            return false;
        }
        name = enc_name;
        if (ctx->extradata_size > 0) {
            extradata.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
            if (!extradata.empty() && extradata[0] == 1) extradata.clear();  // avcC form: learn from the first keyframe instead
        }
        return true;
    }

    bool receive(std::vector<Packet>& out) {
        for (;;) {
            int r = avcodec_receive_packet(ctx, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) {
                err = "encode: " + av_err(r);
                return false;
            }
            Packet p;
            p.type = MediaType::Video;
            p.data.assign(pkt->data, pkt->data + pkt->size);
            p.pts_us = av_rescale_q(pkt->pts, ctx->time_base, kUs);
            p.dts_us = pkt->dts == AV_NOPTS_VALUE ? p.pts_us : av_rescale_q(pkt->dts, ctx->time_base, kUs);
            p.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) || h264_is_keyframe(p.data.data(), p.data.size());
            av_packet_unref(pkt);
            add_packet(std::move(p), out);
        }
    }
};

VideoEncoder::VideoEncoder() : p_(new Impl) {}
VideoEncoder::~VideoEncoder() { p_->close(); }

bool VideoEncoder::open(const VideoEncoderConfig& cfg) {
    apply_log_level();
    p_->close();
    p_->cfg = cfg;
    p_->err.clear();
    std::vector<std::string> names;
    if (cfg.encoder != "auto") {
        names.push_back(cfg.encoder);
    } else {
#if defined(_WIN32)
        names = {"h264_mf"};
#elif defined(__ANDROID__)
        names = {"h264_mediacodec"};
#elif defined(__APPLE__)
        names = {"h264_videotoolbox"};
#elif defined(__EMSCRIPTEN__)
        names = {"webcodecs"};  // the browser's encoder (hardware when it has one)
#else
        names = {"h264_vaapi", "h264_v4l2m2m"};
#endif
        // software: x264 (when built in, GPL) is the best one; asked for software -> try it first
        if (cfg.hardware) names.push_back("libx264");
        else names.insert(names.begin(), "libx264");
        names.push_back("libopenh264");
    }
    std::string errs;
    for (auto& n : names) {
#ifdef __EMSCRIPTEN__
        if (n == "webcodecs") {
            if (p_->try_open_webcodecs()) return true;
            errs += p_->err + "; ";
            p_->err.clear();
            continue;
        }
#endif
        if (p_->try_open(n.c_str())) break;
        if (n == "h264_mf" && cfg.hardware) {  // no hardware MFT on this machine: Microsoft's software one
            p_->cfg.hardware = false;
            if (p_->try_open(n.c_str())) break;
            p_->cfg.hardware = true;
        }
        if (!p_->err.empty()) errs += p_->err + "; ";
        else errs += n + ": not built in; ";
        p_->err.clear();
    }
    if (!p_->ctx) {
        p_->err = "no H.264 encoder could be opened (" + errs + ")";
        return false;
    }
    p_->err.clear();
    p_->frame = av_frame_alloc();
    p_->frame->format = p_->ctx->pix_fmt;
    p_->frame->width = cfg.width;
    p_->frame->height = cfg.height;
    p_->pkt = av_packet_alloc();
    return true;
}

bool VideoEncoder::encode(const VideoFrame& in, std::vector<Packet>& out) {
#ifdef __EMSCRIPTEN__
    if (p_->wc) {
        Impl& s = *p_;
        s.err.clear();
        const VideoFrame* src = &in;
        const bool direct = in.width == s.cfg.width && in.height == s.cfg.height &&
                            (in.format == PixelFormat::I420 || in.format == PixelFormat::NV12 || in.format == PixelFormat::BGRA);
        if (!direct) {
            if (!s.conv.convert(in, s.tmp, PixelFormat::I420, s.cfg.width, s.cfg.height)) {
                s.err = "pixel conversion failed";
                return false;
            }
            src = &s.tmp;
        }
        int64_t pts = in.pts_us;
        if (pts <= s.last_pts) pts = s.last_pts + 1;
        s.last_pts = pts;
        const bool key = s.force_key || s.frames % std::max(1, s.cfg.gop) == 0;
        const int fmt = src->format == PixelFormat::I420 ? 0 : src->format == PixelFormat::NV12 ? 1 : 2;
        const int r = mk_wc_venc_encode(s.wc, fmt, src->width, src->height, src->data[0], src->stride[0], src->data[1], src->stride[1],
                                        src->data[2], src->stride[2], (double)pts, key ? 1 : 0);
        if (r < 0) {
            s.err = "webcodecs: " + Impl::take_js(mk_wc_error(s.wc));
            return false;
        }
        if (r == 0) {
            s.err = "encoder stayed full; frame dropped";  // as below: the caller keeps going
        } else {
            ++s.frames;
            if (key) s.force_key = false;
        }
        s.receive_webcodecs(out);
        return true;
    }
#endif
    if (!p_->ctx) return false;
    PixelFormat want;
    from_av(p_->ctx->pix_fmt, want);
    const VideoFrame* src = &in;
    if (in.format != want || in.width != p_->cfg.width || in.height != p_->cfg.height) {
        if (!p_->conv.convert(in, p_->tmp, want, p_->cfg.width, p_->cfg.height)) {
            p_->err = "pixel conversion failed";
            return false;
        }
        src = &p_->tmp;
    }
    AVFrame* f = p_->frame;
    for (int i = 0; i < 3; ++i) {
        f->data[i] = src->data[i];
        f->linesize[i] = src->stride[i];
    }
    int64_t pts = in.pts_us;
    if (pts <= p_->last_pts) pts = p_->last_pts + 1;  // encoders reject non-increasing pts
    p_->last_pts = pts;
    f->pts = pts;
    f->pict_type = p_->force_key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (p_->force_key) f->flags |= AV_FRAME_FLAG_KEY;
    else f->flags &= ~AV_FRAME_FLAG_KEY;
    p_->force_key = false;
    // A full encoder answers EAGAIN: drain it and offer the frame again. MediaCodec keeps a deep
    // pipeline and says EAGAIN often, so one retry is not enough -- a single retry silently lost
    // a third of the frames on Android.
    bool ok = true;
    int r = AVERROR(EAGAIN);
    int attempt = 0;
    for (; attempt < 64; ++attempt) {
        r = avcodec_send_frame(p_->ctx, f);
        if (r != AVERROR(EAGAIN)) break;
        if (!p_->receive(out)) return false;
    }
    if (r == AVERROR(EAGAIN)) {
        p_->err = "encoder stayed full; frame dropped";
        return true;  // the caller keeps going; the recorder counts it as a drop
    }
    if (r < 0) {
        p_->err = "send_frame: " + av_err(r);
        return false;
    }
    ok = p_->receive(out);
    return ok;
}

bool VideoEncoder::flush(std::vector<Packet>& out) {
#ifdef __EMSCRIPTEN__
    if (p_->wc) {  // waits for the browser (ASYNCIFY): call it from the program's flow, not from a device callback
        const bool ok = mk_wc_venc_flush(p_->wc) != 0;
        if (!ok) p_->err = "webcodecs: " + Impl::take_js(mk_wc_error(p_->wc));
        p_->receive_webcodecs(out);
        return ok;
    }
#endif
    if (!p_->ctx) return false;
    avcodec_send_frame(p_->ctx, nullptr);
    return p_->receive(out);
}

void VideoEncoder::request_keyframe() { p_->force_key = true; }
void VideoEncoder::close() { p_->close(); }
const VideoEncoderConfig& VideoEncoder::config() const { return p_->cfg; }
std::string VideoEncoder::name() const { return p_->name; }
const std::vector<uint8_t>& VideoEncoder::extradata() const { return p_->extradata; }
const std::string& VideoEncoder::error() const { return p_->err; }

// ---------------------------------------------------------------- AudioEncoder

struct AudioEncoder::Impl {
    AudioEncoderConfig cfg;
    AVCodecContext* ctx = nullptr;
    SwrContext* swr = nullptr;
    int swr_rate = 0, swr_ch = 0;
    AVAudioFifo* fifo = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    std::vector<uint8_t> extradata;
    std::string err;
    int64_t next_pts = INT64_MIN;  // in samples at cfg.sample_rate
    int64_t first_us = 0;
    int64_t in_samples = 0;        // input samples consumed (incl. inserted silence), at the input rate

    void close() {
        avcodec_free_context(&ctx);
        swr_free(&swr);
        if (fifo) av_audio_fifo_free(fifo), fifo = nullptr;
        av_frame_free(&frame);
        av_packet_free(&pkt);
    }

    bool receive(std::vector<Packet>& out) {
        for (;;) {
            int r = avcodec_receive_packet(ctx, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) {
                err = "aac encode: " + av_err(r);
                return false;
            }
            Packet p;
            p.type = MediaType::Audio;
            p.data.assign(pkt->data, pkt->data + pkt->size);
            p.pts_us = av_rescale_q(pkt->pts, ctx->time_base, kUs);
            p.dts_us = av_rescale_q(pkt->dts, ctx->time_base, kUs);
            p.keyframe = true;
            av_packet_unref(pkt);
            out.push_back(std::move(p));
        }
    }

    bool send_from_fifo(std::vector<Packet>& out, bool final) {
        const int fs = ctx->frame_size;
        while (av_audio_fifo_size(fifo) >= fs || (final && av_audio_fifo_size(fifo) > 0)) {
            int n = std::min(fs, av_audio_fifo_size(fifo));
            av_frame_unref(frame);
            frame->format = ctx->sample_fmt;
            av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
            frame->sample_rate = ctx->sample_rate;
            frame->nb_samples = n;
            if (av_frame_get_buffer(frame, 0) < 0) return false;
            av_audio_fifo_read(fifo, (void**)frame->data, n);
            frame->pts = next_pts;
            next_pts += n;
            int r = avcodec_send_frame(ctx, frame);
            if (r < 0) {
                err = "aac send_frame: " + av_err(r);
                return false;
            }
            if (!receive(out)) return false;
        }
        return true;
    }
};

AudioEncoder::AudioEncoder() : p_(new Impl) {}
AudioEncoder::~AudioEncoder() { p_->close(); }

bool AudioEncoder::open(const AudioEncoderConfig& cfg) {
    p_->close();
    p_->cfg = cfg;
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        p_->err = "AAC encoder not built in";
        return false;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->sample_rate = cfg.sample_rate;
    av_channel_layout_default(&c->ch_layout, cfg.channels);
    c->sample_fmt = AV_SAMPLE_FMT_FLTP;
    c->bit_rate = cfg.bitrate;
    c->time_base = {1, cfg.sample_rate};
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int r = avcodec_open2(c, codec, nullptr);
    if (r < 0) {
        p_->err = "aac open: " + av_err(r);
        avcodec_free_context(&c);
        return false;
    }
    p_->ctx = c;
    if (c->extradata_size > 0) p_->extradata.assign(c->extradata, c->extradata + c->extradata_size);
    p_->fifo = av_audio_fifo_alloc(c->sample_fmt, cfg.channels, c->frame_size * 4);
    p_->frame = av_frame_alloc();
    p_->pkt = av_packet_alloc();
    p_->next_pts = INT64_MIN;
    return true;
}

bool AudioEncoder::encode(const AudioFrame& in, std::vector<Packet>& out) {
    Impl& s = *p_;
    if (!s.ctx || in.channels <= 0 || in.samples.empty()) return s.ctx != nullptr;
    if (!s.swr || s.swr_rate != in.sample_rate || s.swr_ch != in.channels) {
        swr_free(&s.swr);
        AVChannelLayout inl;
        av_channel_layout_default(&inl, in.channels);
        if (swr_alloc_set_opts2(&s.swr, &s.ctx->ch_layout, s.ctx->sample_fmt, s.ctx->sample_rate, &inl, AV_SAMPLE_FMT_S16,
                                in.sample_rate, 0, nullptr) < 0 ||
            swr_init(s.swr) < 0) {
            s.err = "audio resampler init failed";
            return false;
        }
        s.swr_rate = in.sample_rate, s.swr_ch = in.channels;
    }
    if (s.next_pts == INT64_MIN) {
        s.first_us = in.pts_us;
        s.in_samples = 0;
        s.next_pts = av_rescale_q(in.pts_us, kUs, s.ctx->time_base);
    }
    // AAC timestamps count samples. If the source skipped (a USB microphone dropping data,
    // a stalled device), fill the hole with silence so the audio stays on the video's timeline.
    const int64_t expected_us = s.first_us + s.in_samples * 1000000 / in.sample_rate;
    int pad = 0;
    if (in.pts_us - expected_us > 30000) pad = (int)std::min<int64_t>((in.pts_us - expected_us) * in.sample_rate / 1000000, in.sample_rate * 10);
    std::vector<int16_t> padded;
    const int16_t* src = in.samples.data();
    if (pad > 0) {
        padded.assign((size_t)pad * in.channels, 0);
        padded.insert(padded.end(), in.samples.begin(), in.samples.end());
        src = padded.data();
    }
    const int in_n = in.sample_count() + pad;
    s.in_samples += in_n;
    const int max_out = (int)swr_get_out_samples(s.swr, in_n) + 16;
    std::vector<float> planes((size_t)max_out * s.cfg.channels);
    uint8_t* outp[8] = {nullptr};
    for (int c = 0; c < s.cfg.channels && c < 8; ++c) outp[c] = (uint8_t*)(planes.data() + (size_t)c * max_out);
    const uint8_t* inp[1] = {(const uint8_t*)src};
    int got = swr_convert(s.swr, outp, max_out, inp, in_n);
    if (got < 0) {
        s.err = "audio resample failed";
        return false;
    }
    if (got > 0) av_audio_fifo_write(s.fifo, (void**)outp, got);
    return s.send_from_fifo(out, false);
}

bool AudioEncoder::flush(std::vector<Packet>& out) {
    if (!p_->ctx) return false;
    if (p_->next_pts != INT64_MIN && !p_->send_from_fifo(out, true)) return false;
    avcodec_send_frame(p_->ctx, nullptr);
    return p_->receive(out);
}

void AudioEncoder::close() { p_->close(); }
const AudioEncoderConfig& AudioEncoder::config() const { return p_->cfg; }
const std::vector<uint8_t>& AudioEncoder::extradata() const { return p_->extradata; }
const std::string& AudioEncoder::error() const { return p_->err; }

// ---------------------------------------------------------------- VideoDecoder

struct VideoDecoder::Impl {
    AVCodecContext* ctx = nullptr;
    bool want_mediacodec = false;  // Android: swap the software decoder for MediaCodec at the first keyframe
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* sw = nullptr;           // hardware frames are copied down into this
    AVBufferRef* hw_dev = nullptr;
    AVPixelFormat hw_fmt = AV_PIX_FMT_NONE;
    std::string err, name;
    int wc = 0;               // browser: WebCodecs decoder id (0: FFmpeg)
    bool wait_key = false;    // switched to software mid-stream: skip to the next keyframe
    void close() {
        avcodec_free_context(&ctx);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        av_frame_free(&sw);
        av_buffer_unref(&hw_dev);
        hw_fmt = AV_PIX_FMT_NONE;
#ifdef __EMSCRIPTEN__
        if (wc) mk_wc_close(wc), wc = 0;
#endif
    }

#ifdef __EMSCRIPTEN__
    // "avc1.PPCCLL" from the SPS of a keyframe (profile, constraint flags, level).
    static std::string codec_string(const Packet& in) {
        char b[32] = "avc1.42e01f";
        for_each_nal(in.data.data(), in.data.size(), [&](const uint8_t* nal, size_t n) {
            if (n >= 4 && (nal[0] & 0x1f) == 7) std::snprintf(b, sizeof(b), "avc1.%02x%02x%02x", nal[1], nal[2], nal[3]);
        });
        return b;
    }
    void receive_webcodecs(std::vector<VideoFrame>& out) {
        int info[9];
        double ts = 0;
        while (uint8_t* d = mk_wc_vdec_next(wc, info, &ts)) {
            const int fmt = info[0], w = info[1], h = info[2], cw = (w + 1) / 2, ch = (h + 1) / 2;
            VideoFrame vf;
            if (fmt == 0) {
                vf.alloc(w, h, PixelFormat::I420);
                vf.copy_plane(0, d + info[3], info[4], w, h);
                vf.copy_plane(1, d + info[5], info[6], cw, ch);
                vf.copy_plane(2, d + info[7], info[8], cw, ch);
            } else if (fmt == 1) {
                vf.alloc(w, h, PixelFormat::NV12);
                vf.copy_plane(0, d + info[3], info[4], w, h);
                vf.copy_plane(1, d + info[5], info[6], cw * 2, ch);
            } else {  // RGBA -> BGRA
                vf.alloc(w, h, PixelFormat::BGRA);
                for (int y = 0; y < h; ++y) {
                    const uint8_t* s = d + info[3] + (size_t)y * info[4];
                    uint8_t* t = vf.data[0] + (size_t)y * vf.stride[0];
                    for (int x = 0; x < w; ++x, s += 4, t += 4) t[0] = s[2], t[1] = s[1], t[2] = s[0], t[3] = 255;
                }
            }
            free(d);
            vf.pts_us = (int64_t)ts;
            out.push_back(std::move(vf));
        }
    }
    // The browser's decoder failed (e.g. a profile it does not take): go on in software from the next keyframe.
    void fall_back_to_software() {
        char* e = mk_wc_error(wc);
        err = std::string("webcodecs: ") + (e ? e : "") + " (decoding in software)";
        free(e);
        mk_wc_close(wc), wc = 0;
        name = "software";
        wait_key = true;
    }
#endif
    // Offered formats: take the hardware one; else the first software one (the decoder fell back).
    static AVPixelFormat pick_format(AVCodecContext* c, const AVPixelFormat* fmts) {
        Impl* s = (Impl*)c->opaque;
        for (const AVPixelFormat* f = fmts; *f != AV_PIX_FMT_NONE; ++f)
            if (*f == s->hw_fmt) return *f;
        for (const AVPixelFormat* f = fmts; *f != AV_PIX_FMT_NONE; ++f)
            if (!(av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *f;
        return AV_PIX_FMT_NONE;
    }
    bool setup_hw(const AVCodec* codec) {
#if defined(_WIN32)
        const AVHWDeviceType type = AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__APPLE__)
        const AVHWDeviceType type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(__ANDROID__)
        const AVHWDeviceType type = AV_HWDEVICE_TYPE_MEDIACODEC;
#else
        const AVHWDeviceType type = AV_HWDEVICE_TYPE_VAAPI;
#endif
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* hc = avcodec_get_hw_config(codec, i);
            if (!hc) return false;  // not built in for this OS
            if ((hc->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && hc->device_type == type) {
                hw_fmt = hc->pix_fmt;
                break;
            }
        }
        if (av_hwdevice_ctx_create(&hw_dev, type, nullptr, nullptr, 0) < 0) return false;  // no GPU / driver
        ctx->hw_device_ctx = av_buffer_ref(hw_dev);
        ctx->opaque = this;
        ctx->get_format = pick_format;
        name = av_hwdevice_get_type_name(type);
        return true;
    }
    // Android: the first keyframe carries the SPS/PPS and the size MediaCodec needs. Swap the
    // software decoder for it; if anything goes wrong, keep decoding in software.
    void try_mediacodec(const Packet& in) {
        want_mediacodec = false;
        std::vector<uint8_t> ps = h264_extract_parameter_sets(in.data.data(), in.data.size());
        const AVCodec* codec = avcodec_find_decoder_by_name("h264_mediacodec");
        if (ps.empty() || !codec) {
            name = "software";
            return;
        }
        int w = 0, h = 0;
        AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
        if (parser) {
            // without this the parser waits for the next access unit before it reports the size
            parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
            AVCodecContext* probe = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
            uint8_t* o = nullptr;
            int on = 0;
            av_parser_parse2(parser, probe, &o, &on, in.data.data(), (int)in.data.size(), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            w = parser->width, h = parser->height;
            avcodec_free_context(&probe);
            av_parser_close(parser);
        }
        if (w <= 0 || h <= 0) {
            name = "software";
            return;
        }
        AVCodecContext* c = avcodec_alloc_context3(codec);
        c->pkt_timebase = kUs;
        c->width = w, c->height = h;
        c->extradata = (uint8_t*)av_mallocz(ps.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!c->extradata) {
            avcodec_free_context(&c);
            name = "software";
            return;
        }
        memcpy(c->extradata, ps.data(), ps.size());
        c->extradata_size = (int)ps.size();
        if (avcodec_open2(c, codec, nullptr) < 0) {  // no MediaCodec for H.264 on this device
            avcodec_free_context(&c);
            name = "software";
            return;
        }
        avcodec_free_context(&ctx);
        ctx = c;
        name = "mediacodec";
    }

    bool receive(std::vector<VideoFrame>& out) {
        for (;;) {
            int r = avcodec_receive_frame(ctx, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) {
                err = "decode: " + av_err(r);
                return false;
            }
            VideoFrame vf;
            const AVFrame* src = frame;
            if (frame->format == hw_fmt && hw_fmt != AV_PIX_FMT_NONE) {
                if (!sw) sw = av_frame_alloc();
                av_frame_unref(sw);
                int t = av_hwframe_transfer_data(sw, frame, 0);  // GPU surface -> NV12 in memory
                if (t < 0) {
                    err = "hw frame transfer: " + av_err(t);
                    av_frame_unref(frame);
                    return false;
                }
                src = sw;
            }
            if (!frame_from_av(src, vf, err)) return false;
            int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            vf.pts_us = ts == AV_NOPTS_VALUE ? 0 : ts;
            av_frame_unref(frame);
            out.push_back(std::move(vf));
        }
    }
};

VideoDecoder::VideoDecoder() : p_(new Impl) {}
VideoDecoder::~VideoDecoder() { p_->close(); }

bool VideoDecoder::open(bool hardware) {
    apply_log_level();
    p_->close();
    p_->name = "software";
    // On Android the hardware decoder is a codec of its own, not an hwaccel on the software one.
    // It has to be configured with the size and the parameter sets, which only the first keyframe
    // carries, so the software decoder starts and decode() swaps it in when that frame arrives.
    p_->want_mediacodec = false;
#ifdef __ANDROID__
    p_->want_mediacodec = hardware && avcodec_find_decoder_by_name("h264_mediacodec") != nullptr;
    if (p_->want_mediacodec) p_->name = "mediacodec";
#endif
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        p_->err = "H.264 decoder not built in";
        return false;
    }
    p_->ctx = avcodec_alloc_context3(codec);
    p_->ctx->pkt_timebase = kUs;
    p_->ctx->thread_count = 0;  // auto
    // Slice threads only: frame threading holds back one frame per thread (16+ on big CPUs),
    // which shows up as latency and as bursts after a seek / at the end of a file.
    p_->ctx->thread_type = FF_THREAD_SLICE;
    p_->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (hardware && !p_->want_mediacodec && !p_->setup_hw(codec)) {
        av_buffer_unref(&p_->hw_dev);
        p_->hw_fmt = AV_PIX_FMT_NONE;  // software decoding, as if hardware had not been asked for
    }
    int r = avcodec_open2(p_->ctx, codec, nullptr);
    if (r < 0) {
        p_->err = "h264 decoder open: " + av_err(r);
        p_->close();
        return false;
    }
    p_->pkt = av_packet_alloc();
    p_->frame = av_frame_alloc();
#ifdef __EMSCRIPTEN__
    // the browser's decoder (hardware when it has one); the software one above stays as the fallback
    static int next_wc = 1000000;  // apart from the encoders' ids
    p_->wait_key = false;
    if (hardware && mk_wc_vdec_open(next_wc)) p_->wc = next_wc++, p_->name = "webcodecs";
#endif
    return true;
}

bool VideoDecoder::decode(const Packet& in, std::vector<VideoFrame>& out) {
#ifdef __EMSCRIPTEN__
    if (p_->wc) {
        const std::string codec = in.keyframe ? Impl::codec_string(in) : "";
        if (mk_wc_vdec_decode(p_->wc, in.data.data(), (int)in.data.size(), (double)in.pts_us, in.keyframe ? 1 : 0, codec.c_str())) {
            p_->receive_webcodecs(out);
            return true;
        }
        p_->receive_webcodecs(out);
        p_->fall_back_to_software();
    }
    if (p_->wait_key) {
        if (!in.keyframe) return true;
        p_->wait_key = false;
    }
#endif
    if (!p_->ctx) return false;
    if (p_->want_mediacodec && in.keyframe) p_->try_mediacodec(in);
    av_new_packet(p_->pkt, (int)in.data.size());
    memcpy(p_->pkt->data, in.data.data(), in.data.size());
    p_->pkt->pts = in.pts_us;
    p_->pkt->dts = in.dts_us;
    if (in.keyframe) p_->pkt->flags |= AV_PKT_FLAG_KEY;
    int r = avcodec_send_packet(p_->ctx, p_->pkt);
    av_packet_unref(p_->pkt);
    if (r == AVERROR(EAGAIN)) {
        if (!p_->receive(out)) return false;
        av_new_packet(p_->pkt, (int)in.data.size());
        memcpy(p_->pkt->data, in.data.data(), in.data.size());
        p_->pkt->pts = in.pts_us;
        p_->pkt->dts = in.dts_us;
        r = avcodec_send_packet(p_->ctx, p_->pkt);
        av_packet_unref(p_->pkt);
    }
    if (r < 0 && r != AVERROR_INVALIDDATA) {
        p_->err = "send_packet: " + av_err(r);
        return false;
    }
    return p_->receive(out);
}

bool VideoDecoder::flush(std::vector<VideoFrame>& out) {
#ifdef __EMSCRIPTEN__
    if (p_->wc) {  // waits for the browser (ASYNCIFY): call it from the program's flow, not from a device callback
        mk_wc_vdec_flush(p_->wc);
        p_->receive_webcodecs(out);
        return true;
    }
#endif
    if (!p_->ctx) return false;
    avcodec_send_packet(p_->ctx, nullptr);
    bool ok = p_->receive(out);
    avcodec_flush_buffers(p_->ctx);
    return ok;
}

bool VideoDecoder::drain(std::vector<VideoFrame>& out) {
#ifdef __EMSCRIPTEN__
    if (p_->wc) {
        const bool done = mk_wc_vdec_drain(p_->wc) != 0;
        p_->receive_webcodecs(out);
        return done;
    }
#endif
    flush(out);
    return true;
}

void VideoDecoder::poll(std::vector<VideoFrame>& out) {
#ifdef __EMSCRIPTEN__
    if (p_->wc) p_->receive_webcodecs(out);
#endif
    (void)out;
}

void VideoDecoder::close() { p_->close(); }
std::string VideoDecoder::name() const { return p_->name; }
const std::string& VideoDecoder::error() const { return p_->err; }

// ---------------------------------------------------------------- AudioDecoder

struct AudioDecoder::Impl {
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* swr = nullptr;
    std::string err;
    void close() {
        avcodec_free_context(&ctx);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        swr_free(&swr);
    }
};

AudioDecoder::AudioDecoder() : p_(new Impl) {}
AudioDecoder::~AudioDecoder() { p_->close(); }

bool AudioDecoder::open(int sample_rate, int channels, const std::vector<uint8_t>& extradata) {
    p_->close();
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec) {
        p_->err = "AAC decoder not built in";
        return false;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->sample_rate = sample_rate;
    av_channel_layout_default(&c->ch_layout, channels);
    c->pkt_timebase = kUs;
    if (!extradata.empty()) {
        c->extradata = (uint8_t*)av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(c->extradata, extradata.data(), extradata.size());
        c->extradata_size = (int)extradata.size();
    }
    int r = avcodec_open2(c, codec, nullptr);
    if (r < 0) {
        p_->err = "aac decoder open: " + av_err(r);
        avcodec_free_context(&c);
        return false;
    }
    p_->ctx = c;
    p_->pkt = av_packet_alloc();
    p_->frame = av_frame_alloc();
    return true;
}

bool AudioDecoder::decode(const Packet& in, std::vector<AudioFrame>& out) {
    Impl& s = *p_;
    if (!s.ctx) return false;
    av_new_packet(s.pkt, (int)in.data.size());
    memcpy(s.pkt->data, in.data.data(), in.data.size());
    s.pkt->pts = in.pts_us;
    s.pkt->dts = in.dts_us;
    int r = avcodec_send_packet(s.ctx, s.pkt);
    av_packet_unref(s.pkt);
    if (r < 0 && r != AVERROR_INVALIDDATA && r != AVERROR(EAGAIN)) {
        s.err = "aac send_packet: " + av_err(r);
        return false;
    }
    for (;;) {
        r = avcodec_receive_frame(s.ctx, s.frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) {
            s.err = "aac decode: " + av_err(r);
            return false;
        }
        AVFrame* f = s.frame;
        const int ch = f->ch_layout.nb_channels;
        if (!s.swr) {
            if (swr_alloc_set_opts2(&s.swr, &f->ch_layout, AV_SAMPLE_FMT_S16, f->sample_rate, &f->ch_layout,
                                    (AVSampleFormat)f->format, f->sample_rate, 0, nullptr) < 0 ||
                swr_init(s.swr) < 0) {
                s.err = "aac output converter init failed";
                return false;
            }
        }
        AudioFrame af;
        af.sample_rate = f->sample_rate;
        af.channels = ch;
        af.samples.resize((size_t)f->nb_samples * ch);
        uint8_t* outp[1] = {(uint8_t*)af.samples.data()};
        int got = swr_convert(s.swr, outp, f->nb_samples, (const uint8_t**)f->extended_data, f->nb_samples);
        if (got < 0) got = 0;
        af.samples.resize((size_t)got * ch);
        af.pts_us = f->pts == AV_NOPTS_VALUE ? in.pts_us : f->pts;
        av_frame_unref(f);
        out.push_back(std::move(af));
    }
    return true;
}

void AudioDecoder::close() { p_->close(); }
const std::string& AudioDecoder::error() const { return p_->err; }

// ---------------------------------------------------------------- Mp4Writer

StreamInfo stream_info_of(const VideoEncoder* video, const AudioEncoder* audio) {
    StreamInfo si;
    if (video) {
        si.has_video = true;
        si.width = video->config().width, si.height = video->config().height, si.fps = video->config().fps;
        si.video_extradata = video->extradata();
    }
    if (audio) {
        si.has_audio = true;
        si.sample_rate = audio->config().sample_rate, si.channels = audio->config().channels;
        si.audio_bitrate = audio->config().bitrate;
        si.audio_extradata = audio->extradata();
    }
    return si;
}

struct Mp4Writer::Impl {
    AVFormatContext* fmt = nullptr;
    AVStream* vs = nullptr;
    AVStream* as = nullptr;
    StreamInfo info;
    bool header = false;
    std::vector<Packet> pending;  // packets held until SPS/PPS are known
    std::string path, err;
    int64_t base_us = INT64_MIN;  // first timestamp of the file -> 0

    bool write_header() {
        if (vs) {
            const std::vector<uint8_t>& ex = info.video_extradata;
            if (ex.empty()) return false;  // not yet
            vs->codecpar->extradata = (uint8_t*)av_mallocz(ex.size() + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(vs->codecpar->extradata, ex.data(), ex.size());
            vs->codecpar->extradata_size = (int)ex.size();
        }
        int r = avformat_write_header(fmt, nullptr);
        if (r < 0) {
            err = "write header: " + av_err(r);
            return false;
        }
        header = true;
        return true;
    }

    bool write_now(const Packet& p) {
        AVStream* st = p.type == MediaType::Video ? vs : as;
        if (!st) return true;
        if (base_us == INT64_MIN) base_us = p.dts_us;
        AVPacket* pkt = av_packet_alloc();
        av_new_packet(pkt, (int)p.data.size());
        memcpy(pkt->data, p.data.data(), p.data.size());
        pkt->stream_index = st->index;
        pkt->pts = av_rescale_q(std::max<int64_t>(0, p.pts_us - base_us), kUs, st->time_base);
        pkt->dts = av_rescale_q(std::max<int64_t>(0, p.dts_us - base_us), kUs, st->time_base);
        if (p.keyframe) pkt->flags |= AV_PKT_FLAG_KEY;
        int r = av_interleaved_write_frame(fmt, pkt);
        av_packet_free(&pkt);
        if (r < 0) {
            err = "write packet: " + av_err(r);
            return false;
        }
        return true;
    }
};

Mp4Writer::Mp4Writer() : p_(new Impl) {}
Mp4Writer::~Mp4Writer() { close(); }

bool Mp4Writer::open(const std::string& path, const VideoEncoder* video, const AudioEncoder* audio) {
    return open(path, stream_info_of(video, audio));
}

bool Mp4Writer::open(const std::string& path, const StreamInfo& info) {
    close();
    Impl& s = *p_;
    s.err.clear();
    s.path = path;
    s.info = info;
    int r = avformat_alloc_output_context2(&s.fmt, nullptr, nullptr, path.c_str());
    if (r < 0 || !s.fmt) {
        s.err = "unknown output format for " + path;
        return false;
    }
    if (info.has_video) {
        s.vs = avformat_new_stream(s.fmt, nullptr);
        AVCodecParameters* cp = s.vs->codecpar;
        cp->codec_type = AVMEDIA_TYPE_VIDEO;
        cp->codec_id = AV_CODEC_ID_H264;
        cp->width = info.width;
        cp->height = info.height;
        cp->format = AV_PIX_FMT_YUV420P;
        s.vs->time_base = {1, 90000};
        if (info.fps > 0) s.vs->avg_frame_rate = {info.fps, 1};
    }
    if (info.has_audio) {
        s.as = avformat_new_stream(s.fmt, nullptr);
        AVCodecParameters* cp = s.as->codecpar;
        cp->codec_type = AVMEDIA_TYPE_AUDIO;
        cp->codec_id = AV_CODEC_ID_AAC;
        cp->sample_rate = info.sample_rate;
        av_channel_layout_default(&cp->ch_layout, info.channels);
        cp->bit_rate = info.audio_bitrate;
        cp->frame_size = 1024;
        const std::vector<uint8_t>& ex = info.audio_extradata;
        if (!ex.empty()) {
            cp->extradata = (uint8_t*)av_mallocz(ex.size() + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(cp->extradata, ex.data(), ex.size());
            cp->extradata_size = (int)ex.size();
        }
        s.as->time_base = {1, info.sample_rate};
    }
    if (!(s.fmt->oformat->flags & AVFMT_NOFILE)) {
        r = avio_open(&s.fmt->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (r < 0) {
            s.err = "cannot create " + path + ": " + av_err(r);
            avformat_free_context(s.fmt);
            s.fmt = nullptr;
            return false;
        }
    }
    if (!info.has_video) return s.write_header();
    return true;  // the header waits for SPS/PPS (given, or from the first keyframe)
}

bool Mp4Writer::write(const Packet& pkt) {
    Impl& s = *p_;
    if (!s.fmt) return false;
    if (!s.header) {
        if (pkt.type == MediaType::Video && !pkt.keyframe && s.pending.empty()) return true;  // start at a keyframe
        if (pkt.type == MediaType::Video && pkt.keyframe && s.info.video_extradata.empty())
            s.info.video_extradata = h264_extract_parameter_sets(pkt.data.data(), pkt.data.size());
        s.pending.push_back(pkt);
        if (!s.write_header()) {
            if (!s.err.empty()) return false;
            if (s.pending.size() > 600) {
                s.err = "no SPS/PPS in the video";
                return false;
            }
            return true;
        }
        // Drop audio that precedes the first video keyframe, then flush in order.
        int64_t start = INT64_MIN;
        for (auto& p : s.pending)
            if (p.type == MediaType::Video) { start = p.dts_us; break; }
        for (auto& p : s.pending)
            if (start == INT64_MIN || p.type == MediaType::Video || p.pts_us >= start) {
                if (s.base_us == INT64_MIN) s.base_us = start;
                if (!s.write_now(p)) return false;
            }
        s.pending.clear();
        return true;
    }
    if (pkt.type == MediaType::Audio && s.base_us != INT64_MIN && pkt.pts_us < s.base_us) return true;
    return s.write_now(pkt);
}

bool Mp4Writer::close() {
    Impl& s = *p_;
    if (!s.fmt) return true;
    bool ok = true;
    if (s.header) {
        int r = av_write_trailer(s.fmt);
        if (r < 0) s.err = "write trailer: " + av_err(r), ok = false;
    } else {
        ok = false;
        if (s.err.empty()) s.err = "nothing was written";
    }
    if (!(s.fmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&s.fmt->pb);
    avformat_free_context(s.fmt);
    s.fmt = nullptr;
    s.vs = s.as = nullptr;
    s.header = false;
    s.pending.clear();
    s.base_us = INT64_MIN;
    return ok;
}

const std::string& Mp4Writer::error() const { return p_->err; }

// ---------------------------------------------------------------- Mp4Reader

struct Mp4Reader::Impl {
    AVFormatContext* fmt = nullptr;
    int vi = -1, ai = -1;
    int nal_len = 0;              // 0: stream is already Annex-B; else avcC NAL length size
    std::vector<uint8_t> ps;      // Annex-B SPS/PPS to put before keyframes
    int64_t video_count = 0;      // video packets read (timestamps for streams without any)
    bool adts = false;            // audio packets carry ADTS headers (.aac files): strip them
    MediaInfo info;
    AVPacket* pkt = nullptr;
    std::string err;
    void close() {
        avformat_close_input(&fmt);
        av_packet_free(&pkt);
        vi = ai = -1;
        info = MediaInfo();
    }
};

Mp4Reader::Mp4Reader() : p_(new Impl) {}
Mp4Reader::~Mp4Reader() { p_->close(); }

// avcC -> Annex-B SPS/PPS, returns NAL length size (or 0 if extradata is not avcC).
static int parse_avcc(const uint8_t* d, int n, std::vector<uint8_t>& ps) {
    if (n < 7 || d[0] != 1) return 0;
    const int len_size = (d[4] & 3) + 1;
    int pos = 5;
    for (int pass = 0; pass < 2; ++pass) {
        if (pos >= n) break;
        int count = pass == 0 ? (d[pos] & 0x1f) : d[pos];
        ++pos;
        for (int i = 0; i < count && pos + 2 <= n; ++i) {
            int l = (d[pos] << 8) | d[pos + 1];
            pos += 2;
            if (pos + l > n) break;
            static const uint8_t sc[4] = {0, 0, 0, 1};
            ps.insert(ps.end(), sc, sc + 4);
            ps.insert(ps.end(), d + pos, d + pos + l);
            pos += l;
        }
    }
    return len_size;
}

bool Mp4Reader::open(const std::string& path) {
    Impl& s = *p_;
    s.close();
    s.err.clear();
    int r = avformat_open_input(&s.fmt, path.c_str(), nullptr, nullptr);
    if (r < 0) {
        s.err = "cannot open " + path + ": " + av_err(r);
        return false;
    }
    r = avformat_find_stream_info(s.fmt, nullptr);
    if (r < 0) {
        s.err = "cannot read stream info: " + av_err(r);
        s.close();
        return false;
    }
    s.vi = av_find_best_stream(s.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    s.ai = av_find_best_stream(s.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (s.vi >= 0 && s.fmt->streams[s.vi]->codecpar->codec_id != AV_CODEC_ID_H264) s.vi = -1;
    if (s.ai >= 0 && s.fmt->streams[s.ai]->codecpar->codec_id != AV_CODEC_ID_AAC) s.ai = -1;
    if (s.vi < 0 && s.ai < 0) {
        s.err = "no H.264 video or AAC audio in " + path;
        s.close();
        return false;
    }
    if (s.vi >= 0) {
        AVStream* st = s.fmt->streams[s.vi];
        s.info.has_video = true;
        s.info.width = st->codecpar->width;
        s.info.height = st->codecpar->height;
        AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
        s.info.fps = fr.den ? av_q2d(fr) : 0;
        s.nal_len = parse_avcc(st->codecpar->extradata, st->codecpar->extradata_size, s.ps);
        if (s.nal_len == 0 && st->codecpar->extradata_size > 0)
            s.ps = h264_extract_parameter_sets(st->codecpar->extradata, st->codecpar->extradata_size);
        s.info.video_extradata = s.ps;
    }
    if (s.ai >= 0) {
        AVCodecParameters* cp = s.fmt->streams[s.ai]->codecpar;
        s.info.has_audio = true;
        s.info.sample_rate = cp->sample_rate;
        s.info.channels = cp->ch_layout.nb_channels;
        if (cp->extradata_size > 0) s.info.audio_extradata.assign(cp->extradata, cp->extradata + cp->extradata_size);
    }
    s.pkt = av_packet_alloc();
    if (s.ai >= 0 && s.info.audio_extradata.empty()) {
        // No AudioSpecificConfig: an ADTS stream. Build it from the first header, then start over.
        while (av_read_frame(s.fmt, s.pkt) >= 0) {
            const bool mine = s.pkt->stream_index == s.ai;
            const uint8_t* h = s.pkt->data;
            if (mine && s.pkt->size >= 7 && h[0] == 0xFF && (h[1] & 0xF0) == 0xF0) {
                const int obj = ((h[2] >> 6) & 3) + 1, sri = (h[2] >> 2) & 0xF, ch = ((h[2] & 1) << 2) | (h[3] >> 6);
                s.info.audio_extradata = {(uint8_t)((obj << 3) | (sri >> 1)), (uint8_t)(((sri & 1) << 7) | (ch << 3))};
                s.adts = true;
            }
            av_packet_unref(s.pkt);
            if (mine) break;
        }
        avformat_close_input(&s.fmt);
        AVPacket* keep = s.pkt;
        s.pkt = nullptr;
        if (avformat_open_input(&s.fmt, path.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(s.fmt, nullptr) < 0) {
            av_packet_free(&keep);
            s.err = "cannot reopen " + path;
            s.close();
            return false;
        }
        s.pkt = keep;
    }
    s.info.duration_us = s.fmt->duration == AV_NOPTS_VALUE ? 0 : s.fmt->duration;
    return true;
}

const MediaInfo& Mp4Reader::info() const { return p_->info; }

bool Mp4Reader::read(Packet& out) {
    Impl& s = *p_;
    if (!s.fmt) return false;
    for (;;) {
        int r = av_read_frame(s.fmt, s.pkt);
        if (r < 0) {
            if (r != AVERROR_EOF) s.err = "read: " + av_err(r);
            return false;
        }
        if (s.pkt->stream_index != s.vi && s.pkt->stream_index != s.ai) {
            av_packet_unref(s.pkt);
            continue;
        }
        AVStream* st = s.fmt->streams[s.pkt->stream_index];
        const int64_t start = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;
        out = Packet();
        out.type = s.pkt->stream_index == s.vi ? MediaType::Video : MediaType::Audio;
        int64_t pts = s.pkt->pts == AV_NOPTS_VALUE ? s.pkt->dts : s.pkt->pts;
        int64_t dts = s.pkt->dts == AV_NOPTS_VALUE ? pts : s.pkt->dts;
        if (pts == AV_NOPTS_VALUE) {
            // elementary streams (.h264) carry no time: number the frames at the stream's rate
            const double fps = s.info.fps > 0 ? s.info.fps : 30;
            out.pts_us = out.dts_us = out.type == MediaType::Video ? (int64_t)(s.video_count * 1e6 / fps) : 0;
        } else {
            out.pts_us = av_rescale_q(pts - start, st->time_base, kUs);
            out.dts_us = av_rescale_q(dts - start, st->time_base, kUs);
        }
        if (out.type == MediaType::Video) ++s.video_count;
        out.keyframe = (s.pkt->flags & AV_PKT_FLAG_KEY) != 0;
        const uint8_t* d = s.pkt->data;
        const int n = s.pkt->size;
        if (out.type == MediaType::Video && s.nal_len > 0) {
            // length-prefixed -> Annex-B, SPS/PPS in front of keyframes
            if (out.keyframe) out.data = s.ps;
            int pos = 0;
            while (pos + s.nal_len <= n) {
                uint32_t l = 0;
                for (int i = 0; i < s.nal_len; ++i) l = (l << 8) | d[pos + i];
                pos += s.nal_len;
                if (l > (uint32_t)(n - pos)) break;
                static const uint8_t sc[4] = {0, 0, 0, 1};
                out.data.insert(out.data.end(), sc, sc + 4);
                out.data.insert(out.data.end(), d + pos, d + pos + l);
                pos += (int)l;
            }
        } else if (out.type == MediaType::Audio && s.adts && n >= 7 && d[0] == 0xFF && (d[1] & 0xF0) == 0xF0) {
            const int hl = (d[1] & 1) ? 7 : 9;  // protection_absent ? no CRC : CRC
            out.data.assign(d + std::min(hl, n), d + n);
        } else {
            out.data.assign(d, d + n);
        }
        av_packet_unref(s.pkt);
        return true;
    }
}

bool Mp4Reader::seek(int64_t pts_us) {
    if (!p_->fmt) return false;
    if (pts_us == 0) p_->video_count = 0;
    return av_seek_frame(p_->fmt, -1, pts_us, AVSEEK_FLAG_BACKWARD) >= 0;
}

void Mp4Reader::close() { p_->close(); }
const std::string& Mp4Reader::error() const { return p_->err; }

}  // namespace mk
