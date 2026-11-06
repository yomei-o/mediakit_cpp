// mediakit io layer: camera and microphone capture, speaker output.
// One API; each OS has its own implementation under io/<os>/:
//   Windows  Media Foundation (camera) + WASAPI (microphone, speaker)
//   Linux    V4L2 + ALSA
//   macOS/iOS AVFoundation + AudioQueue
//   Android  NDK Camera2 + AAudio              (todo)
//   Browser  getUserMedia + Web Audio (wasm; callbacks on the main thread while the program waits)
// Callbacks run on a capture thread; copy what you need and return quickly.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mk_types.h"

namespace mk {

std::vector<DeviceInfo> list_cameras();
std::vector<DeviceInfo> list_microphones();
std::vector<DeviceInfo> list_speakers();

struct CameraMode {
    int width = 0, height = 0;
    double fps = 0;
    std::string format;  // "NV12", "YUY2", "MJPG", "H264", ...
};
// What a camera can deliver natively (empty id = first camera).
std::vector<CameraMode> list_camera_modes(const std::string& device_id = "");

struct CameraConfig {
    std::string device_id;  // empty = first camera
    int width = 1280, height = 720;
    int fps = 30;           // the nearest mode the camera has is used
    std::string format;     // prefer this native format ("YUY2", "MJPG", "NV12"); empty = any
};

class Camera {
public:
    Camera();
    ~Camera();
    bool open(const CameraConfig& cfg);
    // Frames come as NV12 (pts_us on the mk::now_us() clock).
    bool start(std::function<void(const VideoFrame&)> on_frame);
    void stop();
    void close();
    int width() const;
    int height() const;
    double fps() const;
    std::string format() const;  // native format in use
    std::string name() const;
    const std::string& error() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

struct MicrophoneConfig {
    std::string device_id;  // empty = default microphone
    int sample_rate = 48000;
    int channels = 2;
};

class Microphone {
public:
    Microphone();
    ~Microphone();
    bool open(const MicrophoneConfig& cfg);
    bool start(std::function<void(const AudioFrame&)> on_audio);  // s16 at the configured rate
    void stop();
    void close();
    int sample_rate() const;
    int channels() const;
    std::string name() const;
    const std::string& error() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

struct SpeakerConfig {
    std::string device_id;  // empty = default output
    int sample_rate = 48000;
    int channels = 2;
};

class Speaker {
public:
    Speaker();
    ~Speaker();
    bool open(const SpeakerConfig& cfg);
    bool start();
    void write(const AudioFrame& frame);  // queued; resampled if the rate differs from the config
    // Media time (pts_us of the frames written) of the sample being heard now; -1 before any.
    int64_t played_pts_us() const;
    int64_t queued_us() const;  // audio written but not yet heard
    void clear();               // drop what is queued (seek / stop)
    void stop();
    void close();
    const std::string& error() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

}  // namespace mk
