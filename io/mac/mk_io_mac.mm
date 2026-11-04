// mediakit io for macOS / iOS: AVFoundation camera, AudioQueue microphone / speaker.
//
// AVFoundation converts whatever the camera sends natively to NV12 for us (as Media Foundation
// does on Windows), so the frames handed out are always NV12 and format() names the native mode.
// AudioQueue is used for both capture and playback because it is the same API on macOS and iOS
// and it resamples to the rate we ask for.
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <CoreAudio/CoreAudio.h>
#endif
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "mk_io.h"
#include "mk_test_source.h"

namespace mk {
namespace {

int64_t host_ticks_to_us(uint64_t ticks) {
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return (int64_t)((__int128)ticks * tb.numer / tb.denom / 1000);
}

int64_t host_now_us() { return host_ticks_to_us(mach_absolute_time()); }

// The camera's CMSampleBuffer stamps and the AudioQueue's AudioTimeStamp are both on the host
// clock. One anchor for the whole process, so video and audio keep their relative timing.
int64_t host_us_to_now_us(int64_t host_us) {
    static const int64_t off = now_us() - host_now_us();
    return host_us + off;
}

std::string fourcc_name(uint32_t f) {
    switch (f) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange: return "NV12";
        case kCVPixelFormatType_422YpCbCr8_yuvs: return "YUY2";
        case kCVPixelFormatType_422YpCbCr8: return "UYVY";
        case kCVPixelFormatType_420YpCbCr8Planar:
        case kCVPixelFormatType_420YpCbCr8PlanarFullRange: return "I420";
        case kCVPixelFormatType_32BGRA: return "BGRA";
        case kCMVideoCodecType_JPEG:
        case kCMVideoCodecType_JPEG_OpenDML: return "MJPG";
        case kCMVideoCodecType_H264: return "H264";
    }
    char b[5] = {(char)((f >> 24) & 0xff), (char)((f >> 16) & 0xff), (char)((f >> 8) & 0xff), (char)(f & 0xff), 0};
    return b;
}

NSArray<AVCaptureDevice*>* video_devices() {
    NSMutableArray* types = [NSMutableArray array];
    [types addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
#if TARGET_OS_OSX
    if (@available(macOS 14.0, *)) {
        [types addObject:AVCaptureDeviceTypeExternal];
        [types addObject:AVCaptureDeviceTypeContinuityCamera];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
    }
#endif
    return [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                                 mediaType:AVMediaTypeVideo
                                                                  position:AVCaptureDevicePositionUnspecified]
        .devices;
}

// Some cameras (the ones on the legacy DAL path) only accept the exact CMTime out of their own
// AVFrameRateRange, not an equivalent rational, and throw an NSException for anything else.
void apply_format(AVCaptureDevice* dev, AVCaptureDeviceFormat* fmt, AVFrameRateRange* range, double fps) {
    if (![dev lockForConfiguration:nil]) return;
    @try {
        dev.activeFormat = fmt;
    } @catch (NSException*) {
    }
    CMTime want = CMTimeMake(1, (int32_t)std::max(1L, std::lround(fps)));
    if (CMTimeCompare(want, range.minFrameDuration) < 0 || CMTimeCompare(want, range.maxFrameDuration) > 0)
        want = range.minFrameDuration;
    for (CMTime dur : {want, range.minFrameDuration}) {
        @try {
            dev.activeVideoMinFrameDuration = dur;  // at most this many frames per second
            dev.activeVideoMaxFrameDuration = dur;  // and do not let auto exposure slow it down
            break;
        } @catch (NSException*) {
        }
    }
    [dev unlockForConfiguration];
}

AVCaptureDevice* find_camera(const std::string& id) {
    if (id.empty()) {
        NSArray<AVCaptureDevice*>* all = video_devices();
        return all.count ? all[0] : [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    }
    NSString* want = [NSString stringWithUTF8String:id.c_str()];
    for (AVCaptureDevice* d in video_devices())
        if ([d.uniqueID isEqualToString:want]) return d;
    return nil;
}

// The user has to allow camera / microphone access once; a denied device just fails to open.
bool ensure_access(AVMediaType type) {
    if ([AVCaptureDevice authorizationStatusForMediaType:type] == AVAuthorizationStatusAuthorized) return true;
    __block bool ok = false;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [AVCaptureDevice requestAccessForMediaType:type
                             completionHandler:^(BOOL granted) {
                                 ok = granted;
                                 dispatch_semaphore_signal(sem);
                             }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 60ll * NSEC_PER_SEC));
    return ok;
}

std::string to_utf8(NSString* s) { return s ? std::string(s.UTF8String) : std::string(); }

}  // namespace
}  // namespace mk

// ================================================================ cameras

@interface MkVideoDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, assign) mk::Camera::Impl* owner;
@end

namespace mk {

std::vector<DeviceInfo> list_cameras() {
    std::vector<DeviceInfo> out;
    @autoreleasepool {
        for (AVCaptureDevice* d in video_devices()) out.push_back({to_utf8(d.uniqueID), to_utf8(d.localizedName)});
    }
    return out;
}

std::vector<CameraMode> list_camera_modes(const std::string& device_id) {
    if (device_id == "test") return {{1280, 720, 30, "NV12"}, {640, 480, 30, "NV12"}};
    std::vector<CameraMode> out;
    @autoreleasepool {
        AVCaptureDevice* d = find_camera(device_id);
        for (AVCaptureDeviceFormat* f in d.formats) {
            CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
            const std::string name = fourcc_name(CMFormatDescriptionGetMediaSubType(f.formatDescription));
            if (f.videoSupportedFrameRateRanges.count == 0) out.push_back({dim.width, dim.height, 0, name});
            for (AVFrameRateRange* r in f.videoSupportedFrameRateRanges) out.push_back({dim.width, dim.height, r.maxFrameRate, name});
        }
    }
    return out;
}

struct Camera::Impl {
    AVCaptureSession* session = nil;
    AVCaptureDevice* device = nil;
    AVCaptureVideoDataOutput* output = nil;
    MkVideoDelegate* delegate = nil;
    dispatch_queue_t queue = nullptr;

    int width = 0, height = 0;
    double fps = 0;
    std::string name, err, native;

    std::mutex cb_mu;
    std::function<void(const VideoFrame&)> cb;
    bool running = false;
    bool test = false;
    TestSource test_src;

    void deliver(CMSampleBufferRef sb) {
        CVImageBufferRef img = CMSampleBufferGetImageBuffer(sb);
        if (!img) return;
        std::unique_lock<std::mutex> lk(cb_mu);
        if (!running || !cb) return;
        CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        const int w = (int)CVPixelBufferGetWidth(img) & ~1, h = (int)CVPixelBufferGetHeight(img) & ~1;
        const OSType pf = CVPixelBufferGetPixelFormatType(img);
        VideoFrame f;
        if (pf == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || pf == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
            f.alloc(w, h, PixelFormat::NV12);
            f.copy_plane(0, (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(img, 0), (int)CVPixelBufferGetBytesPerRowOfPlane(img, 0), w, h);
            f.copy_plane(1, (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(img, 1), (int)CVPixelBufferGetBytesPerRowOfPlane(img, 1), w, h / 2);
        } else if (pf == kCVPixelFormatType_32BGRA) {
            f.alloc(w, h, PixelFormat::BGRA);
            f.copy_plane(0, (const uint8_t*)CVPixelBufferGetBaseAddress(img), (int)CVPixelBufferGetBytesPerRow(img), w * 4, h);
        }
        CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        if (f.empty()) return;
        const CMTime t = CMSampleBufferGetPresentationTimeStamp(sb);
        f.pts_us = CMTIME_IS_NUMERIC(t) ? host_us_to_now_us(CMTimeConvertScale(t, 1000000, kCMTimeRoundingMethod_Default).value) : now_us();
        cb(f);
    }
};

}  // namespace mk

@implementation MkVideoDelegate
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection {
    (void)output, (void)connection;
    if (self.owner) self.owner->deliver(sampleBuffer);
}
@end

namespace mk {

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
    @autoreleasepool {
        if (!ensure_access(AVMediaTypeVideo)) {
            s.err = "camera access was not granted";
            return false;
        }
        AVCaptureDevice* dev = find_camera(cfg.device_id);
        if (!dev) {
            s.err = cfg.device_id.empty() ? "no camera found" : "no such camera: " + cfg.device_id;
            return false;
        }
        // Nearest native mode: size first, then fps, then cheap formats (NV12 < YUY2 < MJPG).
        AVCaptureDeviceFormat* best = nil;
        AVFrameRateRange* best_range = nil;
        double best_fps = 0, best_score = 1e30;
        for (AVCaptureDeviceFormat* f in dev.formats) {
            CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
            const std::string fmt = fourcc_name(CMFormatDescriptionGetMediaSubType(f.formatDescription));
            for (AVFrameRateRange* r in f.videoSupportedFrameRateRanges) {
                const double rate = std::min(r.maxFrameRate, std::max(r.minFrameRate, (double)cfg.fps));
                double score = std::fabs(std::log((double)std::max(1, dim.width * dim.height) / std::max(1, cfg.width * cfg.height))) * 100 +
                               std::fabs(rate - cfg.fps) + (fmt == "NV12" ? 0 : fmt == "YUY2" ? 0.5 : 1);
                if (!cfg.format.empty() && fmt != cfg.format) score += 1000;
                if (score < best_score) best_score = score, best = f, best_range = r, best_fps = rate;
            }
        }
        if (!best) {
            s.err = "camera has no usable mode";
            return false;
        }
        NSError* nserr = nil;
        AVCaptureDeviceInput* in = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&nserr];
        if (!in) {
            s.err = "deviceInputWithDevice: " + to_utf8(nserr.localizedDescription);
            return false;
        }
        s.session = [[AVCaptureSession alloc] init];
#if TARGET_OS_IPHONE
        s.session.sessionPreset = AVCaptureSessionPresetInputPriority;  // do not let the preset pick the mode
#endif
        [s.session beginConfiguration];
        if (![s.session canAddInput:in]) {
            [s.session commitConfiguration];
            s.session = nil;
            s.err = "the camera is already in use";
            return false;
        }
        [s.session addInput:in];
        s.output = [[AVCaptureVideoDataOutput alloc] init];
        s.output.alwaysDiscardsLateVideoFrames = YES;
        s.output.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
        s.delegate = [[MkVideoDelegate alloc] init];
        s.delegate.owner = &s;
        s.queue = dispatch_queue_create("mk.camera", DISPATCH_QUEUE_SERIAL);
        [s.output setSampleBufferDelegate:s.delegate queue:s.queue];
        [s.session addOutput:s.output];
        [s.session commitConfiguration];

        // The mode only sticks once the session is running: before that the session applies its own
        // preset to the device and the activeFormat set here is thrown away (this camera then ran
        // at 1920x1080 and 5 fps whatever was asked for). It does survive a later stop / start.
        // The session keeps running from open() to close(); start() / stop() only gate delivery.
        // Stopping it would let the session re-apply its preset and undo the mode.
        [s.session startRunning];
        apply_format(dev, best, best_range, best_fps);

        s.device = dev;
        s.name = to_utf8(dev.localizedName);
        // report what the device really ended up with
        AVCaptureDeviceFormat* active = dev.activeFormat ?: best;
        s.native = fourcc_name(CMFormatDescriptionGetMediaSubType(active.formatDescription));
        CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(active.formatDescription);
        s.width = dim.width & ~1, s.height = dim.height & ~1;
        const double d = CMTimeGetSeconds(dev.activeVideoMinFrameDuration);
        s.fps = d > 0 ? 1.0 / d : best_fps;
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
    if (!s.session) {
        if (s.err.empty()) s.err = "camera not open";
        return false;
    }
    std::lock_guard<std::mutex> lk(s.cb_mu);
    s.cb = std::move(on_frame);
    s.running = true;
    return true;
}

void Camera::stop() {
    Impl& s = *p_;
    s.test_src.stop();
    std::lock_guard<std::mutex> lk(s.cb_mu);  // a delivery in flight holds this until it returns
    s.running = false;
    s.cb = nullptr;
}

void Camera::close() {
    Impl& s = *p_;
    stop();
    @autoreleasepool {
        if (s.session) [s.session stopRunning];  // waits for the delivery queue to drain
        if (s.output) [s.output setSampleBufferDelegate:nil queue:nullptr];
        if (s.delegate) s.delegate.owner = nullptr;
        s.output = nil;
        s.delegate = nil;
        s.session = nil;
        s.device = nil;
    }
    s.queue = nullptr;
}

int Camera::width() const { return p_->width; }
int Camera::height() const { return p_->height; }
double Camera::fps() const { return p_->fps; }
std::string Camera::format() const { return p_->test ? "NV12" : p_->native; }
std::string Camera::name() const { return p_->name; }
const std::string& Camera::error() const { return p_->err; }

// ================================================================ audio devices

namespace {

#if TARGET_OS_OSX
std::string device_string(AudioObjectID id, AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef s = nullptr;
    UInt32 n = sizeof(s);
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &n, &s) != noErr || !s) return {};
    char buf[512] = {0};
    CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}

bool device_has_channels(AudioObjectID id, bool input) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreamConfiguration,
                                    input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
                                    kAudioObjectPropertyElementMain};
    UInt32 n = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, nullptr, &n) != noErr || !n) return false;
    std::vector<uint8_t> raw(n);
    AudioBufferList* bl = (AudioBufferList*)raw.data();
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &n, bl) != noErr) return false;
    for (UInt32 i = 0; i < bl->mNumberBuffers; ++i)
        if (bl->mBuffers[i].mNumberChannels) return true;
    return false;
}
#endif

std::vector<DeviceInfo> list_audio(bool input) {
    std::vector<DeviceInfo> out;
#if TARGET_OS_OSX
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 n = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &n) != noErr) return out;
    std::vector<AudioObjectID> ids(n / sizeof(AudioObjectID));
    if (ids.empty() || AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &n, ids.data()) != noErr) return out;
    for (AudioObjectID id : ids) {
        if (!device_has_channels(id, input)) continue;
        std::string uid = device_string(id, kAudioDevicePropertyDeviceUID);
        std::string name = device_string(id, kAudioObjectPropertyName);
        if (!uid.empty()) out.push_back({uid, name.empty() ? uid : name});
    }
#else
    (void)input;
    out.push_back({"", "default"});
#endif
    return out;
}

// s16 interleaved, as the rest of mediakit expects; AudioQueue converts from / to the device.
AudioStreamBasicDescription pcm_s16(int rate, int ch) {
    AudioStreamBasicDescription d = {};
    d.mSampleRate = rate;
    d.mFormatID = kAudioFormatLinearPCM;
    d.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    d.mChannelsPerFrame = ch;
    d.mBitsPerChannel = 16;
    d.mFramesPerPacket = 1;
    d.mBytesPerFrame = 2 * ch;
    d.mBytesPerPacket = d.mBytesPerFrame;
    return d;
}

#if TARGET_OS_OSX
void set_queue_device(AudioQueueRef q, const std::string& uid) {
    if (uid.empty()) return;
    CFStringRef s = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    AudioQueueSetProperty(q, kAudioQueueProperty_CurrentDevice, &s, sizeof(s));
    CFRelease(s);
}
std::string device_name_for_uid(const std::string& uid, bool input) {
    if (uid.empty()) return "default";
    for (auto& d : list_audio(input))
        if (d.id == uid) return d.name;
    return uid;
}
#else
void set_queue_device(AudioQueueRef, const std::string&) {}
std::string device_name_for_uid(const std::string& uid, bool) { return uid.empty() ? "default" : uid; }
#endif

const int kAudioBuffers = 4;  // 4 x 20 ms

}  // namespace

std::vector<DeviceInfo> list_microphones() { return list_audio(true); }
std::vector<DeviceInfo> list_speakers() { return list_audio(false); }

// ================================================================ microphone

struct Microphone::Impl {
    MicrophoneConfig cfg;
    AudioQueueRef q = nullptr;
    AudioQueueBufferRef bufs[kAudioBuffers] = {};
    std::function<void(const AudioFrame&)> cb;
    std::string name, err;
    std::atomic<bool> running{false};
    bool test = false;
    TestSource test_src;

    static void on_buffer(void* user, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp* ts, UInt32, const AudioStreamPacketDescription*) {
        Impl& s = *(Impl*)user;
        if (!s.running) return;
        const int ch = s.cfg.channels;
        const int frames = (int)(b->mAudioDataByteSize / (2 * ch));
        if (frames > 0 && s.cb) {
            AudioFrame a;
            a.sample_rate = s.cfg.sample_rate, a.channels = ch;
            a.samples.assign((const int16_t*)b->mAudioData, (const int16_t*)b->mAudioData + (size_t)frames * ch);
            // the first sample of this buffer was captured at the host time the queue reports
            a.pts_us = (ts && (ts->mFlags & kAudioTimeStampHostTimeValid))
                           ? host_us_to_now_us(host_ticks_to_us(ts->mHostTime))
                           : now_us() - (int64_t)frames * 1000000 / s.cfg.sample_rate;
            s.cb(a);
        }
        AudioQueueEnqueueBuffer(q, b, 0, nullptr);
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
    @autoreleasepool {
        if (!ensure_access(AVMediaTypeAudio)) {
            s.err = "microphone access was not granted";
            return false;
        }
    }
    AudioStreamBasicDescription fmt = pcm_s16(cfg.sample_rate, cfg.channels);
    OSStatus r = AudioQueueNewInput(&fmt, &Impl::on_buffer, &s, nullptr, nullptr, 0, &s.q);
    if (r != noErr) {
        s.err = "AudioQueueNewInput failed (" + std::to_string((int)r) + ")";
        return false;
    }
    set_queue_device(s.q, cfg.device_id);
    const UInt32 bytes = (UInt32)(cfg.sample_rate / 50) * fmt.mBytesPerFrame;  // 20 ms
    for (int i = 0; i < kAudioBuffers; ++i) {
        if (AudioQueueAllocateBuffer(s.q, bytes, &s.bufs[i]) != noErr) {
            s.err = "AudioQueueAllocateBuffer failed";
            close();
            return false;
        }
    }
    s.name = device_name_for_uid(cfg.device_id, true);
    return true;
}

bool Microphone::start(std::function<void(const AudioFrame&)> on_audio) {
    Impl& s = *p_;
    stop();
    if (s.test) {
        s.test_src.start_audio(s.cfg.sample_rate, s.cfg.channels, std::move(on_audio));
        return true;
    }
    if (!s.q) {
        if (s.err.empty()) s.err = "microphone not open";
        return false;
    }
    s.cb = std::move(on_audio);
    s.running = true;
    for (int i = 0; i < kAudioBuffers; ++i) AudioQueueEnqueueBuffer(s.q, s.bufs[i], 0, nullptr);
    OSStatus r = AudioQueueStart(s.q, nullptr);
    if (r != noErr) {
        s.running = false;
        s.err = "AudioQueueStart failed (" + std::to_string((int)r) + ")";
        return false;
    }
    return true;
}

void Microphone::stop() {
    Impl& s = *p_;
    s.test_src.stop();
    if (!s.running) return;
    s.running = false;
    AudioQueueStop(s.q, true);  // synchronous: no callback is in flight afterwards
    s.cb = nullptr;
}

void Microphone::close() {
    Impl& s = *p_;
    stop();
    if (s.q) AudioQueueDispose(s.q, true), s.q = nullptr;
    for (auto& b : s.bufs) b = nullptr;
}

int Microphone::sample_rate() const { return p_->cfg.sample_rate; }
int Microphone::channels() const { return p_->cfg.channels; }
std::string Microphone::name() const { return p_->name; }
const std::string& Microphone::error() const { return p_->err; }

// ================================================================ speaker

struct Speaker::Impl {
    SpeakerConfig cfg;
    AudioQueueRef q = nullptr;
    AudioQueueBufferRef bufs[kAudioBuffers] = {};
    int chunk = 0;  // frames per buffer
    std::string err;
    std::atomic<bool> running{false};

    mutable std::mutex mu;
    std::deque<int16_t> qd;
    int64_t q_pts = -1;         // media time of qd.front()
    int64_t written_end = -1;   // media time just after the last real sample handed to the queue
    int64_t handed_frames = 0;  // frames handed to AudioQueue since start
    int64_t silence_after = 0;  // silence frames written after written_end (underrun padding)
    mutable int64_t delay_frames = 0;
    // linear resampler state for write() at another rate
    int rs_rate = 0;
    double rs_pos = 0;
    std::vector<int16_t> rs_last;

    // Fill one buffer from the queue, padding with silence, and hand it back to AudioQueue.
    void fill(AudioQueueBufferRef b) {
        const int ch = cfg.channels;
        int16_t* dst = (int16_t*)b->mAudioData;
        int real = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            real = (int)std::min<size_t>(chunk, qd.size() / ch);
            std::copy(qd.begin(), qd.begin() + (size_t)real * ch, dst);
            qd.erase(qd.begin(), qd.begin() + (size_t)real * ch);
            if (real) {
                q_pts += (int64_t)real * 1000000 / cfg.sample_rate;
                written_end = q_pts;
                silence_after = 0;
            }
            silence_after += chunk - real;
            handed_frames += chunk;
        }
        std::fill(dst + (size_t)real * ch, dst + (size_t)chunk * ch, 0);
        b->mAudioDataByteSize = (UInt32)chunk * 2 * ch;
        AudioQueueEnqueueBuffer(q, b, 0, nullptr);
    }

    static void on_buffer(void* user, AudioQueueRef, AudioQueueBufferRef b) {
        Impl& s = *(Impl*)user;
        if (s.running) s.fill(b);
    }

    // frames the device has actually played since AudioQueueStart
    void refresh_delay() const {
        AudioTimeStamp ts = {};
        Boolean disc = false;
        int64_t played = 0;
        if (q && running && AudioQueueGetCurrentTime(q, nullptr, &ts, &disc) == noErr && (ts.mFlags & kAudioTimeStampSampleTimeValid))
            played = (int64_t)ts.mSampleTime;
        std::lock_guard<std::mutex> lk(mu);
        delay_frames = std::max<int64_t>(0, handed_frames - played);
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
    AudioStreamBasicDescription fmt = pcm_s16(cfg.sample_rate, cfg.channels);
    OSStatus r = AudioQueueNewOutput(&fmt, &Impl::on_buffer, &s, nullptr, nullptr, 0, &s.q);
    if (r != noErr) {
        s.err = "AudioQueueNewOutput failed (" + std::to_string((int)r) + ")";
        return false;
    }
    set_queue_device(s.q, cfg.device_id);
    s.chunk = cfg.sample_rate / 50;  // 20 ms
    for (int i = 0; i < kAudioBuffers; ++i) {
        if (AudioQueueAllocateBuffer(s.q, (UInt32)s.chunk * fmt.mBytesPerFrame, &s.bufs[i]) != noErr) {
            s.err = "AudioQueueAllocateBuffer failed";
            close();
            return false;
        }
    }
    return true;
}

bool Speaker::start() {
    Impl& s = *p_;
    if (!s.q) {
        if (s.err.empty()) s.err = "speaker not open";
        return false;
    }
    if (s.running) return true;
    s.running = true;
    for (int i = 0; i < kAudioBuffers; ++i) s.fill(s.bufs[i]);  // prime with silence
    OSStatus r = AudioQueueStart(s.q, nullptr);
    if (r != noErr) {
        s.running = false;
        s.err = "AudioQueueStart failed (" + std::to_string((int)r) + ")";
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
    if (s.qd.empty()) s.q_pts = f.pts_us;
    if (f.sample_rate == s.cfg.sample_rate) {
        s.qd.insert(s.qd.end(), mixed.begin(), mixed.end());
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
            s.qd.push_back((int16_t)std::lround(a + (b - a) * t));
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
    const int64_t q_us = (int64_t)(s.qd.size() / std::max(1, s.cfg.channels)) * 1000000 / s.cfg.sample_rate;
    const int64_t end = !s.qd.empty() ? s.q_pts + q_us : s.written_end;
    const int64_t played = s.played_locked();
    if (end < 0) return 0;
    if (played < 0) return q_us;
    return std::max<int64_t>(0, end - played);
}

void Speaker::clear() {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    s.qd.clear();
    s.q_pts = s.written_end = -1;
    s.silence_after = 0;
    s.rs_last.clear();
}

void Speaker::stop() {
    Impl& s = *p_;
    if (!s.running) return;
    s.running = false;
    AudioQueueStop(s.q, true);
    std::lock_guard<std::mutex> lk(s.mu);
    s.handed_frames = s.delay_frames = 0;
}

void Speaker::close() {
    Impl& s = *p_;
    stop();
    if (s.q) AudioQueueDispose(s.q, true), s.q = nullptr;
    for (auto& b : s.bufs) b = nullptr;
    clear();
}

const std::string& Speaker::error() const { return p_->err; }

}  // namespace mk
