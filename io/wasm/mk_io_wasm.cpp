// mediakit io for the browser (Emscripten): camera through getUserMedia.
// There are no threads: a JS timer grabs the <video> into a canvas at the configured fps and calls back
// into C++ on the main thread, whenever the program is waiting (mk::Window::run, emscripten_sleep).
// open() awaits the browser's permission prompt, so programs are linked with -sASYNCIFY (CMake adds it).
// Frames are BGRA. Microphone and speaker are not implemented yet.
#include <emscripten.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#include "mk_io.h"

EM_JS_DEPS(mk_io_wasm, "$UTF8ToString,$stringToNewUTF8,malloc,free");

// All browser state lives in Module.mkCams[id]; C++ keeps only the id.
EM_ASYNC_JS(int, mk_js_cam_open, (int id, const char* device, int width, int height), {
    Module.mkCams = Module.mkCams || {};
    const cam = { running: false, timer: 0, error: "" };
    Module.mkCams[id] = cam;
    try {
        if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia)
            throw new Error('getUserMedia is not available (the page must be https:// or http://localhost)');
        const video = { width: { ideal: width }, height: { ideal: height } };
        const dev = UTF8ToString(device);
        if (dev) video.deviceId = { exact: dev };
        cam.stream = await navigator.mediaDevices.getUserMedia({ video: video, audio: false });
        cam.video = document.createElement('video');
        cam.video.muted = true;
        cam.video.playsInline = true;
        cam.video.srcObject = cam.stream;
        await cam.video.play();
        for (let i = 0; i < 300 && !cam.video.videoWidth; ++i) await new Promise(r => setTimeout(r, 10));
        if (!cam.video.videoWidth) throw new Error('the camera sent no picture');
        cam.canvas = document.createElement('canvas');
        cam.ctx = cam.canvas.getContext('2d', { willReadFrequently: true });
        return 1;
    } catch (e) {
        cam.error = (e && e.name ? e.name + ': ' : "") + (e && e.message ? e.message : String(e));
        if (cam.stream) cam.stream.getTracks().forEach(t => t.stop());
        cam.stream = null;
        return 0;
    }
});

EM_JS(char*, mk_js_cam_info, (int id), {
    // "width\theight\tlabel" or "\t\terror"
    const cam = Module.mkCams && Module.mkCams[id];
    if (!cam) return stringToNewUTF8('\t\tno camera');
    if (!cam.stream) return stringToNewUTF8('\t\t' + cam.error);
    const label = cam.stream.getVideoTracks()[0].label || 'camera';
    return stringToNewUTF8(cam.video.videoWidth + '\t' + cam.video.videoHeight + '\t' + label);
});

// fps ticks on the wall clock; a late tick (slow callback) skips ahead instead of bursting.
// test: no camera, C++ draws the frame.
EM_JS(void, mk_js_cam_start, (int id, double fps, int test), {
    Module.mkCams = Module.mkCams || {};
    const cam = Module.mkCams[id] || (Module.mkCams[id] = { error: "" });
    const period = 1000 / fps;
    const t0 = performance.now();
    let n = 0;
    const tick = () => {
        if (!cam.running) return;
        let ptr = 0, w = 0, h = 0;
        if (!test) {
            const v = cam.video;
            w = v.videoWidth, h = v.videoHeight;
            if (w && h) {
                if (cam.canvas.width != w || cam.canvas.height != h) cam.canvas.width = w, cam.canvas.height = h;
                cam.ctx.drawImage(v, 0, 0, w, h);
                const img = cam.ctx.getImageData(0, 0, w, h);
                ptr = _malloc(img.data.length);
                HEAPU8.set(img.data, ptr);
            }
        }
        if (test || ptr) _mk_wasm_camera_frame(id, ptr, w, h);
        if (ptr) _free(ptr);
        const now = performance.now();
        n = Math.max(n + 1, Math.floor((now - t0) / period));
        if (cam.running) cam.timer = setTimeout(tick, Math.max(0, t0 + n * period - now));
    };
    cam.running = true;
    cam.timer = setTimeout(tick, 0);
});

EM_JS(void, mk_js_cam_stop, (int id), {
    const cam = Module.mkCams && Module.mkCams[id];
    if (!cam) return;
    cam.running = false;
    clearTimeout(cam.timer);
});

EM_JS(void, mk_js_cam_close, (int id), {
    const cam = Module.mkCams && Module.mkCams[id];
    if (!cam) return;
    cam.running = false;
    clearTimeout(cam.timer);
    if (cam.stream) cam.stream.getTracks().forEach(t => t.stop());
    delete Module.mkCams[id];
});

// "deviceId\tlabel\n" per camera. Labels stay empty until the page has been given camera permission.
EM_ASYNC_JS(char*, mk_js_list_cameras, (), {
    let out = "";
    try {
        const devs = await navigator.mediaDevices.enumerateDevices();
        for (const d of devs)
            if (d.kind == 'videoinput') out += d.deviceId + '\t' + (d.label || 'camera') + '\n';
    } catch (e) {
    }
    return stringToNewUTF8(out);
});

namespace mk {

namespace {

std::map<int, Camera::Impl*>& cameras() {
    static std::map<int, Camera::Impl*> m;
    return m;
}

std::string take(char* s) {
    std::string r = s ? s : "";
    std::free(s);
    return r;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream in(s);
    while (std::getline(in, item, sep)) out.push_back(item);
    return out;
}

// Same idea as TestSource (moving gradient with a white box), drawn straight into BGRA.
void draw_test_frame(VideoFrame& f, int64_t i) {
    for (int y = 0; y < f.height; ++y) {
        uint8_t* row = f.data[0] + (size_t)y * f.stride[0];
        for (int x = 0; x < f.width; ++x) {
            const uint8_t v = (uint8_t)(16 + ((x + y + i * 4) % 200));
            row[4 * x] = v, row[4 * x + 1] = (uint8_t)(v / 2 + 40), row[4 * x + 2] = (uint8_t)(255 - v), row[4 * x + 3] = 255;
        }
    }
    const int bs = f.height / 6, bx = (int)((i * 7) % std::max(1, f.width - bs)), by = f.height / 3;
    for (int y = by; y < by + bs && y < f.height; ++y) std::memset(f.data[0] + (size_t)y * f.stride[0] + (size_t)bx * 4, 255, (size_t)bs * 4);
}

}  // namespace

struct Camera::Impl {
    int id = 0;
    bool test = false, opened = false, running = false;
    int width = 0, height = 0;
    double fps = 0;
    std::string name, err;
    std::function<void(const VideoFrame&)> cb;
    int64_t frames = 0;
};

}  // namespace mk

extern "C" EMSCRIPTEN_KEEPALIVE void mk_wasm_camera_frame(int id, const uint8_t* rgba, int w, int h) {
    auto it = mk::cameras().find(id);
    if (it == mk::cameras().end() || !it->second->running || !it->second->cb) return;
    mk::Camera::Impl& s = *it->second;
    mk::VideoFrame f;
    if (rgba) {
        f.alloc(w, h, mk::PixelFormat::BGRA);
        const size_t n = (size_t)w * h;
        uint8_t* d = f.data[0];
        for (size_t k = 0; k < n; ++k, rgba += 4, d += 4) d[0] = rgba[2], d[1] = rgba[1], d[2] = rgba[0], d[3] = 255;
        s.width = w, s.height = h;
    } else {
        f.alloc(s.width, s.height, mk::PixelFormat::BGRA);
        mk::draw_test_frame(f, s.frames);
    }
    f.pts_us = mk::now_us();
    ++s.frames;
    s.cb(f);
}

namespace mk {

std::vector<DeviceInfo> list_cameras() {
    std::vector<DeviceInfo> out;
    for (const std::string& line : split(take(mk_js_list_cameras()), '\n')) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        DeviceInfo d;
        d.id = line.substr(0, tab);
        d.name = line.substr(tab + 1);
        out.push_back(d);
    }
    return out;
}

std::vector<DeviceInfo> list_microphones() { return {}; }
std::vector<DeviceInfo> list_speakers() { return {}; }

// Browsers do not list modes; ask for a size in CameraConfig and the camera picks the closest it has.
std::vector<CameraMode> list_camera_modes(const std::string& device_id) {
    if (device_id == "test") return {{1280, 720, 30, "BGRA"}, {640, 480, 30, "BGRA"}};
    return {};
}

Camera::Camera() : p_(new Impl) {
    static int next_id = 1;
    p_->id = next_id++;
}

Camera::~Camera() { close(); }

bool Camera::open(const CameraConfig& cfg) {
    close();
    Impl& s = *p_;
    s.err.clear();
    s.fps = cfg.fps > 0 ? cfg.fps : 30;
    s.test = cfg.device_id == "test";
    if (s.test) {
        s.width = cfg.width, s.height = cfg.height;
        s.name = "test pattern";
        s.opened = true;
        return true;
    }
    const int ok = mk_js_cam_open(s.id, cfg.device_id.c_str(), cfg.width, cfg.height);
    const std::vector<std::string> info = split(take(mk_js_cam_info(s.id)), '\t');
    if (!ok || info.size() < 3) {
        s.err = info.size() >= 3 ? info[2] : "camera open failed";
        return false;
    }
    s.width = std::atoi(info[0].c_str()), s.height = std::atoi(info[1].c_str());
    s.name = info[2];
    s.opened = true;
    return true;
}

bool Camera::start(std::function<void(const VideoFrame&)> on_frame) {
    Impl& s = *p_;
    if (!s.opened) {
        s.err = "not open";
        return false;
    }
    stop();
    s.cb = std::move(on_frame);
    s.running = true;
    cameras()[s.id] = &s;
    mk_js_cam_start(s.id, s.fps, s.test ? 1 : 0);
    return true;
}

void Camera::stop() {
    Impl& s = *p_;
    if (!s.running) return;
    s.running = false;
    mk_js_cam_stop(s.id);
    cameras().erase(s.id);
}

void Camera::close() {
    stop();
    if (p_->opened && !p_->test) mk_js_cam_close(p_->id);
    p_->opened = false;
}

int Camera::width() const { return p_->width; }
int Camera::height() const { return p_->height; }
double Camera::fps() const { return p_->fps; }
std::string Camera::format() const { return p_->test ? "BGRA" : "RGBA"; }
std::string Camera::name() const { return p_->name; }
const std::string& Camera::error() const { return p_->err; }

// ---------------------------------------------------------------- not implemented in the browser yet

struct Microphone::Impl {
    std::string err = "microphone: not implemented in the wasm build yet";
};
Microphone::Microphone() : p_(new Impl) {}
Microphone::~Microphone() = default;
bool Microphone::open(const MicrophoneConfig&) { return false; }
bool Microphone::start(std::function<void(const AudioFrame&)>) { return false; }
void Microphone::stop() {}
void Microphone::close() {}
int Microphone::sample_rate() const { return 0; }
int Microphone::channels() const { return 0; }
std::string Microphone::name() const { return {}; }
const std::string& Microphone::error() const { return p_->err; }

struct Speaker::Impl {
    std::string err = "speaker: not implemented in the wasm build yet";
};
Speaker::Speaker() : p_(new Impl) {}
Speaker::~Speaker() = default;
bool Speaker::open(const SpeakerConfig&) { return false; }
bool Speaker::start() { return false; }
void Speaker::write(const AudioFrame&) {}
int64_t Speaker::played_pts_us() const { return -1; }
int64_t Speaker::queued_us() const { return 0; }
void Speaker::clear() {}
void Speaker::stop() {}
void Speaker::close() {}
const std::string& Speaker::error() const { return p_->err; }

}  // namespace mk
