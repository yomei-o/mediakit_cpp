// mediakit io for Linux: V4L2 camera, ALSA microphone / speaker.
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "mk_io.h"
#include "mk_test_source.h"

namespace mk {

namespace {

int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r == -1 && errno == EINTR);
    return r;
}

std::string fourcc_name(uint32_t f) {
    switch (f) {
        case V4L2_PIX_FMT_NV12: return "NV12";
        case V4L2_PIX_FMT_YUYV: return "YUY2";
        case V4L2_PIX_FMT_MJPEG: return "MJPG";
        case V4L2_PIX_FMT_YUV420: return "I420";
        case V4L2_PIX_FMT_H264: return "H264";
    }
    char b[5] = {(char)(f & 0xff), (char)((f >> 8) & 0xff), (char)((f >> 16) & 0xff), (char)((f >> 24) & 0xff), 0};
    return b;
}

bool is_capture_device(const std::string& path, std::string* card) {
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;
    v4l2_capability cap = {};
    bool ok = xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
    uint32_t caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
    ok = ok && (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING);
    if (ok && card) *card = (const char*)cap.card;
    ::close(fd);
    return ok;
}

std::string first_camera() {
    for (int i = 0; i < 64; ++i) {
        std::string p = "/dev/video" + std::to_string(i);
        if (is_capture_device(p, nullptr)) return p;
    }
    return {};
}

}  // namespace

// ================================================================ cameras

std::vector<DeviceInfo> list_cameras() {
    std::vector<DeviceInfo> out;
    for (int i = 0; i < 64; ++i) {
        std::string p = "/dev/video" + std::to_string(i), card;
        if (is_capture_device(p, &card)) out.push_back({p, card});
    }
    return out;
}

std::vector<CameraMode> list_camera_modes(const std::string& device_id) {
    if (device_id == "test") return {{1280, 720, 30, "NV12"}, {640, 480, 30, "NV12"}};
    std::vector<CameraMode> out;
    std::string path = device_id.empty() ? first_camera() : device_id;
    int fd = path.empty() ? -1 : ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return out;
    v4l2_fmtdesc fd_ = {};
    fd_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (fd_.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &fd_) == 0; ++fd_.index) {
        v4l2_frmsizeenum fs = {};
        fs.pixel_format = fd_.pixelformat;
        for (fs.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; ++fs.index) {
            if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
            v4l2_frmivalenum fi = {};
            fi.pixel_format = fd_.pixelformat;
            fi.width = fs.discrete.width, fi.height = fs.discrete.height;
            bool any = false;
            for (fi.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0; ++fi.index) {
                if (fi.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
                double f = fi.discrete.numerator ? (double)fi.discrete.denominator / fi.discrete.numerator : 0;
                out.push_back({(int)fs.discrete.width, (int)fs.discrete.height, f, fourcc_name(fd_.pixelformat)});
                any = true;
            }
            if (!any) out.push_back({(int)fs.discrete.width, (int)fs.discrete.height, 0, fourcc_name(fd_.pixelformat)});
        }
    }
    ::close(fd);
    return out;
}

struct Camera::Impl {
    int fd = -1;
    struct Buf {
        void* p = nullptr;
        size_t n = 0;
    };
    std::vector<Buf> bufs;
    uint32_t pixfmt = 0;
    int width = 0, height = 0, bytesperline = 0;
    double fps = 0;
    std::string name, err, native;
    std::thread th;
    std::atomic<bool> running{false};
    std::function<void(const VideoFrame&)> cb;
    bool test = false;
    TestSource test_src;

    void unmap() {
        for (auto& b : bufs)
            if (b.p && b.p != MAP_FAILED) munmap(b.p, b.n);
        bufs.clear();
    }

    void deliver(const uint8_t* p, size_t used, int64_t pts) {
        VideoFrame f;
        if (pixfmt == V4L2_PIX_FMT_MJPEG) {
            f.set_compressed(width, height, PixelFormat::MJPEG, p, used);  // decoded by FrameConverter
        } else if (pixfmt == V4L2_PIX_FMT_NV12) {
            f.alloc(width, height, PixelFormat::NV12);
            f.copy_plane(0, p, bytesperline, width, height);
            f.copy_plane(1, p + (size_t)bytesperline * height, bytesperline, width, height / 2);
        } else if (pixfmt == V4L2_PIX_FMT_YUV420) {
            f.alloc(width, height, PixelFormat::I420);
            const int cs = bytesperline / 2;
            f.copy_plane(0, p, bytesperline, width, height);
            f.copy_plane(1, p + (size_t)bytesperline * height, cs, width / 2, height / 2);
            f.copy_plane(2, p + (size_t)bytesperline * height + (size_t)cs * (height / 2), cs, width / 2, height / 2);
        } else {  // YUYV -> NV12
            f.alloc(width, height, PixelFormat::NV12);
            for (int y = 0; y < height; ++y) {
                const uint8_t* s = p + (size_t)y * bytesperline;
                uint8_t* dy = f.data[0] + (size_t)y * f.stride[0];
                for (int x = 0; x < width; ++x) dy[x] = s[x * 2];
                if (!(y & 1)) {
                    uint8_t* duv = f.data[1] + (size_t)(y / 2) * f.stride[1];
                    for (int x = 0; x + 1 < width; x += 2) duv[x] = s[x * 2 + 1], duv[x + 1] = s[x * 2 + 3];
                }
            }
        }
        f.pts_us = pts;
        if (cb) cb(f);
    }

    void loop() {
        while (running) {
            pollfd pf = {fd, POLLIN, 0};
            int r = poll(&pf, 1, 200);
            if (r <= 0) continue;
            v4l2_buffer b = {};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
                if (errno == EAGAIN) continue;
                err = std::string("VIDIOC_DQBUF: ") + strerror(errno);
                break;
            }
            int64_t pts = now_us();
            // monotonic driver timestamps are on the same clock as now_us() (steady_clock)
            if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC && (b.timestamp.tv_sec || b.timestamp.tv_usec))
                pts = (int64_t)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec;
            if (b.index < bufs.size() && !(b.flags & V4L2_BUF_FLAG_ERROR))
                deliver((const uint8_t*)bufs[b.index].p, b.bytesused, pts);
            xioctl(fd, VIDIOC_QBUF, &b);
        }
        running = false;
    }
};

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
    std::string path = cfg.device_id.empty() ? first_camera() : cfg.device_id;
    if (path.empty()) {
        s.err = "no camera found";
        return false;
    }
    if (!is_capture_device(path, &s.name)) {
        s.err = "not a capture device: " + path;
        return false;
    }
    s.fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (s.fd < 0) {
        s.err = "cannot open " + path + ": " + strerror(errno);
        return false;
    }
    // nearest mode: size, then fps, then cheap formats (NV12 < YUYV < MJPEG)
    std::vector<CameraMode> modes = list_camera_modes(path);
    const CameraMode* best = nullptr;
    double best_score = 1e30;
    for (auto& m : modes) {
        if (m.format != "NV12" && m.format != "YUY2" && m.format != "MJPG" && m.format != "I420") continue;
        double sc = std::fabs(std::log((double)std::max(1, m.width * m.height) / std::max(1, cfg.width * cfg.height))) * 100 +
                    std::fabs(m.fps - cfg.fps) + (m.format == "NV12" || m.format == "I420" ? 0 : m.format == "YUY2" ? 0.5 : 1);
        if (!cfg.format.empty() && m.format != cfg.format) sc += 1000;
        if (sc < best_score) best_score = sc, best = &m;
    }
    v4l2_format f = {};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = best ? best->width : cfg.width;
    f.fmt.pix.height = best ? best->height : cfg.height;
    f.fmt.pix.pixelformat = !best ? V4L2_PIX_FMT_YUYV
                            : best->format == "NV12" ? V4L2_PIX_FMT_NV12
                            : best->format == "I420" ? V4L2_PIX_FMT_YUV420
                            : best->format == "MJPG" ? V4L2_PIX_FMT_MJPEG
                                                     : V4L2_PIX_FMT_YUYV;
    f.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(s.fd, VIDIOC_S_FMT, &f) < 0) {
        s.err = std::string("VIDIOC_S_FMT: ") + strerror(errno);
        close();
        return false;
    }
    s.pixfmt = f.fmt.pix.pixelformat;
    s.width = (int)f.fmt.pix.width & ~1, s.height = (int)f.fmt.pix.height & ~1;
    s.bytesperline = f.fmt.pix.bytesperline ? (int)f.fmt.pix.bytesperline : (s.pixfmt == V4L2_PIX_FMT_YUYV ? s.width * 2 : s.width);
    s.native = fourcc_name(s.pixfmt);
    v4l2_streamparm sp = {};
    sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sp.parm.capture.timeperframe.numerator = 1;
    sp.parm.capture.timeperframe.denominator = best && best->fps > 0 ? (uint32_t)std::lround(best->fps) : (uint32_t)cfg.fps;
    xioctl(s.fd, VIDIOC_S_PARM, &sp);
    if (xioctl(s.fd, VIDIOC_G_PARM, &sp) == 0 && sp.parm.capture.timeperframe.numerator)
        s.fps = (double)sp.parm.capture.timeperframe.denominator / sp.parm.capture.timeperframe.numerator;
    else
        s.fps = cfg.fps;
    v4l2_requestbuffers rb = {};
    rb.count = 4;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(s.fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
        s.err = std::string("VIDIOC_REQBUFS: ") + strerror(errno);
        close();
        return false;
    }
    for (uint32_t i = 0; i < rb.count; ++i) {
        v4l2_buffer b = {};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(s.fd, VIDIOC_QUERYBUF, &b) < 0) {
            s.err = "VIDIOC_QUERYBUF failed";
            close();
            return false;
        }
        Impl::Buf mb;
        mb.n = b.length;
        mb.p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, b.m.offset);
        if (mb.p == MAP_FAILED) {
            s.err = "mmap failed";
            close();
            return false;
        }
        s.bufs.push_back(mb);
    }
    return true;
}

bool Camera::start(std::function<void(const VideoFrame&)> on_frame) {
    Impl& s = *p_;
    stop();
    if (s.test) {
        s.test_src.start_video(s.width, s.height, s.fps, std::move(on_frame));
        return true;
    }
    if (s.fd < 0) {
        if (s.err.empty()) s.err = "camera not open";
        return false;
    }
    for (uint32_t i = 0; i < s.bufs.size(); ++i) {
        v4l2_buffer b = {};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        xioctl(s.fd, VIDIOC_QBUF, &b);
    }
    v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(s.fd, VIDIOC_STREAMON, &t) < 0) {
        s.err = std::string("VIDIOC_STREAMON: ") + strerror(errno);
        return false;
    }
    s.cb = std::move(on_frame);
    s.running = true;
    s.th = std::thread([&s] { s.loop(); });
    return true;
}

void Camera::stop() {
    Impl& s = *p_;
    s.test_src.stop();
    s.running = false;
    if (s.th.joinable()) {
        s.th.join();
        v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(s.fd, VIDIOC_STREAMOFF, &t);
    }
}

void Camera::close() {
    stop();
    p_->unmap();
    if (p_->fd >= 0) ::close(p_->fd), p_->fd = -1;
}

int Camera::width() const { return p_->width; }
int Camera::height() const { return p_->height; }
double Camera::fps() const { return p_->fps; }
std::string Camera::format() const { return p_->native; }
std::string Camera::name() const { return p_->name; }
const std::string& Camera::error() const { return p_->err; }

// ================================================================ ALSA

namespace {

std::vector<DeviceInfo> list_pcm(const char* io) {
    std::vector<DeviceInfo> out;
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return out;
    for (void** h = hints; *h; ++h) {
        char* name = snd_device_name_get_hint(*h, "NAME");
        char* desc = snd_device_name_get_hint(*h, "DESC");
        char* ioid = snd_device_name_get_hint(*h, "IOID");
        if (name && (!ioid || !strcmp(ioid, io)) && strcmp(name, "null")) {
            std::string d = desc ? desc : name;
            std::replace(d.begin(), d.end(), '\n', ' ');
            out.push_back({name, d});
        }
        free(name), free(desc), free(ioid);
    }
    snd_device_name_free_hint(hints);
    return out;
}

// s16 interleaved at the given rate / channels ("plug" layers convert to whatever the hardware does)
bool open_pcm(snd_pcm_t** pcm, const std::string& dev, snd_pcm_stream_t dir, int rate, int ch, std::string& err) {
    const std::string name = dev.empty() ? "default" : dev;
    int r = snd_pcm_open(pcm, name.c_str(), dir, 0);
    if (r < 0) {
        err = "snd_pcm_open(" + name + "): " + snd_strerror(r);
        return false;
    }
    r = snd_pcm_set_params(*pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, ch, rate, 1, 100000);  // 100 ms
    if (r < 0) {
        err = std::string("snd_pcm_set_params: ") + snd_strerror(r);
        snd_pcm_close(*pcm);
        *pcm = nullptr;
        return false;
    }
    return true;
}

}  // namespace

std::vector<DeviceInfo> list_microphones() { return list_pcm("Input"); }
std::vector<DeviceInfo> list_speakers() { return list_pcm("Output"); }

struct Microphone::Impl {
    MicrophoneConfig cfg;
    snd_pcm_t* pcm = nullptr;
    std::thread th;
    std::atomic<bool> running{false};
    std::function<void(const AudioFrame&)> cb;
    std::string name, err;
    bool test = false;
    TestSource test_src;

    void loop() {
        const int chunk = cfg.sample_rate / 100;  // 10 ms
        snd_pcm_prepare(pcm);
        snd_pcm_start(pcm);
        while (running) {
            AudioFrame a;
            a.sample_rate = cfg.sample_rate, a.channels = cfg.channels;
            a.samples.resize((size_t)chunk * cfg.channels);
            snd_pcm_sframes_t n = snd_pcm_readi(pcm, a.samples.data(), chunk);
            if (n == -EPIPE || n == -ESTRPIPE) {  // overrun: recover, the gap is filled by the encoder
                snd_pcm_recover(pcm, (int)n, 1);
                continue;
            }
            if (n < 0) {
                err = std::string("snd_pcm_readi: ") + snd_strerror((int)n);
                break;
            }
            snd_pcm_sframes_t delay = 0;
            snd_pcm_delay(pcm, &delay);
            a.samples.resize((size_t)n * cfg.channels);
            // the first sample was captured (n + delay) samples ago
            a.pts_us = now_us() - (int64_t)(n + std::max<snd_pcm_sframes_t>(0, delay)) * 1000000 / cfg.sample_rate;
            if (cb && n > 0) cb(a);
        }
        snd_pcm_drop(pcm);
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
    if (!open_pcm(&s.pcm, cfg.device_id, SND_PCM_STREAM_CAPTURE, cfg.sample_rate, cfg.channels, s.err)) return false;
    s.name = cfg.device_id.empty() ? "default" : cfg.device_id;
    return true;
}

bool Microphone::start(std::function<void(const AudioFrame&)> on_audio) {
    Impl& s = *p_;
    stop();
    if (s.test) {
        s.test_src.start_audio(s.cfg.sample_rate, s.cfg.channels, std::move(on_audio));
        return true;
    }
    if (!s.pcm) {
        if (s.err.empty()) s.err = "microphone not open";
        return false;
    }
    s.cb = std::move(on_audio);
    s.running = true;
    s.th = std::thread([&s] { s.loop(); });
    return true;
}

void Microphone::stop() {
    p_->test_src.stop();
    p_->running = false;
    if (p_->th.joinable()) p_->th.join();
}

void Microphone::close() {
    stop();
    if (p_->pcm) snd_pcm_close(p_->pcm), p_->pcm = nullptr;
}

int Microphone::sample_rate() const { return p_->cfg.sample_rate; }
int Microphone::channels() const { return p_->cfg.channels; }
std::string Microphone::name() const { return p_->name; }
const std::string& Microphone::error() const { return p_->err; }

// ================================================================ speaker

struct Speaker::Impl {
    SpeakerConfig cfg;
    snd_pcm_t* pcm = nullptr;
    std::thread th;
    std::atomic<bool> running{false};
    std::string err;

    mutable std::mutex mu;
    std::deque<int16_t> q;
    int64_t q_pts = -1;          // media time of q.front()
    int64_t written_end = -1;    // media time just after the last real sample handed to ALSA
    int64_t delay_frames = 0;    // frames in ALSA not yet heard (last check)
    int64_t silence_after = 0;   // silence frames written after written_end (underrun padding)
    // linear resampler state for write() at another rate
    int rs_rate = 0;
    double rs_pos = 0;
    std::vector<int16_t> rs_last;

    void loop() {
        const int ch = cfg.channels, chunk = cfg.sample_rate / 100;
        std::vector<int16_t> buf((size_t)chunk * ch);
        snd_pcm_prepare(pcm);
        while (running) {
            int real = 0;
            {
                std::lock_guard<std::mutex> lk(mu);
                real = (int)std::min<size_t>(chunk, q.size() / ch);
                std::copy(q.begin(), q.begin() + (size_t)real * ch, buf.begin());
                std::fill(buf.begin() + (size_t)real * ch, buf.end(), 0);
                q.erase(q.begin(), q.begin() + (size_t)real * ch);
                if (real) {
                    q_pts += (int64_t)real * 1000000 / cfg.sample_rate;
                    written_end = q_pts;
                    silence_after = 0;
                }
                silence_after += chunk - real;
            }
            snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf.data(), chunk);  // blocks while ALSA's buffer is full
            if (n < 0) snd_pcm_recover(pcm, (int)n, 1);
            snd_pcm_sframes_t d = 0;
            if (snd_pcm_delay(pcm, &d) == 0) {
                std::lock_guard<std::mutex> lk(mu);
                delay_frames = std::max<snd_pcm_sframes_t>(0, d);
            }
        }
        snd_pcm_drop(pcm);
    }

    int64_t played_locked() const {
        if (written_end < 0) return -1;
        // the sample being heard is delay_frames before the end of what was written
        int64_t back = delay_frames - silence_after;
        if (back < 0) back = 0;  // only silence is still queued: everything real was heard
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
    return open_pcm(&s.pcm, cfg.device_id, SND_PCM_STREAM_PLAYBACK, cfg.sample_rate, cfg.channels, s.err);
}

bool Speaker::start() {
    Impl& s = *p_;
    if (!s.pcm) {
        if (s.err.empty()) s.err = "speaker not open";
        return false;
    }
    if (s.running) return true;
    s.running = true;
    s.th = std::thread([&s] { s.loop(); });
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
    // linear resampling
    if (s.rs_rate != f.sample_rate || s.rs_last.empty()) s.rs_rate = f.sample_rate, s.rs_pos = 1, s.rs_last.assign(mixed.begin(), mixed.begin() + ch);
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
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->played_locked();
}

int64_t Speaker::queued_us() const {
    Impl& s = *p_;
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
}

void Speaker::stop() {
    p_->running = false;
    if (p_->th.joinable()) p_->th.join();
}

void Speaker::close() {
    stop();
    if (p_->pcm) snd_pcm_close(p_->pcm), p_->pcm = nullptr;
    clear();
}

const std::string& Speaker::error() const { return p_->err; }

}  // namespace mk
