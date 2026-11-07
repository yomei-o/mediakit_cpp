// Recorder -> MP4 -> Player, without devices: record 3 s of the test camera + test tone, check the file,
// play it back and check every frame arrives on time.
//   record_play_test [out.mp4] [--hw|--sw]   (the browser version records into memory)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "mk_pipeline.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Playback follows the speaker, which the browser starts only after a click on the page.
EM_ASYNC_JS(void, wait_for_audio_permission, (), {
    const probe = new AudioContext();
    await new Promise(r => setTimeout(r, 100));
    const ok = probe.state == "running";
    probe.close();
    if (ok) return;
    (Module.print || console.log)("ページをクリックするとテストが始まります (click the page to start)");
    await new Promise(r => document.addEventListener("pointerdown", r, { once: true, capture: true }));
});
#endif

namespace {

// The browser delivers frames and audio only while the program sleeps through emscripten_sleep.
void wait_ms(int ms) {
#ifdef __EMSCRIPTEN__
    emscripten_sleep(ms);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
#ifdef __EMSCRIPTEN__
    std::string path = "/tmp/record_play_test.mp4";
    wait_for_audio_permission();
#else
    std::string path = "record_play_test.mp4";
#endif
    mk::RecorderConfig cfg;
    cfg.camera.device_id = "test";
    cfg.camera.width = 640, cfg.camera.height = 480, cfg.camera.fps = 30;
    cfg.microphone.device_id = "test";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--sw")) cfg.video.hardware = false;
        else if (!std::strcmp(argv[i], "--hw")) cfg.video.hardware = true;
        else path = argv[i];
    }
    const double rec_secs = 3.0;
    int failures = 0;

    // ---- record
    {
        mk::Recorder rec;
        int previews = 0;
        if (!rec.open(cfg, [&](const mk::VideoFrame&) { ++previews; })) return std::printf("FAIL: recorder open: %s\n", rec.error().c_str()), 1;
        wait_ms(500);
        if (!rec.start_recording(path)) return std::printf("FAIL: start_recording: %s\n", rec.error().c_str()), 1;
        wait_ms((int)(rec_secs * 1000));
        mk::RecorderStats st = rec.stats();
        if (!rec.stop_recording()) std::printf("FAIL: stop_recording: %s\n", rec.error().c_str()), ++failures;
        std::printf("recorded %s: %lld frames, %.2f s, %.1f KB, dropped %lld, encoder %s, input %.1f fps, previews %d%s%s\n",
                    path.c_str(), (long long)st.video_frames, st.duration_us / 1e6, st.bytes / 1024.0, (long long)st.dropped_frames,
                    st.encoder.c_str(), st.input_fps, previews, rec.error().empty() ? "" : ", note: ", rec.error().c_str());
        if (st.video_frames < rec_secs * 30 * 0.8) std::printf("FAIL: too few frames encoded\n"), ++failures;
        if (st.dropped_frames > 5) std::printf("FAIL: frames dropped\n"), ++failures;
    }

    // ---- the file
    mk::MediaInfo in;
    {
        mk::Mp4Reader r;
        if (!r.open(path)) return std::printf("FAIL: reading the file: %s\n", r.error().c_str()), 1;
        in = r.info();
        mk::Packet p;
        int vn = 0, an = 0;
        bool first_key = false;
        while (r.read(p)) {
            if (p.type == mk::MediaType::Video) {
                if (!vn) first_key = p.keyframe;
                ++vn;
            } else {
                ++an;
            }
        }
        std::printf("file: %dx%d %.1f fps, %.2f s, %d video packets, %d audio packets (%d Hz x%d)\n", in.width, in.height, in.fps,
                    in.duration_us / 1e6, vn, an, in.sample_rate, in.channels);
        if (in.width != 640 || in.height != 480) std::printf("FAIL: size\n"), ++failures;
        if (!first_key) std::printf("FAIL: the file does not start with a keyframe\n"), ++failures;
        if (!in.has_audio || an < rec_secs * 46 * 0.8) std::printf("FAIL: audio missing\n"), ++failures;  // AAC: ~47 packets/s at 48 kHz
        if (std::fabs(in.duration_us / 1e6 - rec_secs) > 0.5) std::printf("FAIL: duration\n"), ++failures;
    }

    // ---- play it back
    {
        mk::Player pl;
        std::vector<std::pair<int64_t, int64_t>> shown;  // (frame pts, wall time)
        int bad_format = 0;
        if (!pl.open(path, [&](const mk::VideoFrame& f) {
                if (f.format != mk::PixelFormat::BGRA) ++bad_format;
                shown.push_back({f.pts_us, mk::now_us()});
            }))
            return std::printf("FAIL: player open: %s\n", pl.error().c_str()), 1;
        const int64_t t0 = mk::now_us();
        pl.play();
        while (!pl.finished() && mk::now_us() - t0 < in.duration_us + 5000000) wait_ms(20);
        const double wall = (mk::now_us() - t0) / 1e6;
        const std::string dec = pl.decoder_name(), perr = pl.error();
        pl.close();
        int out_of_order = 0, skipped = 0;
        double max_lag = 0;
        std::string skips;  // where frames were left out (pts of the gaps)
        for (size_t i = 1; i < shown.size(); ++i) {
            const int64_t step = in.fps > 0 ? (int64_t)(1e6 / in.fps) : 33333;
            const int n = (int)std::lround((shown[i].first - shown[i - 1].first) / (double)step) - 1;
            if (n <= 0) continue;
            skipped += n;
            if (skips.size() < 120) skips += " " + std::to_string(n) + "@" + std::to_string(shown[i].first / 1000) + "ms";
        }
        const int64_t first_pts = shown.empty() ? 0 : shown[0].first, first_wall = shown.empty() ? 0 : shown[0].second;
        for (size_t i = 0; i < shown.size(); ++i) {
            if (i && shown[i].first <= shown[i - 1].first) ++out_of_order;
            // after the first frame (audio start-up aside), each one is shown at its own time
            max_lag = std::max(max_lag, std::fabs(((shown[i].second - first_wall) - (shown[i].first - first_pts)) / 1e3));
        }
        const int expected = (int)std::lround(in.duration_us / 1e6 * in.fps);
        std::printf("played with %s: shown %zu of ~%d frames in %.2f s; out of order %d; max |lag| %.0f ms%s%s\n", dec.c_str(),
                    shown.size(), expected, wall, out_of_order, max_lag, perr.empty() ? "" : "; ", perr.c_str());
        if (!shown.empty())
            std::printf("first frame pts %.3f s; skipped %d%s\n", shown[0].first / 1e6, skipped, skips.empty() ? "" : (":" + skips).c_str());
        if ((int)shown.size() < expected * 9 / 10) std::printf("FAIL: too few frames shown\n"), ++failures;
        if (out_of_order) std::printf("FAIL: out of order\n"), ++failures;
        if (bad_format) std::printf("FAIL: not BGRA\n"), ++failures;
        if (std::fabs(wall - in.duration_us / 1e6) > 1.0) std::printf("FAIL: wall time %.2f vs %.2f\n", wall, in.duration_us / 1e6), ++failures;
        if (max_lag > 200) std::printf("FAIL: frames off schedule\n"), ++failures;
    }
    std::printf(failures ? "FAILED %d\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
