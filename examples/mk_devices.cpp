// List cameras, microphones and speakers, and open the first camera to show its mode.
//   mk_devices [--grab]   (--grab: capture 30 frames and report the measured fps)
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "mk_io.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    auto show = [](const char* what, const std::vector<mk::DeviceInfo>& v) {
        std::printf("%s: %zu\n", what, v.size());
        for (size_t i = 0; i < v.size(); ++i) std::printf("  [%zu] %s\n       id=%s\n", i, v[i].name.c_str(), v[i].id.c_str());
    };
    auto cams = mk::list_cameras();
    show("cameras", cams);
    for (auto& c : cams) {
        std::printf("  modes of %s:\n", c.name.c_str());
        for (auto& m : mk::list_camera_modes(c.id)) std::printf("    %4dx%-4d %6.2f fps  %s\n", m.width, m.height, m.fps, m.format.c_str());
    }
    show("microphones", mk::list_microphones());
    show("speakers", mk::list_speakers());
    if (argc > 1 && !std::strcmp(argv[1], "--grab")) {
        mk::Camera cam;
        if (!cam.open({})) {
            std::printf("camera open failed: %s\n", cam.error().c_str());
            return 1;
        }
        std::printf("camera '%s' %dx%d @ %.2f fps %s\n", cam.name().c_str(), cam.width(), cam.height(), cam.fps(), cam.format().c_str());
        int n = 0;
        int64_t t0 = 0, t1 = 0;
        cam.start([&](const mk::VideoFrame& f) {
            if (n == 0) t0 = f.pts_us;
            t1 = f.pts_us;
            ++n;
        });
        // Android cameras can take several seconds to produce the first frame, so wait a while.
        for (int i = 0; i < 150 && n < 31; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cam.stop();
        std::printf("frames %d, measured %.1f fps, %s\n", n, n > 1 ? (n - 1) * 1e6 / (double)(t1 - t0) : 0.0, cam.error().c_str());
        mk::Microphone mic;
        if (mic.open({})) {
            std::atomic<int64_t> samples{0}, first_pts{-1}, last_pts{0};
            std::atomic<int> peak{0}, chunks{0};
            mic.start([&](const mk::AudioFrame& a) {
                if (first_pts < 0) first_pts = a.pts_us;
                last_pts = a.pts_us;
                samples += a.sample_count();
                ++chunks;
                for (int16_t s : a.samples) peak = std::max(peak.load(), (int)std::abs(s));
            });
            const int64_t t0 = mk::now_us();
            for (int sec = 1; sec <= 4; ++sec) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::printf("  %.2f s: %lld samples in %d chunks, capture time %.2f s\n", (mk::now_us() - t0) / 1e6,
                            (long long)samples.load(), chunks.load(), (last_pts - first_pts) / 1e6);
            }
            mic.stop();
            std::printf("microphone '%s': %lld samples in 4 s at %d Hz (expected %d), peak %d\n", mic.name().c_str(),
                        (long long)samples.load(), mic.sample_rate(), mic.sample_rate() * 4, peak.load());
        } else {
            std::printf("microphone open failed: %s\n", mic.error().c_str());
        }
    }
    return 0;
}
