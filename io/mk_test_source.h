// Synthetic camera / microphone used when the device id is "test":
// pipelines can be exercised on machines without devices (build servers, CI, emulators).
// Shared by every OS implementation of mk_io.
#pragma once
#include <atomic>
#include <cmath>
#include <functional>
#include <thread>

#include "mk_types.h"

namespace mk {

class TestSource {
public:
    ~TestSource() { stop(); }
    // video: moving gradient with a white box; audio: 440 Hz tone with a click every second.
    void start_video(int w, int h, double fps, std::function<void(const VideoFrame&)> cb) {
        stop();
        running_ = true;
        th_ = std::thread([=] {
            const int64_t t0 = now_us();
            for (int64_t i = 0; running_; ++i) {
                const int64_t due = t0 + (int64_t)(i * 1e6 / fps);
                while (running_ && now_us() < due) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!running_) break;
                VideoFrame f;
                f.alloc(w, h, PixelFormat::NV12);
                f.pts_us = due;
                for (int y = 0; y < h; ++y) {
                    uint8_t* row = f.data[0] + (size_t)y * f.stride[0];
                    for (int x = 0; x < w; ++x) row[x] = (uint8_t)(16 + ((x + y + i * 4) % 200));
                }
                const int bs = h / 6, bx = (int)((i * 7) % std::max(1, w - bs)), by = h / 3;
                for (int y = by; y < by + bs && y < h; ++y)
                    for (int x = bx; x < bx + bs; ++x) f.data[0][(size_t)y * f.stride[0] + x] = 235;
                for (int y = 0; y < h / 2; ++y) {
                    uint8_t* row = f.data[1] + (size_t)y * f.stride[1];
                    for (int x = 0; x < w / 2; ++x) {
                        row[2 * x] = (uint8_t)(128 + 50 * std::sin((x + i) * 0.05));
                        row[2 * x + 1] = (uint8_t)(128 + 50 * std::cos((y + i) * 0.05));
                    }
                }
                cb(f);
            }
        });
    }
    void start_audio(int rate, int ch, std::function<void(const AudioFrame&)> cb) {
        stop();
        running_ = true;
        th_ = std::thread([=] {
            const int64_t t0 = now_us();
            const int chunk = rate / 100;  // 10 ms
            for (int64_t n = 0; running_; n += chunk) {
                const int64_t due = t0 + n * 1000000 / rate;
                while (running_ && now_us() < due + 10000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!running_) break;
                AudioFrame a;
                a.sample_rate = rate, a.channels = ch, a.pts_us = due;
                a.samples.resize((size_t)chunk * ch);
                for (int k = 0; k < chunk; ++k) {
                    const int64_t s = n + k;
                    double v = 0.3 * std::sin(2 * 3.14159265358979 * 440 * (double)s / rate);
                    if (s % rate < rate / 50) v += 0.5 * ((s / 20) % 2 ? 1 : -1);  // click
                    for (int c = 0; c < ch; ++c) a.samples[(size_t)k * ch + c] = (int16_t)(v * 32767 * 0.8);
                }
                cb(a);
            }
        });
    }
    void stop() {
        running_ = false;
        if (th_.joinable()) th_.join();
    }

private:
    std::atomic<bool> running_{false};
    std::thread th_;
};

}  // namespace mk
