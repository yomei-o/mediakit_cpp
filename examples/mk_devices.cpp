// List cameras, microphones and speakers, and open the first camera to show its mode.
//   mk_devices [--grab]   (--grab: capture 30 frames and report the measured fps)
#include <algorithm>
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
    show("cameras", mk::list_cameras());
    show("microphones", mk::list_microphones());
    show("speakers", mk::list_speakers());
    if (argc > 1 && !std::strcmp(argv[1], "--grab")) {
        mk::Camera cam;
        if (!cam.open({})) {
            std::printf("camera open failed: %s\n", cam.error().c_str());
            return 1;
        }
        std::printf("camera '%s' %dx%d @ %.2f fps\n", cam.name().c_str(), cam.width(), cam.height(), cam.fps());
        int n = 0;
        int64_t t0 = 0, t1 = 0;
        cam.start([&](const mk::VideoFrame& f) {
            if (n == 0) t0 = f.pts_us;
            t1 = f.pts_us;
            ++n;
        });
        for (int i = 0; i < 50 && n < 31; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cam.stop();
        std::printf("frames %d, measured %.1f fps, %s\n", n, n > 1 ? (n - 1) * 1e6 / (double)(t1 - t0) : 0.0, cam.error().c_str());
        mk::Microphone mic;
        if (mic.open({})) {
            int64_t samples = 0;
            int peak = 0;
            mic.start([&](const mk::AudioFrame& a) {
                samples += a.sample_count();
                for (int16_t s : a.samples) peak = std::max(peak, (int)std::abs(s));
            });
            std::this_thread::sleep_for(std::chrono::seconds(1));
            mic.stop();
            std::printf("microphone '%s': %lld samples in 1 s at %d Hz, peak %d\n", mic.name().c_str(), (long long)samples,
                        mic.sample_rate(), peak);
        } else {
            std::printf("microphone open failed: %s\n", mic.error().c_str());
        }
    }
    return 0;
}
