// Put an H.264 elementary stream and an AAC (ADTS) stream into one MP4, without re-encoding.
//   mk_mux out.mp4 video.h264 [audio.aac] [--fps N]
// --fps: frame rate of the .h264 (it has no timestamps). Default: from the SPS timing info,
//        or, when there is audio, frames / audio duration.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "mk_codec.h"

int main(int argc, char** argv) {
    std::string out, vpath, apath;
    double fps = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fps" && i + 1 < argc) fps = std::atof(argv[++i]);
        else if (out.empty()) out = a;
        else if (vpath.empty()) vpath = a;
        else apath = a;
    }
    if (out.empty() || vpath.empty()) {
        std::printf("usage: mk_mux out.mp4 video.h264 [audio.aac] [--fps N]\n");
        return 2;
    }
    mk::Mp4Reader vr, ar;
    if (!vr.open(vpath)) return std::printf("video: %s\n", vr.error().c_str()), 1;
    const bool has_audio = !apath.empty();
    if (has_audio && !ar.open(apath)) return std::printf("audio: %s\n", ar.error().c_str()), 1;
    const mk::MediaInfo vi = vr.info();
    const mk::MediaInfo ai = has_audio ? ar.info() : mk::MediaInfo();

    // Frame count (and audio length) for the automatic frame rate.
    mk::Packet p;
    int64_t frames = 0;
    while (vr.read(p)) frames += p.type == mk::MediaType::Video;
    int64_t audio_us = 0;
    if (has_audio) {
        int64_t n = 0;
        while (ar.read(p)) n += p.type == mk::MediaType::Audio;
        audio_us = n * 1024 * 1000000 / std::max(1, ai.sample_rate);
    }
    if (fps <= 0 && has_audio && audio_us > 0) {
        fps = frames * 1e6 / audio_us;
        double r = std::round(fps);
        if (std::fabs(fps - r) < 0.3) fps = r;  // 29.97 stays, 14.98 -> 15
    }
    if (fps <= 0) fps = vi.fps > 0 ? vi.fps : 30;
    std::printf("video %s: %dx%d, %lld frames at %.3f fps = %.2f s\n", vpath.c_str(), vi.width, vi.height, (long long)frames, fps,
                frames / fps);
    if (has_audio)
        std::printf("audio %s: %d Hz x%d, %.2f s\n", apath.c_str(), ai.sample_rate, ai.channels, audio_us / 1e6);

    vr.open(vpath);
    if (has_audio) ar.open(apath);
    mk::StreamInfo si;
    si.has_video = true;
    si.width = vi.width, si.height = vi.height, si.fps = (int)std::lround(fps);
    si.video_extradata = vi.video_extradata;
    if (has_audio) {
        si.has_audio = true;
        si.sample_rate = ai.sample_rate, si.channels = ai.channels, si.audio_extradata = ai.audio_extradata;
        si.audio_bitrate = 128000;
    }
    mk::Mp4Writer w;
    if (!w.open(out, si)) return std::printf("%s\n", w.error().c_str()), 1;

    // Merge the two by time.
    int64_t vindex = 0;
    mk::Packet vp, ap;
    bool vok = vr.read(vp), aok = has_audio && ar.read(ap);
    auto vtime = [&] { return (int64_t)(vindex * 1e6 / fps); };
    int64_t written = 0;
    while (vok || aok) {
        if (vok && (!aok || vtime() <= ap.pts_us)) {
            vp.pts_us = vp.dts_us = vtime();
            ++vindex;
            if (!w.write(vp)) return std::printf("%s\n", w.error().c_str()), 1;
            vok = vr.read(vp);
        } else {
            if (!w.write(ap)) return std::printf("%s\n", w.error().c_str()), 1;
            aok = ar.read(ap);
        }
        ++written;
    }
    if (!w.close()) return std::printf("%s\n", w.error().c_str()), 1;
    std::printf("wrote %s: %lld packets\n", out.c_str(), (long long)written);
    return 0;
}
