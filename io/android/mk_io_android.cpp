// mediakit io for Android: NDK Camera2 + AImageReader, AAudio for the microphone and the speaker.
// Everything here is plain NDK, so no JavaVM is needed (the H.264 MediaCodec encoder in the codec
// layer does need one -- see mk::set_java_vm). The app still has to hold the CAMERA and
// RECORD_AUDIO permissions before open() is called.
#include <aaudio/AAudio.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <dlfcn.h>
#include <media/NdkImageReader.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "mk_io.h"
#include "mk_test_source.h"

namespace mk {
namespace {

// Camera2 delivers its results over binder, which needs a thread pool in this process. A Java
// app already has one; a native-only process does not, and then the camera opens, accepts the
// repeating request and simply never produces a frame. (Available since API 29; before that a
// plain native binary cannot use the camera.)
void start_binder_thread_pool() {
    static const bool once = [] {
        void* lib = dlopen("libbinder_ndk.so", RTLD_NOW);
        if (!lib) return false;
        auto set_max = (void (*)(uint32_t))dlsym(lib, "ABinderProcess_setThreadPoolMaxThreadCount");
        auto start = (void (*)())dlsym(lib, "ABinderProcess_startThreadPool");
        if (set_max) set_max(4);
        if (start) start();
        return start != nullptr;
    }();
    (void)once;
}

std::string camera_label(ACameraMetadata* meta, const std::string& id) {
    ACameraMetadata_const_entry e = {};
    if (meta && ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_FACING, &e) == ACAMERA_OK && e.count) {
        switch (e.data.u8[0]) {
            case ACAMERA_LENS_FACING_FRONT: return "front camera (" + id + ")";
            case ACAMERA_LENS_FACING_BACK: return "back camera (" + id + ")";
            case ACAMERA_LENS_FACING_EXTERNAL: return "external camera (" + id + ")";
        }
    }
    return "camera " + id;
}

// The largest frame rate the camera can hold for this size, from the minimum frame durations.
double max_fps_for(ACameraMetadata* meta, int w, int h) {
    ACameraMetadata_const_entry e = {};
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, &e) != ACAMERA_OK) return 0;
    for (uint32_t i = 0; i + 3 < e.count; i += 4) {
        if (e.data.i64[i] != AIMAGE_FORMAT_YUV_420_888) continue;
        if ((int)e.data.i64[i + 1] != w || (int)e.data.i64[i + 2] != h) continue;
        const int64_t ns = e.data.i64[i + 3];
        return ns > 0 ? 1e9 / (double)ns : 0;
    }
    return 0;
}

struct Chosen {
    int width = 0, height = 0;
    double fps = 0;
};

// Nearest mode: size first, then fps. YUV_420_888 is the only format the io layer takes.
Chosen pick_mode(ACameraMetadata* meta, const CameraConfig& cfg) {
    Chosen best;
    double best_score = 1e30;
    ACameraMetadata_const_entry e = {};
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) != ACAMERA_OK) return best;
    for (uint32_t i = 0; i + 3 < e.count; i += 4) {
        if (e.data.i32[i] != AIMAGE_FORMAT_YUV_420_888) continue;
        if (e.data.i32[i + 3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) continue;
        const int w = e.data.i32[i + 1], h = e.data.i32[i + 2];
        const double cap = max_fps_for(meta, w, h);
        const double fps = cap > 0 ? std::min(cap, (double)cfg.fps) : cfg.fps;
        const double score = std::fabs(std::log((double)std::max(1, w * h) / std::max(1, cfg.width * cfg.height))) * 100 +
                             std::fabs(fps - cfg.fps);
        if (score < best_score) best_score = score, best = {w, h, fps};
    }
    return best;
}

}  // namespace

// ================================================================ cameras

std::vector<DeviceInfo> list_cameras() {
    start_binder_thread_pool();
    std::vector<DeviceInfo> out;
    ACameraManager* mgr = ACameraManager_create();
    if (!mgr) return out;
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(mgr, &ids) == ACAMERA_OK && ids) {
        for (int i = 0; i < ids->numCameras; ++i) {
            const std::string id = ids->cameraIds[i];
            ACameraMetadata* meta = nullptr;
            ACameraManager_getCameraCharacteristics(mgr, id.c_str(), &meta);
            out.push_back({id, camera_label(meta, id)});
            if (meta) ACameraMetadata_free(meta);
        }
        ACameraManager_deleteCameraIdList(ids);
    }
    ACameraManager_delete(mgr);
    return out;
}

std::vector<CameraMode> list_camera_modes(const std::string& device_id) {
    if (device_id == "test") return {{1280, 720, 30, "NV12"}, {640, 480, 30, "NV12"}};
    std::vector<CameraMode> out;
    ACameraManager* mgr = ACameraManager_create();
    if (!mgr) return out;
    std::string id = device_id;
    if (id.empty()) {
        ACameraIdList* ids = nullptr;
        if (ACameraManager_getCameraIdList(mgr, &ids) == ACAMERA_OK && ids) {
            if (ids->numCameras > 0) id = ids->cameraIds[0];
            ACameraManager_deleteCameraIdList(ids);
        }
    }
    ACameraMetadata* meta = nullptr;
    if (!id.empty() && ACameraManager_getCameraCharacteristics(mgr, id.c_str(), &meta) == ACAMERA_OK && meta) {
        ACameraMetadata_const_entry e = {};
        if (ACameraMetadata_getConstEntry(meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
            for (uint32_t i = 0; i + 3 < e.count; i += 4) {
                if (e.data.i32[i + 3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) continue;
                const int w = e.data.i32[i + 1], h = e.data.i32[i + 2];
                const char* name = e.data.i32[i] == AIMAGE_FORMAT_YUV_420_888 ? "NV12"
                                   : e.data.i32[i] == AIMAGE_FORMAT_JPEG      ? "MJPG"
                                   : e.data.i32[i] == AIMAGE_FORMAT_PRIVATE   ? "PRIV"
                                                                              : "?";
                out.push_back({w, h, max_fps_for(meta, w, h), name});
            }
        }
        ACameraMetadata_free(meta);
    }
    ACameraManager_delete(mgr);
    return out;
}

struct Camera::Impl {
    ACameraManager* mgr = nullptr;
    ACameraDevice* dev = nullptr;
    AImageReader* reader = nullptr;
    ANativeWindow* window = nullptr;
    ACaptureSessionOutputContainer* outputs = nullptr;
    ACaptureSessionOutput* output = nullptr;
    ACameraOutputTarget* target = nullptr;
    ACaptureRequest* request = nullptr;
    ACameraCaptureSession* session = nullptr;

    int width = 0, height = 0;
    double fps = 0;
    std::string id, name, err, native = "NV12";
    bool monotonic_stamps = true;  // the sensor clock is the one mk::now_us() uses
    int64_t anchor = 0;            // when it is not: added to the sensor time
    bool anchored = false;

    // The NDK keeps the pointers it is handed for these, so none of them may live on the stack.
    AImageReader_ImageListener listener = {};
    ACameraDevice_StateCallbacks dev_cb = {};
    ACameraCaptureSession_stateCallbacks ses_cb = {};

    // createCaptureSession returns before the session is configured; a repeating request handed
    // over too early is dropped on the floor (the camera then simply never produces a frame).
    std::mutex ses_mu;
    std::condition_variable ses_cv;
    bool ses_ready = false;

    std::mutex cb_mu;
    std::function<void(const VideoFrame&)> cb;
    bool running = false;
    bool test = false;
    TestSource test_src;

    void deliver(AImage* img) {
        std::unique_lock<std::mutex> lk(cb_mu);
        if (!running || !cb) return;
        int32_t w = 0, h = 0;
        AImage_getWidth(img, &w);
        AImage_getHeight(img, &h);
        w &= ~1, h &= ~1;
        uint8_t *y = nullptr, *u = nullptr, *v = nullptr;
        int ylen = 0, ulen = 0, vlen = 0;
        int32_t ys = 0, us = 0, vs = 0, upix = 1, vpix = 1;
        if (AImage_getPlaneData(img, 0, &y, &ylen) != AMEDIA_OK) return;
        AImage_getPlaneData(img, 1, &u, &ulen);
        AImage_getPlaneData(img, 2, &v, &vlen);
        AImage_getPlaneRowStride(img, 0, &ys);
        AImage_getPlaneRowStride(img, 1, &us);
        AImage_getPlaneRowStride(img, 2, &vs);
        AImage_getPlanePixelStride(img, 1, &upix);
        AImage_getPlanePixelStride(img, 2, &vpix);
        if (!y || !u || !v) return;

        VideoFrame f;
        if (upix == 2 && v == u + 1) {  // chroma already interleaved as U,V: NV12
            f.alloc(w, h, PixelFormat::NV12);
            f.copy_plane(0, y, ys, w, h);
            f.copy_plane(1, u, us, w, h / 2);
        } else if (upix == 2 && u == v + 1) {  // interleaved as V,U (NV21): rebuild it as NV12
            f.alloc(w, h, PixelFormat::NV12);
            f.copy_plane(0, y, ys, w, h);
            for (int r = 0; r < h / 2; ++r) {
                const uint8_t* su = u + (size_t)r * us;
                const uint8_t* sv = v + (size_t)r * vs;
                uint8_t* d = f.data[1] + (size_t)r * f.stride[1];
                for (int c = 0; c < w / 2; ++c) d[2 * c] = su[2 * c], d[2 * c + 1] = sv[2 * c];
            }
        } else {  // planar
            f.alloc(w, h, PixelFormat::I420);
            f.copy_plane(0, y, ys, w, h);
            if (upix == 1 && vpix == 1) {
                f.copy_plane(1, u, us, w / 2, h / 2);
                f.copy_plane(2, v, vs, w / 2, h / 2);
            } else {
                for (int r = 0; r < h / 2; ++r) {
                    const uint8_t* su = u + (size_t)r * us;
                    const uint8_t* sv = v + (size_t)r * vs;
                    uint8_t* du = f.data[1] + (size_t)r * f.stride[1];
                    uint8_t* dv = f.data[2] + (size_t)r * f.stride[2];
                    for (int c = 0; c < w / 2; ++c) du[c] = su[c * upix], dv[c] = sv[c * vpix];
                }
            }
        }
        int64_t ts = 0;
        AImage_getTimestamp(img, &ts);
        const int64_t src = ts / 1000;
        if (monotonic_stamps) {
            f.pts_us = src;
        } else {
            if (!anchored) anchor = now_us() - src, anchored = true;  // boot time, or an unknown clock
            f.pts_us = src + anchor;
        }
        cb(f);
    }

    static void on_image(void* user, AImageReader* reader) {
        Impl& s = *(Impl*)user;
        AImage* img = nullptr;
        if (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK || !img) return;
        s.deliver(img);
        AImage_delete(img);
    }

    void tear_down() {
        if (session) ACameraCaptureSession_stopRepeating(session), ACameraCaptureSession_close(session), session = nullptr;
        if (request) ACaptureRequest_free(request), request = nullptr;
        if (target) ACameraOutputTarget_free(target), target = nullptr;
        if (outputs) ACaptureSessionOutputContainer_free(outputs), outputs = nullptr;
        if (output) ACaptureSessionOutput_free(output), output = nullptr;
        if (dev) ACameraDevice_close(dev), dev = nullptr;
        if (reader) AImageReader_setImageListener(reader, nullptr), AImageReader_delete(reader), reader = nullptr;
        window = nullptr;  // owned by the reader
        if (mgr) ACameraManager_delete(mgr), mgr = nullptr;
    }
};

namespace {
void dev_disconnected(void*, ACameraDevice*) {}
void dev_error(void*, ACameraDevice*, int) {}
void ses_closed(void*, ACameraCaptureSession*) {}
void ses_configured(void* user, ACameraCaptureSession*) {
    Camera::Impl& s = *(Camera::Impl*)user;
    std::lock_guard<std::mutex> lk(s.ses_mu);
    s.ses_ready = true;
    s.ses_cv.notify_all();
}
}  // namespace

Camera::Camera() : p_(new Impl) {}
Camera::~Camera() { close(); }

bool Camera::open(const CameraConfig& cfg) {
    close();
    Impl& s = *p_;
    s.err.clear();
    s.test = cfg.device_id == "test";
    if (s.test) {
        s.width = std::max(2, cfg.width & ~1), s.height = std::max(2, cfg.height & ~1);
        s.fps = cfg.fps > 0 ? cfg.fps : 30;
        s.name = "test pattern";
        s.native = "NV12";
        return true;
    }
    start_binder_thread_pool();
    s.mgr = ACameraManager_create();
    if (!s.mgr) {
        s.err = "ACameraManager_create failed";
        return false;
    }
    s.id = cfg.device_id;
    if (s.id.empty()) {
        ACameraIdList* ids = nullptr;
        if (ACameraManager_getCameraIdList(s.mgr, &ids) == ACAMERA_OK && ids) {
            if (ids->numCameras > 0) s.id = ids->cameraIds[0];
            ACameraManager_deleteCameraIdList(ids);
        }
    }
    if (s.id.empty()) {
        s.err = "no camera found";
        close();
        return false;
    }
    ACameraMetadata* meta = nullptr;
    if (ACameraManager_getCameraCharacteristics(s.mgr, s.id.c_str(), &meta) != ACAMERA_OK || !meta) {
        s.err = "cannot read the characteristics of camera " + s.id;
        close();
        return false;
    }
    s.name = camera_label(meta, s.id);
    ACameraMetadata_const_entry src = {};
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE, &src) == ACAMERA_OK && src.count)
        s.monotonic_stamps = src.data.u8[0] == ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN;
    const Chosen mode = pick_mode(meta, cfg);
    ACameraMetadata_free(meta);
    if (!mode.width) {
        s.err = "camera " + s.id + " has no YUV_420_888 output";
        close();
        return false;
    }
    s.width = mode.width & ~1, s.height = mode.height & ~1;
    s.fps = mode.fps > 0 ? mode.fps : cfg.fps;

    if (AImageReader_new(s.width, s.height, AIMAGE_FORMAT_YUV_420_888, 4, &s.reader) != AMEDIA_OK) {
        s.err = "AImageReader_new failed";
        close();
        return false;
    }
    s.listener = {&s, &Impl::on_image};
    AImageReader_setImageListener(s.reader, &s.listener);
    if (AImageReader_getWindow(s.reader, &s.window) != AMEDIA_OK || !s.window) {
        s.err = "AImageReader_getWindow failed";
        close();
        return false;
    }
    s.dev_cb = {&s, dev_disconnected, dev_error};
    camera_status_t r = ACameraManager_openCamera(s.mgr, s.id.c_str(), &s.dev_cb, &s.dev);
    if (r != ACAMERA_OK) {
        s.err = "openCamera failed (" + std::to_string((int)r) + "); is the CAMERA permission granted?";
        close();
        return false;
    }
    ACaptureSessionOutputContainer_create(&s.outputs);
    ACaptureSessionOutput_create(s.window, &s.output);
    ACaptureSessionOutputContainer_add(s.outputs, s.output);
    s.ses_cb = {&s, ses_closed, ses_configured, ses_configured};
    if (ACameraDevice_createCaptureSession(s.dev, s.outputs, &s.ses_cb, &s.session) != ACAMERA_OK) {
        s.err = "createCaptureSession failed";
        close();
        return false;
    }
    if (ACameraDevice_createCaptureRequest(s.dev, TEMPLATE_RECORD, &s.request) != ACAMERA_OK) {
        s.err = "createCaptureRequest failed";
        close();
        return false;
    }
    ACameraOutputTarget_create(s.window, &s.target);
    ACaptureRequest_addTarget(s.request, s.target);
    // Pin the rate, so auto exposure cannot slow the camera down in a dark room.
    const int32_t range[2] = {(int32_t)std::lround(s.fps), (int32_t)std::lround(s.fps)};
    ACaptureRequest_setEntry_i32(s.request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, range);
    return true;
}

bool Camera::start(std::function<void(const VideoFrame&)> on_frame) {
    Impl& s = *p_;
    stop();
    if (s.test) {
        s.test_src.start_video(s.width, s.height, s.fps, std::move(on_frame));
        return true;
    }
    if (!s.session || !s.request) {
        if (s.err.empty()) s.err = "camera not open";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(s.cb_mu);
        s.cb = std::move(on_frame);
        s.running = true;
    }
    {
        std::unique_lock<std::mutex> lk(s.ses_mu);
        s.ses_cv.wait_for(lk, std::chrono::seconds(3), [&s] { return s.ses_ready; });
    }
    if (ACameraCaptureSession_setRepeatingRequest(s.session, nullptr, 1, &s.request, nullptr) != ACAMERA_OK) {
        s.err = "setRepeatingRequest failed";
        std::lock_guard<std::mutex> lk(s.cb_mu);
        s.running = false;
        s.cb = nullptr;
        return false;
    }
    return true;
}

void Camera::stop() {
    Impl& s = *p_;
    s.test_src.stop();
    if (s.session) ACameraCaptureSession_stopRepeating(s.session);
    std::lock_guard<std::mutex> lk(s.cb_mu);  // a delivery in flight holds this until it returns
    s.running = false;
    s.cb = nullptr;
}

void Camera::close() {
    stop();
    p_->tear_down();
}

int Camera::width() const { return p_->width; }
int Camera::height() const { return p_->height; }
double Camera::fps() const { return p_->fps; }
std::string Camera::format() const { return p_->native; }
std::string Camera::name() const { return p_->name; }
const std::string& Camera::error() const { return p_->err; }

// ================================================================ audio
//
// AAudio has no device list in the NDK (that lives in Java's AudioManager), so only the default
// device is offered here; a numeric device id from the Java side can still be passed through.

namespace {

int32_t device_id_of(const std::string& s) {
    if (s.empty() || s == "default") return AAUDIO_UNSPECIFIED;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    return end && *end == 0 ? (int32_t)v : AAUDIO_UNSPECIFIED;
}

AAudioStreamBuilder* make_builder(const std::string& device, int rate, int ch, aaudio_direction_t dir) {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return nullptr;
    AAudioStreamBuilder_setDirection(b, dir);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, ch);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setDeviceId(b, device_id_of(device));
    return b;
}

}  // namespace

std::vector<DeviceInfo> list_microphones() { return {{"", "default"}}; }
std::vector<DeviceInfo> list_speakers() { return {{"", "default"}}; }

struct Microphone::Impl {
    MicrophoneConfig cfg;
    AAudioStream* stream = nullptr;
    std::function<void(const AudioFrame&)> cb;
    std::string name = "default", err;
    std::atomic<bool> running{false};
    bool test = false;
    TestSource test_src;

    static aaudio_data_callback_result_t on_data(AAudioStream*, void* user, void* data, int32_t frames) {
        Impl& s = *(Impl*)user;
        if (s.running && s.cb && frames > 0) {
            AudioFrame a;
            a.sample_rate = s.cfg.sample_rate, a.channels = s.cfg.channels;
            const int16_t* p = (const int16_t*)data;
            a.samples.assign(p, p + (size_t)frames * s.cfg.channels);
            // these frames were captured over the window that just ended
            a.pts_us = now_us() - (int64_t)frames * 1000000 / s.cfg.sample_rate;
            s.cb(a);
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
};

Microphone::Microphone() : p_(new Impl) {}
Microphone::~Microphone() { close(); }

bool Microphone::open(const MicrophoneConfig& cfg) {
    close();
    Impl& s = *p_;
    s.cfg = cfg;
    s.err.clear();
    s.test = cfg.device_id == "test";
    if (s.test) {
        s.name = "test tone";
        return true;
    }
    AAudioStreamBuilder* b = make_builder(cfg.device_id, cfg.sample_rate, cfg.channels, AAUDIO_DIRECTION_INPUT);
    if (!b) {
        s.err = "AAudio_createStreamBuilder failed";
        return false;
    }
    AAudioStreamBuilder_setDataCallback(b, &Impl::on_data, &s);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s.stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        s.err = std::string("AAudio input open: ") + AAudio_convertResultToText(r) + " (is RECORD_AUDIO granted?)";
        return false;
    }
    // AAudio may hand back a different rate or channel count than asked for.
    s.cfg.sample_rate = AAudioStream_getSampleRate(s.stream);
    s.cfg.channels = AAudioStream_getChannelCount(s.stream);
    return true;
}

bool Microphone::start(std::function<void(const AudioFrame&)> on_audio) {
    Impl& s = *p_;
    stop();
    if (s.test) {
        s.test_src.start_audio(s.cfg.sample_rate, s.cfg.channels, std::move(on_audio));
        return true;
    }
    if (!s.stream) {
        if (s.err.empty()) s.err = "microphone not open";
        return false;
    }
    s.cb = std::move(on_audio);
    s.running = true;
    aaudio_result_t r = AAudioStream_requestStart(s.stream);
    if (r != AAUDIO_OK) {
        s.running = false;
        s.err = std::string("AAudioStream_requestStart: ") + AAudio_convertResultToText(r);
        return false;
    }
    return true;
}

void Microphone::stop() {
    Impl& s = *p_;
    s.test_src.stop();
    if (!s.running) return;
    s.running = false;
    AAudioStream_requestStop(s.stream);
    aaudio_stream_state_t st = AAUDIO_STREAM_STATE_STOPPING;
    AAudioStream_waitForStateChange(s.stream, AAUDIO_STREAM_STATE_STOPPING, &st, 500000000);
    s.cb = nullptr;
}

void Microphone::close() {
    Impl& s = *p_;
    stop();
    if (s.stream) AAudioStream_close(s.stream), s.stream = nullptr;
}

int Microphone::sample_rate() const { return p_->cfg.sample_rate; }
int Microphone::channels() const { return p_->cfg.channels; }
std::string Microphone::name() const { return p_->name; }
const std::string& Microphone::error() const { return p_->err; }

// ================================================================ speaker

struct Speaker::Impl {
    SpeakerConfig cfg;
    AAudioStream* stream = nullptr;
    std::string err;
    std::atomic<bool> running{false};

    mutable std::mutex mu;
    std::deque<int16_t> q;
    int64_t q_pts = -1;         // media time of q.front()
    int64_t written_end = -1;   // media time just after the last real sample handed to AAudio
    int64_t silence_after = 0;  // silence frames written after written_end (underrun padding)
    mutable int64_t delay_frames = 0;
    int rs_rate = 0;            // linear resampler state for write() at another rate
    double rs_pos = 0;
    std::vector<int16_t> rs_last;

    static aaudio_data_callback_result_t on_data(AAudioStream*, void* user, void* data, int32_t frames) {
        Impl& s = *(Impl*)user;
        const int ch = s.cfg.channels;
        int16_t* dst = (int16_t*)data;
        int real = 0;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            real = (int)std::min<size_t>(frames, s.q.size() / ch);
            std::copy(s.q.begin(), s.q.begin() + (size_t)real * ch, dst);
            s.q.erase(s.q.begin(), s.q.begin() + (size_t)real * ch);
            if (real) {
                s.q_pts += (int64_t)real * 1000000 / s.cfg.sample_rate;
                s.written_end = s.q_pts;
                s.silence_after = 0;
            }
            s.silence_after += frames - real;
        }
        std::fill(dst + (size_t)real * ch, dst + (size_t)frames * ch, 0);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void refresh_delay() const {
        int64_t d = 0;
        if (stream && running) d = AAudioStream_getFramesWritten(stream) - AAudioStream_getFramesRead(stream);
        std::lock_guard<std::mutex> lk(mu);
        delay_frames = std::max<int64_t>(0, d);
    }

    int64_t played_locked() const {
        if (written_end < 0) return -1;
        int64_t back = delay_frames - silence_after;  // only silence queued: everything real was heard
        if (back < 0) back = 0;
        return written_end - back * 1000000 / cfg.sample_rate;
    }
};

Speaker::Speaker() : p_(new Impl) {}
Speaker::~Speaker() { close(); }

bool Speaker::open(const SpeakerConfig& cfg) {
    close();
    Impl& s = *p_;
    s.cfg = cfg;
    s.err.clear();
    AAudioStreamBuilder* b = make_builder(cfg.device_id, cfg.sample_rate, cfg.channels, AAUDIO_DIRECTION_OUTPUT);
    if (!b) {
        s.err = "AAudio_createStreamBuilder failed";
        return false;
    }
    AAudioStreamBuilder_setDataCallback(b, &Impl::on_data, &s);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s.stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        s.err = std::string("AAudio output open: ") + AAudio_convertResultToText(r);
        return false;
    }
    s.cfg.sample_rate = AAudioStream_getSampleRate(s.stream);
    s.cfg.channels = AAudioStream_getChannelCount(s.stream);
    return true;
}

bool Speaker::start() {
    Impl& s = *p_;
    if (!s.stream) {
        if (s.err.empty()) s.err = "speaker not open";
        return false;
    }
    if (s.running) return true;
    s.running = true;
    aaudio_result_t r = AAudioStream_requestStart(s.stream);
    if (r != AAUDIO_OK) {
        s.running = false;
        s.err = std::string("AAudioStream_requestStart: ") + AAudio_convertResultToText(r);
        return false;
    }
    return true;
}

void Speaker::write(const AudioFrame& f) {
    Impl& s = *p_;
    if (f.samples.empty() || f.channels <= 0) return;
    const int ch = s.cfg.channels, n = f.sample_count();
    std::vector<int16_t> mixed((size_t)n * ch);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < ch; ++c) mixed[(size_t)i * ch + c] = f.samples[(size_t)i * f.channels + std::min(c, f.channels - 1)];
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.q.empty()) s.q_pts = f.pts_us;
    if (f.sample_rate == s.cfg.sample_rate) {
        s.q.insert(s.q.end(), mixed.begin(), mixed.end());
        return;
    }
    if (s.rs_rate != f.sample_rate || s.rs_last.empty())
        s.rs_rate = f.sample_rate, s.rs_pos = 1, s.rs_last.assign(mixed.begin(), mixed.begin() + ch);
    const double step = (double)f.sample_rate / s.cfg.sample_rate;
    while (s.rs_pos < n) {
        int i0 = (int)s.rs_pos;
        double t = s.rs_pos - i0;
        for (int c = 0; c < ch; ++c) {
            double a = i0 == 0 ? s.rs_last[c] : mixed[(size_t)(i0 - 1) * ch + c];
            double b = mixed[(size_t)i0 * ch + c];
            s.q.push_back((int16_t)std::lround(a + (b - a) * t));
        }
        s.rs_pos += step;
    }
    s.rs_pos -= n;
    s.rs_last.assign(mixed.end() - ch, mixed.end());
}

int64_t Speaker::played_pts_us() const {
    p_->refresh_delay();
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->played_locked();
}

int64_t Speaker::queued_us() const {
    Impl& s = *p_;
    s.refresh_delay();
    std::lock_guard<std::mutex> lk(s.mu);
    const int64_t q_us = (int64_t)(s.q.size() / std::max(1, s.cfg.channels)) * 1000000 / s.cfg.sample_rate;
    const int64_t end = !s.q.empty() ? s.q_pts + q_us : s.written_end;
    const int64_t played = s.played_locked();
    if (end < 0) return 0;
    if (played < 0) return q_us;
    return std::max<int64_t>(0, end - played);
}

void Speaker::clear() {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    s.q.clear();
    s.q_pts = s.written_end = -1;
    s.silence_after = 0;
    s.rs_last.clear();
}

void Speaker::stop() {
    Impl& s = *p_;
    if (!s.running) return;
    s.running = false;
    AAudioStream_requestStop(s.stream);
    aaudio_stream_state_t st = AAUDIO_STREAM_STATE_STOPPING;
    AAudioStream_waitForStateChange(s.stream, AAUDIO_STREAM_STATE_STOPPING, &st, 500000000);
    std::lock_guard<std::mutex> lk(s.mu);
    s.delay_frames = 0;
}

void Speaker::close() {
    Impl& s = *p_;
    stop();
    if (s.stream) AAudioStream_close(s.stream), s.stream = nullptr;
    clear();
}

const std::string& Speaker::error() const { return p_->err; }

}  // namespace mk
