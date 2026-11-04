// Record camera + microphone (or re-encode an MP4) to MP4 from the console (no window).
//   mk_record out.mp4 [seconds=5] [--size 1280x720] [--fps 30] [--bitrate 4000000] [--sw] [--no-audio]
//             [--input in.mp4]   take the input from a file instead of the camera
//             [--h264 out.h264]  also send the stream to a PacketSink that writes raw Annex-B H.264
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "mk_pipeline.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

// A PacketSink: the simplest possible "other output" - a raw H.264 elementary stream.
// Network services (RTSP / WebRTC / a cloud uploader) plug in the same way.
class H264FileSink : public mk::PacketSink {
public:
    explicit H264FileSink(std::string path) : path_(std::move(path)) {}
    bool open(const mk::StreamInfo& info) override {
        fp_ = std::fopen(path_.c_str(), "wb");
        std::printf("sink open: %dx%d @%d fps, audio %d Hz\n", info.width, info.height, info.fps, info.sample_rate);
        return fp_ != nullptr;
    }
    bool write(const mk::Packet& p) override {
        if (p.type != mk::MediaType::Video) return true;
        if (!packets_ && !p.keyframe) std::printf("FAIL: the sink's first packet is not a keyframe\n");
        ++packets_;
        return std::fwrite(p.data.data(), 1, p.data.size(), fp_) == p.data.size();
    }
    void close() override {
        if (fp_) std::fclose(fp_), fp_ = nullptr;
        std::printf("sink closed: %d video packets\n", packets_);
    }

private:
    std::string path_;
    FILE* fp_ = nullptr;
    int packets_ = 0;
};

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) {
        std::printf(
            "usage: mk_record out.mp4 [seconds] [--size WxH] [--fps N] [--bitrate N] [--sw] [--encoder NAME] [--no-audio]\n"
            "                 [--test] [--test-camera] [--input in.mp4] [--h264 out.h264]\n");
        return 2;
    }
    std::string path = argv[1];
    double seconds = 5;
    std::string h264_path;
    mk::RecorderConfig cfg;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) {
            std::sscanf(argv[++i], "%dx%d", &cfg.camera.width, &cfg.camera.height);
        } else if (a == "--fps" && i + 1 < argc) {
            cfg.camera.fps = std::atoi(argv[++i]);
        } else if (a == "--bitrate" && i + 1 < argc) {
            cfg.video.bitrate = std::atoi(argv[++i]);
        } else if (a == "--sw") {
            cfg.video.hardware = false;
        } else if (a == "--encoder" && i + 1 < argc) {
            cfg.video.encoder = argv[++i];
        } else if (a == "--test") {
            cfg.camera.device_id = cfg.microphone.device_id = "test";
        } else if (a == "--test-camera") {
            cfg.camera.device_id = "test";
        } else if (a == "--input" && i + 1 < argc) {
            cfg.input_file = argv[++i];
        } else if (a == "--h264" && i + 1 < argc) {
            h264_path = argv[++i];
        } else if (a == "--no-audio") {
            cfg.audio = false;
        } else {
            seconds = std::atof(a.c_str());
        }
    }
    int previews = 0;
    mk::Recorder rec;
    if (!rec.open(cfg, [&](const mk::VideoFrame&) { ++previews; })) {
        std::printf("open failed: %s\n", rec.error().c_str());
        return 1;
    }
    std::printf("input: %s %dx%d\naudio: %s\n", rec.input_name().c_str(), rec.input_width(), rec.input_height(),
                rec.audio_name().empty() ? "(none)" : rec.audio_name().c_str());
    if (!rec.error().empty()) std::printf("note: %s\n", rec.error().c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the camera settle
    std::shared_ptr<H264FileSink> sink;
    if (!h264_path.empty()) {
        sink = std::make_shared<H264FileSink>(h264_path);
        if (!rec.add_sink(sink)) std::printf("add_sink failed: %s\n", rec.error().c_str());
    }
    if (!rec.start_recording(path)) {
        std::printf("start failed: %s\n", rec.error().c_str());
        return 1;
    }
    std::printf("recording %s for %.1f s with %s ...\n", path.c_str(), seconds, rec.stats().encoder.c_str());
    const int64_t end = mk::now_us() + (int64_t)(seconds * 1e6);
    while (mk::now_us() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        mk::RecorderStats st = rec.stats();
        std::printf("  %.1f s  %lld frames  %.0f KB  input %.1f fps  dropped %lld\n", st.duration_us / 1e6,
                    (long long)st.video_frames, st.bytes / 1024.0, st.input_fps, (long long)st.dropped_frames);
    }
    bool ok = rec.stop_recording();
    if (sink) rec.remove_sink(sink);
    mk::RecorderStats st = rec.stats();
    std::printf("%s: %.2f s, %lld frames, %.0f KB, preview frames %d%s%s\n", ok ? "done" : "FAILED", st.duration_us / 1e6,
                (long long)st.video_frames, st.bytes / 1024.0, previews, rec.error().empty() ? "" : ", ",
                rec.error().c_str());
    rec.close();
    return ok ? 0 : 1;
}
