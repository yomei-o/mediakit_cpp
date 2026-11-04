// mediakit pipelines: Recorder (camera + mic -> preview + MP4) and Player (MP4 -> video callback + speaker).
#include "mk_pipeline.h"

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace mk {

namespace {

void fit_size(int src_w, int src_h, int want_w, int want_h, int& w, int& h) {
    if (want_w <= 0 && want_h <= 0) w = src_w, h = src_h;
    else if (want_w > 0 && want_h > 0) w = want_w, h = want_h;
    else if (want_w > 0) w = want_w, h = (int)((long long)src_h * want_w / std::max(1, src_w));
    else h = want_h, w = (int)((long long)src_w * want_h / std::max(1, src_h));
    w = std::max(2, w & ~1), h = std::max(2, h & ~1);
}

}  // namespace

// ================================================================ FileSource

struct FileSource::Impl {
    Mp4Reader reader;
    VideoDecoder vdec;
    AudioDecoder adec;
    bool loop = false;
    std::thread th;
    std::atomic<bool> running{false}, done{false};
    mutable std::mutex err_mu;
    std::string err;
    void set_err(const std::string& e) {
        std::lock_guard<std::mutex> lk(err_mu);
        err = e;
    }

    void run(std::function<void(const VideoFrame&)> on_video, std::function<void(const AudioFrame&)> on_audio) {
        const MediaInfo& in = reader.info();
        const int64_t frame_us = in.fps > 0 ? (int64_t)(1e6 / in.fps) : 33333;
        int64_t t0 = now_us(), offset = 0, last_end = 0;
        auto wait_until = [&](int64_t due) {
            while (running && now_us() < due) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        std::vector<VideoFrame> vf;
        std::vector<AudioFrame> af;
        auto emit_video = [&](std::vector<VideoFrame>& frames) {
            for (auto& f : frames) {
                last_end = std::max(last_end, f.pts_us + frame_us);
                f.pts_us = t0 + offset + f.pts_us;
                wait_until(f.pts_us);
                if (!running) return;
                if (on_video) on_video(f);
            }
        };
        Packet p;
        while (running) {
            if (!reader.read(p)) {
                vf.clear();
                if (in.has_video) vdec.flush(vf);
                emit_video(vf);
                if (!loop || !reader.seek(0)) break;
                offset += std::max(last_end, in.duration_us);  // the next lap continues the timeline
                last_end = 0;
                continue;
            }
            if (p.type == MediaType::Video && in.has_video) {
                vf.clear();
                if (!vdec.decode(p, vf)) set_err(vdec.error());
                emit_video(vf);
            } else if (p.type == MediaType::Audio && in.has_audio) {
                af.clear();
                if (!adec.decode(p, af)) set_err(adec.error());
                for (auto& a : af) {
                    last_end = std::max(last_end, a.pts_us + a.duration_us());
                    a.pts_us = t0 + offset + a.pts_us;
                    wait_until(a.pts_us);
                    if (!running) break;
                    if (on_audio) on_audio(a);
                }
            }
        }
        done = true;
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
        s.set_err(s.reader.error());
        return false;
    }
    const MediaInfo& in = s.reader.info();
    if (in.has_video && !s.vdec.open(hardware_decode)) {
        s.set_err(s.vdec.error());
        return false;
    }
    if (in.has_audio && !s.adec.open(in.sample_rate, in.channels, in.audio_extradata)) {
        s.set_err(s.adec.error());
        return false;
    }
    return true;
}

const MediaInfo& FileSource::info() const { return p_->reader.info(); }

bool FileSource::start(std::function<void(const VideoFrame&)> on_video, std::function<void(const AudioFrame&)> on_audio) {
    Impl& s = *p_;
    stop();
    s.running = true;
    s.done = false;
    s.th = std::thread([&s, on_video, on_audio] { s.run(on_video, on_audio); });
    return true;
}

void FileSource::stop() {
    p_->running = false;
    if (p_->th.joinable()) p_->th.join();
}

bool FileSource::finished() const { return p_->done; }

void FileSource::close() {
    stop();
    p_->vdec.close();
    p_->adec.close();
    p_->reader.close();
}

std::string FileSource::error() const {
    std::lock_guard<std::mutex> lk(p_->err_mu);
    return p_->err;
}

// ================================================================ Recorder

struct Recorder::Impl {
    RecorderConfig cfg;
    // input: camera + microphone, or a file
    Camera cam;
    Microphone mic;
    FileSource file;
    bool from_file = false;
    bool have_audio = false;
    int in_w = 0, in_h = 0, in_rate = 0, in_ch = 0;
    double in_fps = 0;
    std::string in_name, audio_name;

    std::function<void(const VideoFrame&)> on_preview;
    mutable std::mutex err_mu;
    std::string err;
    void set_err(const std::string& e) {
        std::lock_guard<std::mutex> lk(err_mu);
        err = e;
    }

    // preview: latest frame only
    std::thread preview_th;
    std::mutex pv_mu;
    std::condition_variable pv_cv;
    VideoFrame pv_frame;
    bool pv_has = false;
    std::atomic<bool> running{false};
    FrameConverter pv_conv;
    int pv_w = 0, pv_h = 0;

    // encode: queue of captured media
    struct Item {
        bool video = true;
        VideoFrame v;
        AudioFrame a;
    };
    std::thread enc_th;
    std::mutex q_mu;
    std::condition_variable q_cv;
    std::deque<Item> q;
    int q_video = 0;
    bool busy = false;
    std::atomic<bool> accepting{false};  // encoders are running (file and/or sinks)
    int64_t enc_start_us = 0;

    std::mutex enc_mu;  // encoders + outputs
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

    mutable std::mutex st_mu;
    RecorderStats st;
    int64_t fps_t0 = 0, fps_n = 0;

    void on_video(const VideoFrame& f) {
        {
            std::lock_guard<std::mutex> lk(st_mu);
            if (fps_t0 == 0) fps_t0 = f.pts_us;
            ++fps_n;
            if (f.pts_us - fps_t0 >= 1000000) {
                st.input_fps = fps_n * 1e6 / (double)(f.pts_us - fps_t0);
                fps_t0 = f.pts_us, fps_n = 0;
            }
        }
        if (on_preview) {
            std::lock_guard<std::mutex> lk(pv_mu);
            pv_frame = f;  // the previous one, if not shown yet, is simply replaced
            pv_has = true;
            pv_cv.notify_one();
        }
        if (accepting && f.pts_us >= enc_start_us) {
            std::lock_guard<std::mutex> lk(q_mu);
            if (q_video >= 8) {  // encoder behind: drop rather than grow without bound
                std::lock_guard<std::mutex> lk2(st_mu);
                ++st.dropped_frames;
                return;
            }
            Item it;
            it.video = true;
            it.v = f;
            q.push_back(std::move(it));
            ++q_video;
            q_cv.notify_one();
        }
    }

    void on_audio(const AudioFrame& a) {
        if (!accepting) return;
        std::lock_guard<std::mutex> lk(q_mu);
        Item it;
        it.video = false;
        it.a = a;
        q.push_back(std::move(it));
        q_cv.notify_one();
    }

    void preview_loop() {
        VideoFrame f, out;
        while (running) {
            {
                std::unique_lock<std::mutex> lk(pv_mu);
                pv_cv.wait_for(lk, std::chrono::milliseconds(200), [&] { return pv_has || !running; });
                if (!pv_has) continue;
                std::swap(f, pv_frame);
                pv_has = false;
            }
            if (pv_conv.convert(f, out, PixelFormat::BGRA, pv_w, pv_h)) on_preview(out);
        }
    }

    StreamInfo stream_info() const { return stream_info_of(venc.get(), aenc.get()); }

    // with enc_mu held
    void write_packets() {
        for (auto& p : pkts) {
            if (writer) {
                if (!writer->write(p)) set_err("write: " + writer->error());
                std::lock_guard<std::mutex> lk(st_mu);
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

    void encode_loop() {
        while (running) {
            Item it;
            {
                std::unique_lock<std::mutex> lk(q_mu);
                q_cv.wait_for(lk, std::chrono::milliseconds(200), [&] { return !q.empty() || !running; });
                if (q.empty()) continue;
                it = std::move(q.front());
                q.pop_front();
                if (it.video) --q_video;
                busy = true;
            }
            {
                std::lock_guard<std::mutex> lk(enc_mu);
                if (venc) {
                    bool ok = it.video ? venc->encode(it.v, pkts) : (aenc ? aenc->encode(it.a, pkts) : true);
                    if (!ok) set_err(it.video ? venc->error() : aenc->error());
                    write_packets();
                }
            }
            {
                std::lock_guard<std::mutex> lk(q_mu);
                busy = false;
            }
            q_cv.notify_all();
        }
    }

    // with enc_mu held
    bool ensure_encoders() {
        if (venc) return true;
        VideoEncoderConfig vc = cfg.video;
        int w, h;
        fit_size(in_w, in_h, vc.width, vc.height, w, h);
        vc.width = w, vc.height = h;
        if (vc.fps <= 0) vc.fps = std::max(1, (int)(in_fps + 0.5));
        venc.reset(new VideoEncoder);
        if (!venc->open(vc)) {
            set_err("video encoder: " + venc->error());
            venc.reset();
            return false;
        }
        if (have_audio) {
            aenc.reset(new AudioEncoder);
            if (!aenc->open({in_rate, in_ch, cfg.audio_bitrate})) {
                set_err("audio encoder: " + aenc->error());
                aenc.reset();
            }
        }
        {
            std::lock_guard<std::mutex> lk(st_mu);
            st.encoder = venc->name();
        }
        enc_start_us = now_us();
        accepting = true;
        return true;
    }

    // Stop the encoders when nothing listens any more (drain first, flush into the outputs).
    void maybe_stop_encoders(std::unique_lock<std::mutex>& enc_lock) {
        if (writer || !sinks.empty() || !venc) return;
        accepting = false;
        enc_lock.unlock();
        {
            std::unique_lock<std::mutex> lk(q_mu);
            q_cv.wait_for(lk, std::chrono::seconds(10), [&] { return q.empty() && !busy; });
            q.clear();
            q_video = 0;
        }
        enc_lock.lock();
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
    s.from_file = !cfg.input_file.empty();
    s.have_audio = false;
    if (s.from_file) {
        if (!s.file.open(cfg.input_file, cfg.input_loop, cfg.input_hardware_decode)) {
            s.set_err("input file: " + s.file.error());
            return false;
        }
        const MediaInfo& in = s.file.info();
        if (!in.has_video) {
            s.set_err("input file has no H.264 video");
            return false;
        }
        s.in_w = in.width & ~1, s.in_h = in.height & ~1, s.in_fps = in.fps > 0 ? in.fps : 30;
        s.have_audio = cfg.audio && in.has_audio;
        s.in_rate = in.sample_rate, s.in_ch = in.channels;
        s.in_name = cfg.input_file;
        s.audio_name = s.have_audio ? "file" : "";
    } else {
        if (!s.cam.open(cfg.camera)) {
            s.set_err("camera: " + s.cam.error());
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
                s.set_err("microphone: " + s.mic.error() + " (video only)");
            }
        }
    }
    fit_size(s.in_w, s.in_h, cfg.preview_width, cfg.preview_height, s.pv_w, s.pv_h);
    s.on_preview = std::move(on_preview);
    s.running = true;
    s.preview_th = std::thread([&s] { s.preview_loop(); });
    s.enc_th = std::thread([&s] { s.encode_loop(); });
    auto on_v = [&s](const VideoFrame& f) { s.on_video(f); };
    auto on_a = [&s](const AudioFrame& a) { s.on_audio(a); };
    if (s.from_file) {
        s.file.start(on_v, s.have_audio ? std::function<void(const AudioFrame&)>(on_a) : nullptr);
    } else {
        if (!s.cam.start(on_v)) {
            s.set_err("camera: " + s.cam.error());
            close();
            return false;
        }
        if (s.have_audio && !s.mic.start(on_a)) {
            s.set_err("microphone: " + s.mic.error());
            s.have_audio = false;
        }
    }
    return true;
}

bool Recorder::start_recording(const std::string& path) {
    Impl& s = *p_;
    if (!s.running) return false;
    stop_recording();
    std::unique_lock<std::mutex> lk(s.enc_mu);
    if (!s.ensure_encoders()) return false;
    s.writer.reset(new Mp4Writer);
    if (!s.writer->open(path, s.venc.get(), s.aenc.get())) {
        s.set_err(s.writer->error());
        s.writer.reset();
        s.maybe_stop_encoders(lk);
        return false;
    }
    s.venc->request_keyframe();  // the file starts at the next keyframe
    s.rec_start_us = now_us();
    std::lock_guard<std::mutex> lk2(s.st_mu);
    double fps = s.st.input_fps;
    int64_t dropped = s.st.dropped_frames;
    std::string enc = s.st.encoder;
    s.st = RecorderStats();
    s.st.input_fps = fps, s.st.dropped_frames = dropped, s.st.encoder = enc;
    s.st.recording = true;
    s.st.sinks = (int)s.sinks.size();
    return true;
}

bool Recorder::stop_recording() {
    Impl& s = *p_;
    std::unique_lock<std::mutex> lk(s.enc_mu);
    if (!s.writer) return true;
    bool ok = true;
    if (s.sinks.empty()) {
        // last output: drain the queue and flush the encoders into the file
        s.accepting = false;
        lk.unlock();
        {
            std::unique_lock<std::mutex> ql(s.q_mu);
            s.q_cv.wait_for(ql, std::chrono::seconds(10), [&] { return s.q.empty() && !s.busy; });
        }
        lk.lock();
        if (s.venc) s.venc->flush(s.pkts);
        if (s.aenc) s.aenc->flush(s.pkts);
        s.write_packets();
    }
    if (!s.writer->close()) {
        s.set_err(s.writer->error());
        ok = false;
    }
    s.writer.reset();
    s.maybe_stop_encoders(lk);
    std::lock_guard<std::mutex> lk2(s.st_mu);
    s.st.recording = false;
    return ok;
}

bool Recorder::recording() const {
    std::lock_guard<std::mutex> lk(p_->st_mu);
    return p_->st.recording;
}

bool Recorder::add_sink(std::shared_ptr<PacketSink> sink) {
    Impl& s = *p_;
    if (!s.running || !sink) return false;
    std::unique_lock<std::mutex> lk(s.enc_mu);
    if (!s.ensure_encoders()) return false;
    if (!sink->open(s.stream_info())) {
        s.set_err("sink open failed");
        s.maybe_stop_encoders(lk);
        return false;
    }
    s.sinks.push_back({sink, false});
    s.venc->request_keyframe();
    std::lock_guard<std::mutex> lk2(s.st_mu);
    s.st.sinks = (int)s.sinks.size();
    return true;
}

void Recorder::remove_sink(const std::shared_ptr<PacketSink>& sink) {
    Impl& s = *p_;
    std::unique_lock<std::mutex> lk(s.enc_mu);
    auto it = std::find_if(s.sinks.begin(), s.sinks.end(), [&](const Impl::SinkSlot& x) { return x.sink == sink; });
    if (it == s.sinks.end()) return;
    it->sink->close();
    s.sinks.erase(it);
    {
        std::lock_guard<std::mutex> lk2(s.st_mu);
        s.st.sinks = (int)s.sinks.size();
    }
    s.maybe_stop_encoders(lk);
}

RecorderStats Recorder::stats() const {
    std::lock_guard<std::mutex> lk(p_->st_mu);
    return p_->st;
}

void Recorder::close() {
    Impl& s = *p_;
    stop_recording();
    {
        std::unique_lock<std::mutex> lk(s.enc_mu);
        for (auto& x : s.sinks) x.sink->close();
        s.sinks.clear();
        s.maybe_stop_encoders(lk);
    }
    s.cam.stop();
    s.mic.stop();
    s.file.stop();
    s.running = false;
    s.pv_cv.notify_all();
    s.q_cv.notify_all();
    if (s.preview_th.joinable()) s.preview_th.join();
    if (s.enc_th.joinable()) s.enc_th.join();
    s.cam.close();
    s.mic.close();
    s.file.close();
}

int Recorder::input_width() const { return p_->in_w; }
int Recorder::input_height() const { return p_->in_h; }
std::string Recorder::input_name() const { return p_->in_name; }
std::string Recorder::audio_name() const { return p_->audio_name; }
std::string Recorder::error() const {
    std::lock_guard<std::mutex> lk(p_->err_mu);
    return p_->err;
}

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
    mutable std::mutex err_mu;
    std::string err;

    std::thread dec_th, show_th;
    std::atomic<bool> running{false}, is_paused{false}, eof{false}, done{false};
    std::mutex fq_mu;
    std::condition_variable fq_cv;
    std::deque<VideoFrame> fq;

    // clock: media time anchored to the wall clock, re-anchored on the audio position
    mutable std::mutex clk_mu;
    int64_t anchor_media = 0, anchor_wall = 0;
    bool clock_started = false;

    void set_err(const std::string& e) {
        std::lock_guard<std::mutex> lk(err_mu);
        err = e;
    }

    int64_t clock() {
        std::lock_guard<std::mutex> lk(clk_mu);
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

    void decode_loop() {
        Packet p;
        std::vector<VideoFrame> vf;
        std::vector<AudioFrame> af;
        const bool dbg = getenv("MK_DEBUG") != nullptr;
        int64_t next_dbg = 0, last_v = 0, last_a = 0;
        while (running) {
            if (is_paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (dbg && now_us() >= next_dbg) {
                next_dbg = now_us() + 250000;
                size_t n;
                {
                    std::lock_guard<std::mutex> lk(fq_mu);
                    n = fq.size();
                }
                std::fprintf(stderr, "[player] fq %zu  audio queued %.3f  played %.3f  read v %.3f a %.3f\n", n,
                             has_audio ? spk.queued_us() / 1e6 : 0.0, has_audio ? spk.played_pts_us() / 1e6 : 0.0, last_v / 1e6,
                             last_a / 1e6);
            }
            // keep a little audio queued; keep the video queue short
            if ((has_audio && spk.queued_us() > 400000)) {
                std::unique_lock<std::mutex> lk(fq_mu);
                if (!has_video || fq.size() >= 2) {
                    lk.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
            }
            {
                std::unique_lock<std::mutex> lk(fq_mu);
                // Normally keep 8 decoded frames ahead. If the speaker is about to run dry, read on anyway
                // (up to 64): the audio clock drives the video, so starving the audio would stall both.
                auto room = [&] {
                    return fq.size() < 8 || (has_audio && fq.size() < 64 && spk.queued_us() < 350000) || !running;
                };
                fq_cv.wait_for(lk, std::chrono::milliseconds(20), room);
                if (!room()) continue;
            }
            if (!reader.read(p)) {
                if (has_video) {
                    vf.clear();
                    vdec.flush(vf);
                    push_video(vf);
                }
                eof = true;
                return;
            }
            if (p.type == MediaType::Video) last_v = p.pts_us; else last_a = p.pts_us;
            if (p.type == MediaType::Video && has_video) {
                vf.clear();
                if (!vdec.decode(p, vf)) set_err(vdec.error());
                push_video(vf);
            } else if (p.type == MediaType::Audio && has_audio) {
                af.clear();
                if (!adec.decode(p, af)) set_err(adec.error());
                for (auto& a : af) spk.write(a);
            }
        }
    }

    void push_video(std::vector<VideoFrame>& vf) {
        for (auto& f : vf) {
            VideoFrame out;
            if (!conv.convert(f, out, PixelFormat::BGRA, out_w, out_h)) continue;
            std::lock_guard<std::mutex> lk(fq_mu);
            fq.push_back(std::move(out));
            fq_cv.notify_all();
        }
    }

    void show_loop() {
        while (running) {
            VideoFrame f;
            int64_t sleep_us = 0;
            {
                std::unique_lock<std::mutex> lk(fq_mu);
                fq_cv.wait_for(lk, std::chrono::milliseconds(20), [&] { return !fq.empty() || !running; });
                if (fq.empty()) {
                    if (eof && (!has_audio || spk.queued_us() <= 0 || clock() >= reader.info().duration_us)) done = true;
                    continue;
                }
                int64_t c = clock();
                if (c < 0) sleep_us = 5000;  // audio not started yet
                else if (fq.front().pts_us > c + 5000) sleep_us = std::min<int64_t>(fq.front().pts_us - c, 10000);
                if (!sleep_us) {
                    f = std::move(fq.front());
                    fq.pop_front();
                }
            }
            if (sleep_us) {  // not due yet: sleep without holding the queue (don't spin)
                std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
                continue;
            }
            {
                // late, and the next one is due too: skip this one
                std::lock_guard<std::mutex> lk(fq_mu);
                if (!fq.empty() && fq.front().pts_us <= clock()) {
                    fq_cv.notify_all();
                    continue;
                }
            }
            fq_cv.notify_all();
            if (on_video) on_video(f);
        }
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
        s.set_err(s.reader.error());
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
        else s.set_err("speaker: " + s.spk.error() + " (playing without sound)");
    }
    if (in.has_video) fit_size(in.width, in.height, out_width, out_height, s.out_w, s.out_h);
    s.on_video = std::move(on_video);
    s.eof = s.done = false;
    s.clock_started = false;
    return true;
}

bool Player::play() {
    Impl& s = *p_;
    if (s.running) {
        if (s.is_paused) {
            {
                std::lock_guard<std::mutex> lk(s.clk_mu);
                s.anchor_wall = now_us();
            }
            s.is_paused = false;
            if (s.has_audio) s.spk.start();
        }
        return true;
    }
    if (!s.has_video && !s.has_audio) return false;
    s.running = true;
    s.is_paused = false;
    if (s.has_audio) s.spk.start();
    s.dec_th = std::thread([&s] { s.decode_loop(); });
    s.show_th = std::thread([&s] { s.show_loop(); });
    return true;
}

void Player::pause() {
    Impl& s = *p_;
    if (!s.running || s.is_paused) return;
    {
        std::lock_guard<std::mutex> lk(s.clk_mu);
        s.anchor_media += now_us() - s.anchor_wall;
        s.anchor_wall = now_us();
    }
    s.is_paused = true;
    if (s.has_audio) s.spk.stop();
}

bool Player::paused() const { return p_->is_paused; }
bool Player::finished() const { return p_->done; }
int64_t Player::position_us() const { return std::max<int64_t>(0, p_->clock()); }
const MediaInfo& Player::info() const { return p_->reader.info(); }

void Player::close() {
    Impl& s = *p_;
    s.running = false;
    s.fq_cv.notify_all();
    if (s.dec_th.joinable()) s.dec_th.join();
    if (s.show_th.joinable()) s.show_th.join();
    s.spk.close();
    s.vdec.close();
    s.adec.close();
    s.reader.close();
    s.fq.clear();
}

std::string Player::decoder_name() const { return p_->vdec.name(); }

std::string Player::error() const {
    std::lock_guard<std::mutex> lk(p_->err_mu);
    return p_->err;
}

}  // namespace mk
