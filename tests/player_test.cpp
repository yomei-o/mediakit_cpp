// Player pipeline without a window: play a file, check every frame arrives on time and in order.
//   player_test file.mp4
#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "mk_pipeline.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: player_test file.mp4\n");
        return 2;
    }
    if (argc > 2 && std::string(argv[2]) == "--dump") {  // packet timeline of the file
        mk::Mp4Reader r;
        if (!r.open(argv[1])) return std::printf("%s\n", r.error().c_str()), 1;
        mk::AudioDecoder ad;
        ad.open(r.info().sample_rate, r.info().channels, r.info().audio_extradata);
        mk::Packet p;
        int64_t vmin = INT64_MAX, vmax = 0, amin = INT64_MAX, amax = 0, asamples = 0, prev_a = -1, max_agap = 0;
        int vn = 0, an = 0;
        int order = 0;
        while (r.read(p)) {
            if (getenv("MK_DEBUG") && order++ < 60)
                std::printf("  %c pts %7.3f dts %7.3f %s %zu B\n", p.type == mk::MediaType::Video ? 'V' : 'A', p.pts_us / 1e6,
                            p.dts_us / 1e6, p.keyframe ? "K" : " ", p.data.size());
            if (p.type == mk::MediaType::Video) {
                ++vn, vmin = std::min(vmin, p.pts_us), vmax = std::max(vmax, p.pts_us);
            } else {
                ++an, amin = std::min(amin, p.pts_us), amax = std::max(amax, p.pts_us);
                if (prev_a >= 0) max_agap = std::max(max_agap, p.pts_us - prev_a);
                prev_a = p.pts_us;
                std::vector<mk::AudioFrame> af;
                ad.decode(p, af);
                for (auto& a : af) {
                    asamples += a.sample_count();
                    if (an == 1) std::printf("first audio frame: %d Hz x%d, %d samples (info %d Hz x%d)\n", a.sample_rate, a.channels,
                                             a.sample_count(), r.info().sample_rate, r.info().channels);
                }
            }
        }
        std::printf("video %d pkts %.3f..%.3f s; audio %d pkts %.3f..%.3f s, max step %.1f ms, %lld samples = %.3f s\n", vn,
                    vmin / 1e6, vmax / 1e6, an, amin / 1e6, amax / 1e6, max_agap / 1e3, (long long)asamples,
                    asamples / (double)std::max(1, r.info().sample_rate));
        return 0;
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
    std::printf("file %dx%d %.2f fps, audio %s, %.2f s, decoder %s%s%s\n", in.width, in.height, in.fps, in.has_audio ? "yes" : "no",
                in.duration_us / 1e6, pl.decoder_name().c_str(), pl.error().empty() ? "" : "; ", pl.error().c_str());
    const int64_t t0 = mk::now_us();
    pl.play();
    int64_t next_dbg = t0;
    while (!pl.finished() && mk::now_us() - t0 < in.duration_us + 5000000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (getenv("MK_DEBUG") && mk::now_us() >= next_dbg) {
            next_dbg += 500000;
            std::printf("  wall %.2f  position %.2f\n", (mk::now_us() - t0) / 1e6, pl.position_us() / 1e6);
        }
    }
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
        if (getenv("MK_DEBUG") && i % 15 == 0) std::printf("  frame %zu pts %.3f shown at %.3f\n", i, shown[i].first / 1e6, (shown[i].second - t0) / 1e6);
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
