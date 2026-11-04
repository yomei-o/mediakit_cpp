// mediakit: types shared by the three layers (io / codec / ui).
#pragma once
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mk {

// One clock for every capture source, so audio and video can be lined up.
inline int64_t now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

enum class PixelFormat { NV12, I420, YUY2, BGRA, RGB24 };

inline const char* pixel_format_name(PixelFormat f) {
    switch (f) {
        case PixelFormat::NV12: return "NV12";
        case PixelFormat::I420: return "I420";
        case PixelFormat::YUY2: return "YUY2";
        case PixelFormat::BGRA: return "BGRA";
        case PixelFormat::RGB24: return "RGB24";
    }
    return "?";
}

// A picture that owns its pixels. data[]/stride[] point into buffer.
struct VideoFrame {
    int width = 0, height = 0;
    PixelFormat format = PixelFormat::NV12;
    std::vector<uint8_t> buffer;
    uint8_t* data[3] = {nullptr, nullptr, nullptr};
    int stride[3] = {0, 0, 0};
    int64_t pts_us = 0;

    VideoFrame() = default;
    VideoFrame(const VideoFrame& o) { *this = o; }
    VideoFrame(VideoFrame&& o) noexcept { *this = std::move(o); }
    VideoFrame& operator=(const VideoFrame& o) {
        if (this == &o) return *this;
        width = o.width, height = o.height, format = o.format, pts_us = o.pts_us;
        buffer = o.buffer;
        rebase(o);
        return *this;
    }
    VideoFrame& operator=(VideoFrame&& o) noexcept {
        if (this == &o) return *this;
        width = o.width, height = o.height, format = o.format, pts_us = o.pts_us;
        const uint8_t* old = o.buffer.data();
        buffer = std::move(o.buffer);  // heap block moves with it, but recompute to be safe
        for (int i = 0; i < 3; ++i) {
            stride[i] = o.stride[i];
            data[i] = o.data[i] ? buffer.data() + (o.data[i] - old) : nullptr;
            o.data[i] = nullptr;
        }
        return *this;
    }

    void alloc(int w, int h, PixelFormat f) {
        width = w, height = h, format = f;
        const int cw = (w + 1) / 2, ch = (h + 1) / 2;
        size_t size = 0;
        switch (f) {
            case PixelFormat::NV12: size = (size_t)w * h + (size_t)cw * 2 * ch; break;
            case PixelFormat::I420: size = (size_t)w * h + (size_t)cw * ch * 2; break;
            case PixelFormat::YUY2: size = (size_t)cw * 4 * h; break;
            case PixelFormat::BGRA: size = (size_t)w * 4 * h; break;
            case PixelFormat::RGB24: size = (size_t)w * 3 * h; break;
        }
        buffer.assign(size, 0);
        uint8_t* p = buffer.data();
        data[0] = p, data[1] = data[2] = nullptr;
        stride[1] = stride[2] = 0;
        switch (f) {
            case PixelFormat::NV12:
                stride[0] = w, stride[1] = cw * 2;
                data[1] = p + (size_t)w * h;
                break;
            case PixelFormat::I420:
                stride[0] = w, stride[1] = stride[2] = cw;
                data[1] = p + (size_t)w * h;
                data[2] = data[1] + (size_t)cw * ch;
                break;
            case PixelFormat::YUY2: stride[0] = cw * 4; break;
            case PixelFormat::BGRA: stride[0] = w * 4; break;
            case PixelFormat::RGB24: stride[0] = w * 3; break;
        }
    }
    bool empty() const { return buffer.empty(); }

private:
    // data[] of a copy point into its own buffer at the same offsets as in `o`.
    void rebase(const VideoFrame& o) {
        for (int i = 0; i < 3; ++i) {
            stride[i] = o.stride[i];
            data[i] = o.data[i] ? buffer.data() + (o.data[i] - o.buffer.data()) : nullptr;
        }
    }

public:
    // Copy a plane from foreign memory (any stride) into this frame.
    void copy_plane(int plane, const uint8_t* src, int src_stride, int row_bytes, int rows) {
        for (int y = 0; y < rows; ++y) std::memcpy(data[plane] + (size_t)y * stride[plane], src + (size_t)y * src_stride, row_bytes);
    }
};

// Signed 16-bit PCM, channels interleaved.
struct AudioFrame {
    int sample_rate = 48000;
    int channels = 2;
    std::vector<int16_t> samples;
    int64_t pts_us = 0;
    int sample_count() const { return channels ? (int)(samples.size() / channels) : 0; }
    int64_t duration_us() const { return sample_rate ? (int64_t)sample_count() * 1000000 / sample_rate : 0; }
};

enum class MediaType { Video, Audio };

// Compressed data. Video is H.264 Annex-B (start codes); audio is raw AAC.
struct Packet {
    MediaType type = MediaType::Video;
    std::vector<uint8_t> data;
    int64_t pts_us = 0;
    int64_t dts_us = 0;
    bool keyframe = false;
};

struct DeviceInfo {
    std::string id;    // pass back to open()
    std::string name;  // for people (UTF-8)
};

}  // namespace mk
