// Player pipeline without a window: play a file, check every frame arrives on time and in order.
//   player_test file.mp4
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "mk_pipeline.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: player_test file.mp4\n");
        return 2;
    }
    mk::Player pl;
    std::mutex mu;
    std::vector<std::pair<int64_t, int64_t>> shown;  // (frame pts, wall time)
    int bad_format = 0;
    if (!pl.open(argv[1], [&](const mk::VideoFrame& f) {
            std::lock_guard<std::mutex> lk(mu);
            if (f.format != mk::PixelFormat::BGRA) ++bad_format;
            shown.push_back({f.pts_us, mk::now_us()});
        })) {
        std::printf("FAIL: open: %s\n", pl.error().c_str());
        return 1;
    }
    const mk::MediaInfo in = pl.info();  // copy: close() resets it
    std::printf("file %dx%d %.2f fps, audio %s, %.2f s%s%s\n", in.width, in.height, in.fps, in.has_audio ? "yes" : "no",
                in.duration_us / 1e6, pl.error().empty() ? "" : "; ", pl.error().c_str());
    const int64_t t0 = mk::now_us();
    pl.play();
    while (!pl.finished() && mk::now_us() - t0 < in.duration_us + 5000000) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const double wall = (mk::now_us() - t0) / 1e6;
    pl.close();
    int failures = 0;
    const int expected = (int)std::lround(in.duration_us / 1e6 * in.fps);
    int out_of_order = 0;
    double max_lag = 0;
    for (size_t i = 0; i < shown.size(); ++i) {
        if (i && shown[i].first <= shown[i - 1].first) ++out_of_order;
        // shown at wall time ~ t0 + pts (allow the audio start-up latency)
        double lag = (shown[i].second - t0 - shown[i].first) / 1e3;
        max_lag = std::max(max_lag, std::fabs(lag));
    }
    std::printf("shown %zu of ~%d frames in %.2f s wall; out of order %d; max |lag| %.0f ms\n", shown.size(), expected, wall,
                out_of_order, max_lag);
    if ((int)shown.size() < expected * 9 / 10) std::printf("FAIL: too few frames\n"), ++failures;
    if (out_of_order) std::printf("FAIL: out of order\n"), ++failures;
    if (bad_format) std::printf("FAIL: not BGRA\n"), ++failures;
    if (std::fabs(wall - in.duration_us / 1e6) > 1.0) std::printf("FAIL: wall time %.2f vs %.2f\n", wall, in.duration_us / 1e6), ++failures;
    if (max_lag > 500) std::printf("FAIL: frames off schedule\n"), ++failures;
    std::printf(failures ? "FAILED %d\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
