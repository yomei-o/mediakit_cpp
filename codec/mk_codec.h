// mediakit codec layer: H.264 / AAC encode and decode, MP4 write and read, pixel conversion.
// The same code on every OS (FFmpeg underneath); only the H.264 encoder backend differs:
//   Windows h264_mf (Media Foundation), Android h264_mediacodec, macOS/iOS h264_videotoolbox,
//   Linux h264_vaapi / libopenh264.  "auto" picks the first one that opens.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "mk_types.h"

namespace mk {

// Android only: hand FFmpeg the app's JavaVM so MediaCodec can be reached (call it once at
// startup, e.g. from JNI_OnLoad). Without it H.264 falls back to software everywhere.
// Returns false on the other systems, where there is nothing to do.
bool set_java_vm(void* vm);

struct VideoEncoderConfig {
    int width = 1280, height = 720;
    int fps = 30;
    int bitrate = 2000000;  // bit/s
    int gop = 30;           // frames between keyframes
    std::string encoder = "auto";  // or an FFmpeg encoder name, e.g. "h264_mf"
    bool hardware = true;          // prefer a hardware encoder when the backend has both
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    bool open(const VideoEncoderConfig& cfg);
    // Any PixelFormat / size is accepted; it is converted to what the encoder wants.
    bool encode(const VideoFrame& frame, std::vector<Packet>& out);
    bool flush(std::vector<Packet>& out);
    void request_keyframe();
    void close();
    const VideoEncoderConfig& config() const;
    std::string name() const;                         // encoder actually used
    const std::vector<uint8_t>& extradata() const;    // SPS+PPS (Annex-B) once known
    const std::string& error() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

struct AudioEncoderConfig {
    int sample_rate = 48000;
    int channels = 2;
    int bitrate = 128000;
};

class AudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();
    bool open(const AudioEncoderConfig& cfg);
    // Frames of any length / rate / channel count; buffered into AAC frames.
    bool encode(const AudioFrame& frame, std::vector<Packet>& out);
    bool flush(std::vector<Packet>& out);
    void close();
    const AudioEncoderConfig& config() const;
    const std::vector<uint8_t>& extradata() const;  // AudioSpecificConfig
    const std::string& error() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    // H.264, Annex-B input. hardware: use the GPU decoder (Windows D3D11VA, Apple VideoToolbox,
    // Android MediaCodec, Linux VA-API) when there is one; otherwise it silently decodes in software.
    bool open(bool hardware = false);
    bool decode(const Packet& pkt, std::vector<VideoFrame>& out);  // out: I420 (software) or NV12 (hardware)
    bool flush(std::vector<VideoFrame>& out);
    void close();
    const std::string& error() const;
    std::string name() const;  // "software" or the hardware API in use ("d3d11va", ...)
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    bool open(int sample_rate, int channels, const std::vector<uint8_t>& extradata);  // AAC
    bool decode(const Packet& pkt, std::vector<AudioFrame>& out);  // out: s16 interleaved
    void close();
    const std::string& error() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// Pixel format conversion and scaling ("RGB conversion"). Keeps its context between calls.
class FrameConverter {
public:
    FrameConverter();
    ~FrameConverter();
    // width/height 0 = keep the source size.
    bool convert(const VideoFrame& in, VideoFrame& out, PixelFormat format, int width = 0, int height = 0);
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// What a stream of packets contains (for writers and other outputs).
struct StreamInfo {
    bool has_video = false;
    int width = 0, height = 0, fps = 0;
    std::vector<uint8_t> video_extradata;  // Annex-B SPS/PPS; may be empty - every keyframe carries them too
    bool has_audio = false;
    int sample_rate = 0, channels = 0, audio_bitrate = 0;
    std::vector<uint8_t> audio_extradata;  // AAC AudioSpecificConfig
};
StreamInfo stream_info_of(const VideoEncoder* video, const AudioEncoder* audio);

struct MediaInfo {
    bool has_video = false;
    int width = 0, height = 0;
    double fps = 0;
    std::vector<uint8_t> video_extradata;  // Annex-B SPS+PPS
    bool has_audio = false;
    int sample_rate = 0, channels = 0;
    std::vector<uint8_t> audio_extradata;
    int64_t duration_us = 0;
};

// MP4 (H.264 + AAC) file writer. Packets go in as produced by the encoders.
class Mp4Writer {
public:
    Mp4Writer();
    ~Mp4Writer();
    // video / audio may be null for an audio-only / video-only file.
    bool open(const std::string& path, const VideoEncoder* video, const AudioEncoder* audio);
    // Without encoders: remuxing packets read from somewhere else (Mp4Reader, a network ...).
    bool open(const std::string& path, const StreamInfo& info);
    bool write(const Packet& pkt);
    bool close();
    const std::string& error() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// MP4 / MOV / TS ... file reader (anything FFmpeg demuxes with H.264 + AAC).
class Mp4Reader {
public:
    Mp4Reader();
    ~Mp4Reader();
    bool open(const std::string& path);
    const MediaInfo& info() const;
    bool read(Packet& pkt);  // false at end of file; video packets come out Annex-B
    bool seek(int64_t pts_us);
    void close();
    const std::string& error() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// Annex-B helpers.
std::vector<uint8_t> h264_extract_parameter_sets(const uint8_t* data, size_t size);  // SPS/PPS NALs of a packet
bool h264_is_keyframe(const uint8_t* data, size_t size);

}  // namespace mk
