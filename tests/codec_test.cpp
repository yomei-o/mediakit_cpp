// Codec layer round trip without any device:
//   synthetic NV12 frames + 440 Hz tone -> H.264 / AAC -> MP4 -> read -> decode -> compare.
//   codec_test [out.mp4] [--encoder NAME] [--sw] [--hwdec]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mk_codec.h"

static int failures = 0;
#define CHECK(c, ...)                         \
    do {                                      \
        if (!(c)) {                           \
            std::printf("FAIL: " __VA_ARGS__); \
            std::printf("\n");                \
            ++failures;                       \
        }                                     \
    } while (0)

static const double kPi = 3.14159265358979323846;

// Moving gradient + a white square that travels; deterministic per frame index.
static void make_frame(mk::VideoFrame& f, int w, int h, int i) {
    f.alloc(w, h, mk::PixelFormat::NV12);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) f.data[0][y * f.stride[0] + x] = (uint8_t)(16 + ((x + y + i * 4) % 200));
    const int sx = (i * 7) % (w - 64), sy = h / 3;
    for (int y = sy; y < sy + 64; ++y)
        for (int x = sx; x < sx + 64; ++x) f.data[0][y * f.stride[0] + x] = 235;
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            f.data[1][y * f.stride[1] + 2 * x] = (uint8_t)(128 + 60 * std::sin((x + i) * 0.05));
            f.data[1][y * f.stride[1] + 2 * x + 1] = (uint8_t)(128 + 60 * std::cos((y + i) * 0.05));
        }
}

static double psnr_y(const mk::VideoFrame& a, const mk::VideoFrame& b) {
    if (a.width != b.width || a.height != b.height || a.empty() || b.empty()) return 0;  // not comparable
    double se = 0;
    for (int y = 0; y < a.height; ++y)
        for (int x = 0; x < a.width; ++x) {
            double d = (double)a.data[0][y * a.stride[0] + x] - b.data[0][y * b.stride[0] + x];
            se += d * d;
        }
    double mse = se / ((double)a.width * a.height);
    return mse <= 0 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}

int main(int argc, char** argv) {
    std::string path = "codec_test.mp4", encoder = "auto";
    bool hw = true, hwdec = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--encoder" && i + 1 < argc) encoder = argv[++i];
        else if (a == "--sw") hw = false;
        else if (a == "--hwdec") hwdec = true;
        else path = a;
    }
    const int W = 640, H = 360, FPS = 30, N = 90, RATE = 48000;

    mk::VideoEncoderConfig vc;
    vc.width = W, vc.height = H, vc.fps = FPS, vc.bitrate = 3000000, vc.gop = 30, vc.encoder = encoder, vc.hardware = hw;
    mk::VideoEncoder venc;
    if (!venc.open(vc)) {
        std::printf("FAIL: video encoder: %s\n", venc.error().c_str());
        return 1;
    }
    std::printf("video encoder: %s\n", venc.name().c_str());
    mk::AudioEncoder aenc;
    if (!aenc.open({RATE, 2, 128000})) {
        std::printf("FAIL: audio encoder: %s\n", aenc.error().c_str());
        return 1;
    }
    mk::Mp4Writer writer;
    CHECK(writer.open(path, &venc, &aenc), "writer open: %s", writer.error().c_str());

    // Live path: decode straight from the encoder output (what a streaming receiver sees).
    mk::VideoDecoder live;
    CHECK(live.open(hwdec), "decoder open");
    std::printf("video decoder: %s\n", live.name().c_str());
    int live_frames = 0, keyframes = 0;
    double min_psnr_live = 99;
    std::vector<mk::VideoFrame> originals;

    int64_t t0 = 1000000;  // capture clock does not start at 0
    int64_t audio_samples = 0;
    std::vector<mk::Packet> pk;
    for (int i = 0; i < N; ++i) {
        mk::VideoFrame f;
        make_frame(f, W, H, i);
        f.pts_us = t0 + (int64_t)i * 1000000 / FPS;
        originals.push_back(f);
        pk.clear();
        CHECK(venc.encode(f, pk), "encode %d: %s", i, venc.error().c_str());
        // one video frame worth of audio, in uneven chunks like a microphone
        int64_t until = (int64_t)(i + 1) * RATE / FPS;
        while (audio_samples < until) {
            int n = (int)std::min<int64_t>(until - audio_samples, 700);
            mk::AudioFrame af;
            af.sample_rate = RATE, af.channels = 2, af.pts_us = t0 + audio_samples * 1000000 / RATE;
            af.samples.resize((size_t)n * 2);
            for (int k = 0; k < n; ++k) {
                int16_t v = (int16_t)(10000 * std::sin(2 * kPi * 440 * (double)(audio_samples + k) / RATE));
                af.samples[2 * k] = af.samples[2 * k + 1] = v;
            }
            audio_samples += n;
            CHECK(aenc.encode(af, pk), "aac: %s", aenc.error().c_str());
        }
        for (auto& p : pk) {
            CHECK(writer.write(p), "write: %s", writer.error().c_str());
            if (p.type != mk::MediaType::Video) continue;
            keyframes += p.keyframe;
            std::vector<mk::VideoFrame> out;
            CHECK(live.decode(p, out), "live decode: %s", live.error().c_str());
            for (auto& d : out) {
                int idx = (int)(((d.pts_us - t0) * FPS + 500000) / 1000000);
                if (getenv("MK_DEBUG") && live_frames < 6) {
                    int best = -1; double bp = 0;
                    for (int k = 0; k < (int)originals.size(); ++k) { double q = psnr_y(originals[k], d); if (q > bp) bp = q, best = k; }
                    std::printf("  live frame pts %lld -> idx %d (psnr %.1f), best match %d (%.1f) size %dx%d fmt %s\n", (long long)(d.pts_us - t0), idx,
                                idx < (int)originals.size() ? psnr_y(originals[idx], d) : 0.0, best, bp, d.width, d.height, mk::pixel_format_name(d.format));
                }
                if (idx >= 0 && idx < (int)originals.size()) min_psnr_live = std::min(min_psnr_live, psnr_y(originals[idx], d));
                ++live_frames;
            }
        }
    }
    pk.clear();
    CHECK(venc.flush(pk), "flush video");
    CHECK(aenc.flush(pk), "flush audio");
    for (auto& p : pk) {
        CHECK(writer.write(p), "write: %s", writer.error().c_str());
        if (p.type == mk::MediaType::Video) {
            std::vector<mk::VideoFrame> out;
            live.decode(p, out);
            live_frames += (int)out.size();
        }
    }
    {
        std::vector<mk::VideoFrame> out;
        live.flush(out);
        live_frames += (int)out.size();
    }
    CHECK(writer.close(), "writer close: %s", writer.error().c_str());
    std::printf("encoded: keyframes %d, live-decoded %d frames, min PSNR(Y) %.1f dB\n", keyframes, live_frames, min_psnr_live);
    CHECK(live_frames == N, "live decoded %d of %d frames", live_frames, N);
    CHECK(keyframes >= N / 30, "keyframes %d", keyframes);
    CHECK(min_psnr_live > 30, "live PSNR %.1f", min_psnr_live);

    // File path.
    mk::Mp4Reader reader;
    if (!reader.open(path)) {
        std::printf("FAIL: reader: %s\n", reader.error().c_str());
        return 1;
    }
    const mk::MediaInfo& info = reader.info();
    std::printf("file: %dx%d %.2f fps, audio %d Hz x%d, %.2f s\n", info.width, info.height, info.fps, info.sample_rate,
                info.channels, info.duration_us / 1e6);
    CHECK(info.has_video && info.width == W && info.height == H, "video info");
    CHECK(info.has_audio && info.sample_rate == RATE && info.channels == 2, "audio info");
    CHECK(std::fabs(info.fps - FPS) < 0.5, "fps %.2f", info.fps);
    CHECK(std::fabs(info.duration_us / 1e6 - 3.0) < 0.2, "duration %.3f", info.duration_us / 1e6);

    mk::VideoDecoder vdec;
    mk::AudioDecoder adec;
    CHECK(vdec.open(hwdec), "vdec");
    CHECK(adec.open(info.sample_rate, info.channels, info.audio_extradata), "adec: %s", adec.error().c_str());
    mk::FrameConverter conv;
    int vframes = 0;
    double min_psnr = 99;
    std::vector<int16_t> pcm;
    mk::Packet p;
    bool first_video = true;
    while (reader.read(p)) {
        if (p.type == mk::MediaType::Video) {
            if (first_video) CHECK(p.keyframe && p.pts_us == 0, "first video packet key=%d pts=%lld", p.keyframe, (long long)p.pts_us);
            first_video = false;
            std::vector<mk::VideoFrame> out;
            CHECK(vdec.decode(p, out), "decode: %s", vdec.error().c_str());
            for (auto& d : out) {
                int idx = (int)((d.pts_us * FPS + 500000) / 1000000);
                if (idx < N) min_psnr = std::min(min_psnr, psnr_y(originals[idx], d));
                if (vframes == 45) {  // RGB conversion check on one frame
                    mk::VideoFrame bgra, small;
                    CHECK(conv.convert(d, bgra, mk::PixelFormat::BGRA), "to BGRA");
                    CHECK(conv.convert(d, small, mk::PixelFormat::RGB24, 320, 180), "to RGB 320x180");
                    // same conversion of the original NV12 frame: the decoded RGB must match it closely
                    mk::VideoFrame ref;
                    mk::FrameConverter conv2;
                    CHECK(conv2.convert(originals[idx], ref, mk::PixelFormat::BGRA), "original to BGRA");
                    double diff = 0;
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W * 4; ++x)
                            diff += std::abs((int)bgra.data[0][y * bgra.stride[0] + x] - (int)ref.data[0][y * ref.stride[0] + x]);
                    diff /= (double)W * H * 4;
                    const uint8_t* px = bgra.data[0] + (H / 3 + 32) * bgra.stride[0] + ((idx * 7) % (W - 64) + 32) * 4;
                    std::printf("frame %d: BGRA mean |diff| vs original %.2f, square pixel BGRA = %d %d %d\n", idx, diff, px[0], px[1],
                                px[2]);
                    CHECK(diff < 4, "RGB differs from the original (%.2f)", diff);
                    CHECK(px[0] * 0.114 + px[1] * 0.587 + px[2] * 0.299 > 200, "square not bright in RGB");
                    CHECK(small.width == 320 && small.height == 180, "scaled size");
                }
                ++vframes;
            }
        } else {
            std::vector<mk::AudioFrame> out;
            CHECK(adec.decode(p, out), "aac decode: %s", adec.error().c_str());
            for (auto& a : out)
                for (int k = 0; k < a.sample_count(); ++k) pcm.push_back(a.samples[(size_t)k * a.channels]);
        }
    }
    {
        std::vector<mk::VideoFrame> out;
        vdec.flush(out);
        for (auto& d : out) {
            int idx = (int)((d.pts_us * FPS + 500000) / 1000000);
            if (idx < N) min_psnr = std::min(min_psnr, psnr_y(originals[idx], d));
            ++vframes;
        }
    }
    // tone check: correlate the middle second with 440 Hz
    double c = 0, s = 0, e = 0;
    const size_t a0 = RATE / 2, a1 = std::min(pcm.size(), (size_t)RATE * 3 / 2);
    for (size_t k = a0; k < a1; ++k) {
        double t = 2 * kPi * 440 * (double)k / RATE;
        c += pcm[k] * std::cos(t), s += pcm[k] * std::sin(t), e += (double)pcm[k] * pcm[k];
    }
    double ratio = e > 0 ? 2 * (c * c + s * s) / ((double)(a1 - a0) * e) : 0;  // 1.0 = pure 440 Hz
    std::printf("decoded from file: %d video frames, min PSNR(Y) %.1f dB; %zu audio samples, 440 Hz energy ratio %.3f\n",
                vframes, min_psnr, pcm.size(), ratio);
    CHECK(vframes == N, "file decoded %d of %d frames", vframes, N);
    CHECK(min_psnr > 30, "file PSNR %.1f", min_psnr);
    CHECK(pcm.size() > (size_t)RATE * 29 / 10, "audio length %zu", pcm.size());
    CHECK(ratio > 0.95, "audio is not the 440 Hz tone (%.3f)", ratio);

    std::printf(failures ? "FAILED %d\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
