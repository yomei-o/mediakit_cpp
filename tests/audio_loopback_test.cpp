// Speaker -> air -> microphone: play a tone on the default speaker and check the default microphone hears it.
//   audio_loopback_test [freq_hz] [-v]   (default 777 Hz: away from the 50 / 60 Hz mains hum harmonics)
// Needs a real speaker and microphone near each other (a webcam microphone next to the PC speaker is enough).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "mk_io.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

const double kPi = 3.14159265358979323846;

// Power of one frequency in x[0..n) (Goertzel), normalised by n^2 so windows of the same size compare.
double tone_power(const int16_t* x, int n, double f, int rate) {
    const double c = 2 * std::cos(2 * kPi * f / rate);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) {
        const double s = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / ((double)n * n);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    double freq = 777;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v")) verbose = true;
        else freq = std::atof(argv[i]);
    }
    const int rate = 48000;
    const double tone_secs = 3.0;

    std::mutex mu;
    std::vector<int16_t> rec;    // mono
    std::vector<int64_t> rec_t;  // capture time of every 10 ms block
    mk::Microphone mic;
    if (!mic.open({"", rate, 1})) return std::printf("FAIL: microphone open: %s\n", mic.error().c_str()), 1;
    mk::Speaker spk;
    if (!spk.open({"", rate, 2}) || !spk.start()) return std::printf("FAIL: speaker open: %s\n", spk.error().c_str()), 1;
    std::printf("microphone '%s' %d Hz, speaker default, tone %.0f Hz for %.1f s\n", mic.name().c_str(), mic.sample_rate(), freq,
                tone_secs);
    mic.start([&](const mk::AudioFrame& a) {
        std::lock_guard<std::mutex> lk(mu);
        for (int i = 0; i < a.sample_count(); ++i) {
            if (rec.size() % (rate / 100) == 0) rec_t.push_back(a.pts_us + (int64_t)i * 1000000 / rate);
            rec.push_back(a.samples[(size_t)i * a.channels]);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // silence: the baseline
    const int64_t tone_written = mk::now_us();
    int64_t pts = 0;
    const int block = rate / 50;  // 20 ms
    for (int b = 0; b < (int)(tone_secs * 50); ++b) {
        mk::AudioFrame f;
        f.sample_rate = rate;
        f.channels = 2;
        f.pts_us = pts;
        for (int i = 0; i < block; ++i) {
            const int16_t v = (int16_t)(16000 * std::sin(2 * kPi * freq * (b * block + i) / rate));
            f.samples.push_back(v);
            f.samples.push_back(v);
        }
        spk.write(f);
        pts += f.duration_us();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds((int)(tone_secs * 1000) + 1500));
    mic.stop();
    spk.stop();

    std::lock_guard<std::mutex> lk(mu);
    // 100 ms windows. Silence = before the tone was written; tone = the middle of the tone, whatever the output latency.
    const int win = rate / 10;
    double base = 0, tone = 0, other = 0;
    int nb = 0, nt = 0;
    int64_t heard_from = -1, heard_to = -1;
    std::vector<double> powers;
    for (size_t w = 0; (w + 1) * win <= rec.size(); ++w) powers.push_back(tone_power(&rec[w * win], win, freq, rate));
    for (size_t w = 0; w < powers.size(); ++w) {
        const int64_t t = rec_t[w * win / (rate / 100)];
        const double p = powers[w];
        if (t + 100000 < tone_written) base += p, ++nb;
        if (t > tone_written + 1000000 && t < tone_written + (int64_t)(tone_secs * 1e6) - 500000) {
            tone += p, ++nt;
            other += (tone_power(&rec[w * win], win, freq * 0.8, rate) + tone_power(&rec[w * win], win, freq * 1.2, rate)) / 2;
        }
        if (verbose) std::printf("  %5.2f s  %8.0f Hz %12.1f\n", (t - rec_t[0]) / 1e6, freq, p);
    }
    if (!nb || !nt) return std::printf("FAIL: too little audio recorded (%zu samples)\n", rec.size()), 1;
    base /= nb;
    tone /= nt;
    other /= nt;
    for (size_t w = 0; w < powers.size(); ++w) {  // where the tone was loud: start latency and length
        if (powers[w] < tone / 10) continue;
        const int64_t t = rec_t[w * win / (rate / 100)];
        if (heard_from < 0) heard_from = t;
        heard_to = t + 100000;
    }

    int failures = 0;
    const double rise_db = 10 * std::log10(tone / std::max(base, 1e-9));
    const double sel_db = 10 * std::log10(tone / std::max(other, 1e-9));
    const double heard_secs = (heard_to - heard_from) / 1e6;
    std::printf("recorded %.2f s (%.1f%% of real time)\n", rec.size() / (double)rate,
                100.0 * rec.size() / rate / ((rec_t.back() - rec_t.front()) / 1e6 + 0.01));
    std::printf("%.0f Hz: silence %.1f -> tone %.1f (+%.1f dB), vs neighbouring frequencies +%.1f dB\n", freq, base, tone, rise_db,
                sel_db);
    std::printf("heard %.2f s after writing, for %.2f s (played %.1f s)\n", (heard_from - tone_written) / 1e6, heard_secs, tone_secs);
    if (rise_db < 20) std::printf("FAIL: the tone is not louder than the silence (speaker muted or too far from the microphone?)\n"), ++failures;
    if (sel_db < 10) std::printf("FAIL: the recording is not the tone\n"), ++failures;
    if (std::fabs(heard_secs - tone_secs) > 0.5) std::printf("FAIL: heard for %.2f s instead of %.1f s\n", heard_secs, tone_secs), ++failures;
    std::printf(failures ? "FAILED %d\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
