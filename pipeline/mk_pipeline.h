// mediakit pipelines: ready-made combinations of io + codec (no UI dependency).
//
//   Recorder  input ──┬─> preview (BGRA, latest frame only) ──> your callback (e.g. Window::show_frame)
//   (camera+mic       └─> H.264 / AAC encoders ──┬─> MP4 file          start_recording / stop_recording
//    or an MP4 file)                             └─> PacketSink(s)     add_sink / remove_sink (streaming, cloud ...)
//
//   Player    MP4 ─> demux ─┬─> H.264 decode ─> BGRA ─> your callback, timed to the audio clock
//                           └─> AAC decode ──────────> speaker
//
//   FileSource  MP4 ─> decode, paced in real time ─> VideoFrame / AudioFrame callbacks, like a camera + mic
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mk_codec.h"
#include "mk_io.h"

namespace mk {

// Where encoded media can go besides an MP4 file. Implement this for a network service, a
// streaming server, a ring buffer ... Calls come from the Recorder's encoder thread.
// (StreamInfo is in mk_codec.h.)

class PacketSink {
public:
    virtual ~PacketSink() = default;
    virtual bool open(const StreamInfo& info) = 0;
    virtual bool write(const Packet& pkt) = 0;  // the first video packet a sink gets is a keyframe
    virtual void close() = 0;
};

// An MP4 file decoded in real time, delivered like camera frames (NV12 / I420) and microphone audio.
class FileSource {
public:
    FileSource();
    ~FileSource();
    bool open(const std::string& path, bool loop = false, bool hardware_decode = true);
    const MediaInfo& info() const;
    // Timestamps are on the mk::now_us() clock, as from a live device.
    bool start(std::function<void(const VideoFrame&)> on_video, std::function<void(const AudioFrame&)> on_audio);
    void stop();
    bool finished() const;  // reached the end (never, when looping)
    void close();
    std::string error() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

struct RecorderConfig {
    // input: the camera + microphone, or an MP4 file when input_file is set
    CameraConfig camera;
    bool audio = true;
    MicrophoneConfig microphone;
    std::string input_file;
    bool input_loop = true;
    bool input_hardware_decode = true;

    VideoEncoderConfig video;   // width/height 0 = the input's size; fps 0 = the input's
    int audio_bitrate = 128000;
    int preview_width = 0;      // 0 = input size (aspect kept when only one is set)
    int preview_height = 0;
    RecorderConfig() { video.width = video.height = 0, video.fps = 0; }
};

struct RecorderStats {
    bool recording = false;       // to a file
    int sinks = 0;
    int64_t duration_us = 0;      // of the current / last recording
    int64_t video_frames = 0;     // encoded since the recording started
    int64_t dropped_frames = 0;   // the encoder fell behind
    int64_t bytes = 0;            // written to the file
    double input_fps = 0;         // measured
    std::string encoder;
};

class Recorder {
public:
    Recorder();
    ~Recorder();
    // Opens the input and starts the preview right away.
    bool open(const RecorderConfig& cfg, std::function<void(const VideoFrame& bgra)> on_preview);
    bool start_recording(const std::string& path);
    bool stop_recording();  // finishes the file
    bool recording() const;
    // Extra outputs; encoding runs while a file or any sink is attached.
    bool add_sink(std::shared_ptr<PacketSink> sink);
    void remove_sink(const std::shared_ptr<PacketSink>& sink);
    RecorderStats stats() const;
    void close();
    int input_width() const;
    int input_height() const;
    std::string input_name() const;   // camera name or file path
    std::string audio_name() const;   // microphone name, "file", or empty
    std::string error() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

class Player {
public:
    Player();
    ~Player();
    // hardware_decode: GPU decoder when there is one (falls back to software by itself)
    bool open(const std::string& path, std::function<void(const VideoFrame& bgra)> on_video,
              int out_width = 0, int out_height = 0, bool hardware_decode = true);
    std::string decoder_name() const;
    bool play();
    void pause();
    bool paused() const;
    bool finished() const;
    int64_t position_us() const;
    const MediaInfo& info() const;
    void close();
    std::string error() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

}  // namespace mk
