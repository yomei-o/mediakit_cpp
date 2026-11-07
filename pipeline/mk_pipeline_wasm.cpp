// mediakit pipelines for the browser (wasm): the same Recorder / Player / FileSource as mk_pipeline.cpp,
// without threads. Work that runs on threads elsewhere runs here on a JS timer ("pump", every few ms) and
// in the camera / microphone callbacks, all on the main thread while the program waits (Window::run(),
// emscripten_sleep). H.264 goes through the browser's WebCodecs (see mk_codec), so the main thread only
// copies frames; FFmpeg handles AAC and MP4 as everywhere else. Files live in MEMFS (mk::finish_saved_file
// hands a recording to the user as a download).
// start_recording / stop_recording / close wait for the browser (ASYNCIFY): call them from the program's
// flow (main, a button, a timer), not from a frame callback.
#include <emscripten.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>

#include "mk_pipeline.h"

EM_JS(void, mk_js_pump_start, (int id, int ms), {
    Module.mkPump = Module.mkPump || {};
    Module.mkPump[id] = setInterval(() => _mk_wasm_pump_tick(id), ms);
});

EM_JS(void, mk_js_pump_stop, (int id), {
    if (!Module.mkPump || !Module.mkPump[id]) return;
    clearInterval(Module.mkPump[id]);
    delete Module.mkPump[id];
});

namespace mk {

namespace {

void fit_size(int src_w, int src_h, int want_w, int want_h, int& w, int& h) {
    if (want_w <= 0 && want_h <= 0) w = src_w, h = src_h;
    else if (want_w > 0 && want_h > 0) w = want_w, h = want_h;
    else if (want_w > 0) w = want_w, h = (int)((long long)src_h * want_w / std::max(1, src_w));
    else h = want_h, w = (int)((long long)src_w * want_h / std::max(1, src_h));
    w = std::max(2, w & ~1), h = std::max(2, h & ~1);
}

std::map<int, std::function<void()>>& pumps() {
    static std::map<int, std::function<void()>> m;
    return m;
}

// Calls fn every few ms from a JS timer, in place of a thread.
class Pump {
public:
    ~Pump() { stop(); }
    void start(std::function<void()> fn, int ms = 5) {
        stop();
        static int next = 1;
        id_ = next++;
        pumps()[id_] = std::move(fn);
        mk_js_pump_start(id_, ms);
    }
    void stop() {
        if (!id_) return;
        mk_js_pump_stop(id_);
        pumps().erase(id_);
        id_ = 0;
    }

private:
    int id_ = 0;
};

// Work done in one tick is capped so the page stays responsive.
const int64_t kTickBudgetUs = 12000;

}  // namespace

}  // namespace mk

extern "C" EMSCRIPTEN_KEEPALIVE void mk_wasm_pump_tick(int id) {
    auto it = mk::pumps().find(id);
    if (it == mk::pumps().end()) return;
    std::function<void()> fn = it->second;  // the tick may stop (erase) its own pump
    fn();
}

namespace mk {

// ================================================================ FileSource

struct FileSource::Impl {
    Mp4Reader reader;
    VideoDecoder vdec;
    AudioDecoder adec;
    bool loop = false, running = false, done = false, eof = false, draining = false;
    std::string err;
    Pump pump;
    std::function<void(const VideoFrame&)> on_video;
    std::function<void(const AudioFrame&)> on_audio;
    int64_t t0 = 0, offset = 0, last_end = 0, frame_us = 33333, decoded_to = 0;
    std::deque<VideoFrame> vq;  // decoded, pts already on the now_us() clock, waiting for their time
    std::deque<AudioFrame> aq;

    void queue_video(std::vector<VideoFrame>& frames) {
        for (auto& f : frames) {
            last_end = std::max(last_end, f.pts_us + frame_us);
            f.pts_us = t0 + offset + f.pts_us;
            decoded_to = std::max(decoded_to, f.pts_us);
            vq.push_back(std::move(f));
        }
    }

    void tick() {
        if (!running) return;
        const MediaInfo& in = reader.info();
        const int64_t start = now_us();
        std::vector<VideoFrame> vf;
        std::vector<AudioFrame> af;
        Packet p;
        // decode up to 0.3 s ahead
        while (!eof && decoded_to - now_us() < 300000 && now_us() - start < kTickBudgetUs) {
            if (draining) {
                vf.clear();
                const bool all = vdec.drain(vf);
                queue_video(vf);
                if (!all) break;
                draining = false;
                if (!loop || !reader.seek(0)) {
                    eof = true;
                    break;
                }
                offset += std::max(last_end, in.duration_us);  // the next lap continues the timeline
                last_end = 0;
                continue;
            }
            if (!reader.read(p)) {
                draining = true;
                continue;
            }
            if (p.type == MediaType::Video && in.has_video) {
                vf.clear();
                if (!vdec.decode(p, vf)) err = vdec.error();
                queue_video(vf);
            } else if (p.type == MediaType::Audio && in.has_audio) {
                af.clear();
                if (!adec.decode(p, af)) err = adec.error();
                for (auto& a : af) {
                    last_end = std::max(last_end, a.pts_us + a.duration_us());
                    a.pts_us = t0 + offset + a.pts_us;
                    decoded_to = std::max(decoded_to, a.pts_us);
                    aq.push_back(std::move(a));
                }
            }
        }
        // hand out what is due
        const int64_t now = now_us();
        while (running && !aq.empty() && aq.front().pts_us <= now) {
            AudioFrame a = std::move(aq.front());
            aq.pop_front();
            if (on_audio) on_audio(a);
        }
        while (running && !vq.empty() && vq.front().pts_us <= now) {
            VideoFrame f = std::move(vq.front());
            vq.pop_front();
            if (on_video) on_video(f);
        }
        if (eof && vq.empty() && aq.empty()) done = true;
    }
};

FileSource::FileSource() : p_(new Impl) {}
FileSource::~FileSource() { close(); }

bool FileSource::open(const std::string& path, bool loop, bool hardware_decode) {
    close();
    Impl& s = *p_;
    s.err.clear();
    s.loop = loop;
    s.done = false;
    if (!s.reader.open(path)) {
        s.err = s.reader.error();
        return false;
    }
    const MediaInfo& in = s.reader.info();
    if (in.has_video && !s.vdec.open(hardware_decode)) {
        s.err = s.vdec.error();
        return false;
    }
    if (in.has_audio && !s.adec.open(in.sample_rate, in.channels, in.audio_extradata)) {
        s.err = s.adec.error();
        return false;
    }
    s.frame_us = in.fps > 0 ? (int64_t)(1e6 / in.fps) : 33333;
    return true;
}

const MediaInfo& FileSource::info() const { return p_->reader.info(); }

bool FileSource::start(std::function<void(const VideoFrame&)> on_video, std::function<void(const AudioFrame&)> on_audio) {
    Impl& s = *p_;
    stop();
    s.on_video = std::move(on_video);
    s.on_audio = std::move(on_audio);
    s.t0 = s.decoded_to = now_us();
    s.offset = s.last_end = 0;
    s.running = true;
    s.done = s.eof = s.draining = false;
    s.pump.start([&s] { s.tick(); });
    return true;
}

void FileSource::stop() {
    p_->running = false;
    p_->pump.stop();
    p_->vq.clear();
    p_->aq.clear();
}

bool FileSource::finished() const { return p_->done; }

void FileSource::close() {
    stop();
    p_->vdec.close();
    p_->adec.close();
    p_->reader.close();
}

std::string FileSource::error() const { return p_->err; }

// ================================================================ Recorder

struct Recorder::Impl {
    RecorderConfig cfg;
    Camera cam;
    Microphone mic;
    FileSource file;
    bool from_file = false;
    bool have_audio = false;
    int in_w = 0, in_h = 0, in_rate = 0, in_ch = 0;
    double in_fps = 0;
    std::string in_name, audio_name;
    bool running = false;

    std::function<void(const VideoFrame&)> on_preview;
    std::string err;
    FrameConverter pv_conv;
    VideoFrame pv_out;
    int pv_w = 0, pv_h = 0;

    bool accepting = false;  // encoders are running (file and/or sinks)
    int64_t enc_start_us = 0;
    std::unique_ptr<VideoEncoder> venc;
    std::unique_ptr<AudioEncoder> aenc;
    std::unique_ptr<Mp4Writer> writer;
    int64_t rec_start_us = 0;
    struct SinkSlot {
        std::shared_ptr<PacketSink> sink;
        bool started = false;  // got its first keyframe
    };
    std::vector<SinkSlot> sinks;
    std::vector<Packet> pkts;

    RecorderStats st;
    int64_t fps_t0 = 0, fps_n = 0;

    // Frames and audio come straight from the devices (main thread): encode right here. WebCodecs only
    // queues the frame, so this is a copy, not an encode.
    void on_video(const VideoFrame& f) {
        if (fps_t0 == 0) fps_t0 = f.pts_us;
        ++fps_n;
        if (f.pts_us - fps_t0 >= 1000000) {
            st.input_fps = fps_n * 1e6 / (double)(f.pts_us - fps_t0);
            fps_t0 = f.pts_us, fps_n = 0;
        }
        if (on_preview) {
            // Browser frames are BGRA already: hand them over at full size. Scaling to preview_width costs
            // the main thread more than the canvas takes to scale the picture by itself.
            if (f.format == PixelFormat::BGRA) on_preview(f);
            else if (pv_conv.convert(f, pv_out, PixelFormat::BGRA, pv_w, pv_h)) on_preview(pv_out);
        }
        if (accepting && venc && f.pts_us >= enc_start_us) {
            if (!venc->encode(f, pkts)) err = venc->error();
            else if (!venc->error().empty()) ++st.dropped_frames;  // the encoder is behind: it dropped this one
            write_packets();
        }
    }

    void on_audio(const AudioFrame& a) {
        if (!accepting || !aenc) return;
        if (!aenc->encode(a, pkts)) err = aenc->error();
        write_packets();
    }

    StreamInfo stream_info() const { return stream_info_of(venc.get(), aenc.get()); }

    void write_packets() {
        for (auto& p : pkts) {
            if (writer) {
                if (!writer->write(p)) err = "write: " + writer->error();
                st.bytes += (int64_t)p.data.size();
                if (p.type == MediaType::Video) {
                    ++st.video_frames;
                    st.duration_us = std::max<int64_t>(st.duration_us, p.pts_us - rec_start_us);
                }
            }
            for (auto& s : sinks) {
                if (!s.started) {
                    if (p.type != MediaType::Video || !p.keyframe) continue;  // a sink starts at a keyframe
                    s.started = true;
                }
                s.sink->write(p);
            }
        }
        pkts.clear();
    }

    bool ensure_encoders() {
        if (venc) return true;
        VideoEncoderConfig vc = cfg.video;
        int w, h;
        fit_size(in_w, in_h, vc.width, vc.height, w, h);
        vc.width = w, vc.height = h;
        if (vc.fps <= 0) vc.fps = std::max(1, (int)(in_fps + 0.5));
        std::unique_ptr<VideoEncoder> v(new VideoEncoder);
        if (!v->open(vc)) {  // waits for the browser; frames meanwhile only go to the preview
            err = "video encoder: " + v->error();
            return false;
        }
        std::unique_ptr<AudioEncoder> a;
        if (have_audio) {
            a.reset(new AudioEncoder);
            if (!a->open({in_rate, in_ch, cfg.audio_bitrate})) {
                err = "audio encoder: " + a->error();
                a.reset();
            }
        }
        venc = std::move(v);
        aenc = std::move(a);
        st.encoder = venc->name();
        enc_start_us = now_us();
        accepting = true;
        return true;
    }

    // Stop the encoders when nothing listens any more.
    void maybe_stop_encoders() {
        if (writer || !sinks.empty() || !venc) return;
        accepting = false;
        venc.reset();
        aenc.reset();
    }
};

Recorder::Recorder() : p_(new Impl) {}
Recorder::~Recorder() { close(); }

bool Recorder::open(const RecorderConfig& cfg, std::function<void(const VideoFrame&)> on_preview) {
    close();
    Impl& s = *p_;
    s.cfg = cfg;
    s.err.clear();
    s.st = RecorderStats();
    s.fps_t0 = s.fps_n = 0;
    s.from_file = !cfg.input_file.empty();
    s.have_audio = false;
    if (s.from_file) {
        if (!s.file.open(cfg.input_file, cfg.input_loop, cfg.input_hardware_decode)) {
            s.err = "input file: " + s.file.error();
            return false;
        }
        const MediaInfo& in = s.file.info();
        if (!in.has_video) {
            s.err = "input file has no H.264 video";
            return false;
        }
        s.in_w = in.width & ~1, s.in_h = in.height & ~1, s.in_fps = in.fps > 0 ? in.fps : 30;
        s.have_audio = cfg.audio && in.has_audio;
        s.in_rate = in.sample_rate, s.in_ch = in.channels;
        s.in_name = cfg.input_file;
        s.audio_name = s.have_audio ? "file" : "";
    } else {
        if (!s.cam.open(cfg.camera)) {
            s.err = "camera: " + s.cam.error();
            return false;
        }
        s.in_w = s.cam.width(), s.in_h = s.cam.height(), s.in_fps = s.cam.fps();
        s.in_name = s.cam.name();
        if (cfg.audio) {
            if (s.mic.open(cfg.microphone)) {
                s.have_audio = true;
                s.in_rate = s.mic.sample_rate(), s.in_ch = s.mic.channels();
                s.audio_name = s.mic.name();
            } else {
                s.err = "microphone: " + s.mic.error() + " (video only)";
            }
        }
    }
    fit_size(s.in_w, s.in_h, cfg.preview_width, cfg.preview_height, s.pv_w, s.pv_h);
    s.on_preview = std::move(on_preview);
    s.running = true;
    auto on_v = [&s](const VideoFrame& f) { s.on_video(f); };
    auto on_a = [&s](const AudioFrame& a) { s.on_audio(a); };
    if (s.from_file) {
        s.file.start(on_v, s.have_audio ? std::function<void(const AudioFrame&)>(on_a) : nullptr);
    } else {
        if (!s.cam.start(on_v)) {
            s.err = "camera: " + s.cam.error();
            close();
            return false;
        }
        if (s.have_audio && !s.mic.start(on_a)) {
            s.err = "microphone: " + s.mic.error();
            s.have_audio = false;
        }
    }
    return true;
}

bool Recorder::start_recording(const std::string& path) {
    Impl& s = *p_;
    if (!s.running) return false;
    stop_recording();
    if (!s.ensure_encoders()) return false;
    s.writer.reset(new Mp4Writer);
    if (!s.writer->open(path, s.venc.get(), s.aenc.get())) {
        s.err = s.writer->error();
        s.writer.reset();
        s.maybe_stop_encoders();
        return false;
    }
    s.venc->request_keyframe();  // the file starts at the next keyframe
    s.rec_start_us = now_us();
    const double fps = s.st.input_fps;
    const int64_t dropped = s.st.dropped_frames;
    const std::string enc = s.st.encoder;
    s.st = RecorderStats();
    s.st.input_fps = fps, s.st.dropped_frames = dropped, s.st.encoder = enc;
    s.st.recording = true;
    s.st.sinks = (int)s.sinks.size();
    return true;
}

bool Recorder::stop_recording() {
    Impl& s = *p_;
    if (!s.writer) return true;
    bool ok = true;
    if (s.sinks.empty()) {
        // last output: flush the encoders into the file (waits for the browser; no new input meanwhile)
        s.accepting = false;
        if (s.venc) s.venc->flush(s.pkts);
        if (s.aenc) s.aenc->flush(s.pkts);
        s.write_packets();
    }
    if (!s.writer->close()) {
        s.err = s.writer->error();
        ok = false;
    }
    s.writer.reset();
    s.maybe_stop_encoders();
    s.st.recording = false;
    return ok;
}

bool Recorder::recording() const { return p_->st.recording; }

bool Recorder::add_sink(std::shared_ptr<PacketSink> sink) {
    Impl& s = *p_;
    if (!s.running || !sink) return false;
    if (!s.ensure_encoders()) return false;
    if (!sink->open(s.stream_info())) {
        s.err = "sink open failed";
        s.maybe_stop_encoders();
        return false;
    }
    s.sinks.push_back({sink, false});
    s.venc->request_keyframe();
    s.st.sinks = (int)s.sinks.size();
    return true;
}

void Recorder::remove_sink(const std::shared_ptr<PacketSink>& sink) {
    Impl& s = *p_;
    auto it = std::find_if(s.sinks.begin(), s.sinks.end(), [&](const Impl::SinkSlot& x) { return x.sink == sink; });
    if (it == s.sinks.end()) return;
    it->sink->close();
    s.sinks.erase(it);
    s.st.sinks = (int)s.sinks.size();
    s.maybe_stop_encoders();
}

RecorderStats Recorder::stats() const { return p_->st; }

void Recorder::close() {
    Impl& s = *p_;
    stop_recording();
    for (auto& x : s.sinks) x.sink->close();
    s.sinks.clear();
    s.maybe_stop_encoders();
    s.running = false;
    s.cam.close();
    s.mic.close();
    s.file.close();
}

int Recorder::input_width() const { return p_->in_w; }
int Recorder::input_height() const { return p_->in_h; }
std::string Recorder::input_name() const { return p_->in_name; }
std::string Recorder::audio_name() const { return p_->audio_name; }
std::string Recorder::error() const { return p_->err; }

// ================================================================ Player

struct Player::Impl {
    Mp4Reader reader;
    VideoDecoder vdec;
    AudioDecoder adec;
    Speaker spk;
    bool has_audio = false, has_video = false;
    FrameConverter conv;
    int out_w = 0, out_h = 0;
    std::function<void(const VideoFrame&)> on_video;
    std::string err;
    Pump pump;
    bool running = false, is_paused = false, eof = false, draining = false, done = false;
    bool audio_on = false;      // the speaker runs (it waits for the first frames: the browser's decoder
                                // takes a moment to start, and the audio would run ahead of the picture)
    std::deque<VideoFrame> fq;  // decoded, in pts order; converted to BGRA only when shown (one thread
                                // does it all here, and frames that are skipped need no conversion)
    VideoFrame shown;

    // clock: media time anchored to the wall clock, re-anchored on the audio position (as mk_pipeline.cpp)
    int64_t anchor_media = 0, anchor_wall = 0;
    bool clock_started = false;

    int64_t clock() {
        const int64_t now = now_us();
        if (has_audio) {
            int64_t a = spk.played_pts_us();
            if (a >= 0) {
                int64_t predicted = anchor_media + (is_paused ? 0 : now - anchor_wall);
                // follow the audio while it moves; extrapolate when it has run out
                if (!clock_started || a > anchor_media) anchor_media = a, anchor_wall = now, clock_started = true;
                else if (!is_paused) return predicted;
                return anchor_media;
            }
            if (!clock_started) return -1;
        }
        if (!clock_started) anchor_media = 0, anchor_wall = now, clock_started = true;
        return anchor_media + (is_paused ? 0 : now - anchor_wall);
    }

    void push_video(std::vector<VideoFrame>& vf) {
        for (auto& f : vf) fq.push_back(std::move(f));
    }

    // What the decode thread does elsewhere, in slices: keep a little audio queued and the video queue short.
    void fill() {
        const int64_t start = now_us();
        Packet p;
        std::vector<VideoFrame> vf;
        std::vector<AudioFrame> af;
        while (!eof && now_us() - start < kTickBudgetUs) {
            if (has_audio && spk.queued_us() > 400000 && (!has_video || fq.size() >= 2)) break;
            // Normally keep 8 decoded frames ahead. If the speaker is about to run dry, read on anyway
            // (up to 64): the audio clock drives the video, so starving the audio would stall both.
            if (!(fq.size() < 8 || (has_audio && fq.size() < 64 && spk.queued_us() < 350000))) break;
            if (draining) {
                vf.clear();
                const bool all = vdec.drain(vf);
                push_video(vf);
                if (all) eof = true;
                break;
            }
            if (!reader.read(p)) {
                if (has_video) draining = true;
                else eof = true;
                continue;
            }
            if (p.type == MediaType::Video && has_video) {
                vf.clear();
                if (!vdec.decode(p, vf)) err = vdec.error();
                push_video(vf);
            } else if (p.type == MediaType::Audio && has_audio) {
                af.clear();
                if (!adec.decode(p, af)) err = adec.error();
                for (auto& a : af) spk.write(a);
            }
        }
    }

    // What the show thread does elsewhere: the frame that is due now, skipping late ones.
    void show() {
        if (fq.empty()) {
            if (eof && (!has_audio || spk.queued_us() <= 0 || clock() >= reader.info().duration_us)) done = true;
            return;
        }
        const int64_t c = clock();
        if (c < 0 || fq.front().pts_us > c + 5000) return;  // audio not started yet / not due yet
        while (fq.size() > 1 && fq[1].pts_us <= c) fq.pop_front();
        VideoFrame f = std::move(fq.front());
        fq.pop_front();
        if (on_video && conv.convert(f, shown, PixelFormat::BGRA, out_w, out_h)) {
            shown.pts_us = f.pts_us;
            on_video(shown);
        }
    }

    void tick() {
        if (!running) return;
        if (!is_paused) {
            fill();
            if (has_audio && !audio_on && (fq.size() >= 2 || eof || !has_video)) audio_on = true, spk.start();
        }
        show();
    }
};

Player::Player() : p_(new Impl) {}
Player::~Player() { close(); }

bool Player::open(const std::string& path, std::function<void(const VideoFrame&)> on_video, int out_width, int out_height,
                  bool hardware_decode) {
    close();
    Impl& s = *p_;
    s.err.clear();
    if (!s.reader.open(path)) {
        s.err = s.reader.error();
        return false;
    }
    const MediaInfo& in = s.reader.info();
    s.has_video = in.has_video && s.vdec.open(hardware_decode);
    s.has_audio = false;
    if (in.has_audio && s.adec.open(in.sample_rate, in.channels, in.audio_extradata)) {
        SpeakerConfig sc;
        sc.sample_rate = in.sample_rate;
        sc.channels = std::min(2, std::max(1, in.channels));
        if (s.spk.open(sc)) s.has_audio = true;
        else s.err = "speaker: " + s.spk.error() + " (playing without sound)";
    }
    if (in.has_video) fit_size(in.width, in.height, out_width, out_height, s.out_w, s.out_h);
    s.on_video = std::move(on_video);
    s.eof = s.done = s.draining = false;
    s.clock_started = false;
    return true;
}

bool Player::play() {
    Impl& s = *p_;
    if (s.running) {
        if (s.is_paused) {
            s.anchor_wall = now_us();
            s.is_paused = false;
            if (s.audio_on) s.spk.start();
        }
        return true;
    }
    if (!s.has_video && !s.has_audio) return false;
    s.running = true;
    s.is_paused = false;
    s.audio_on = false;  // tick() starts the speaker
    s.pump.start([&s] { s.tick(); });
    return true;
}

void Player::pause() {
    Impl& s = *p_;
    if (!s.running || s.is_paused) return;
    s.anchor_media += now_us() - s.anchor_wall;
    s.anchor_wall = now_us();
    s.is_paused = true;
    if (s.audio_on) s.spk.stop();
}

bool Player::paused() const { return p_->is_paused; }
bool Player::finished() const { return p_->done; }
int64_t Player::position_us() const { return std::max<int64_t>(0, p_->clock()); }
const MediaInfo& Player::info() const { return p_->reader.info(); }

void Player::close() {
    Impl& s = *p_;
    s.running = false;
    s.pump.stop();
    s.spk.close();
    s.vdec.close();
    s.adec.close();
    s.reader.close();
    s.fq.clear();
}

std::string Player::decoder_name() const { return p_->vdec.name(); }
std::string Player::error() const { return p_->err; }

}  // namespace mk
