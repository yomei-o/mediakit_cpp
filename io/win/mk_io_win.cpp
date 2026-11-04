// mediakit io for Windows: Media Foundation camera, WASAPI microphone / speaker.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first
#include <audioclient.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>

#include "mk_io.h"
#include "mk_test_source.h"

using Microsoft::WRL::ComPtr;

namespace mk {

namespace {

// kFriendlyName (functiondiscoverykeys_devpkey.h needs initguid games)
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string to_utf8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring to_wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string hr_str(const char* what, HRESULT hr) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s failed (hr=0x%08lx)", what, (unsigned long)hr);
    return buf;
}

// COM for the calling thread; RPC_E_CHANGED_MODE (an STA UI thread) is fine too.
struct ComScope {
    HRESULT hr;
    ComScope() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

struct MfScope {
    MfScope() { MFStartup(MF_VERSION, MFSTARTUP_LITE); }
    ~MfScope() { MFShutdown(); }
};
void mf_once() { static MfScope mf; }

// QPC in 100 ns units -> mk::now_us() clock (steady_clock is QPC based on MSVC).
int64_t qpc100ns_to_us(int64_t t) {
    using namespace std::chrono;
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    // offset between the two scales, measured now
    int64_t qpc_us = (int64_t)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
    return now_us() - qpc_us + t / 10;
}

// ---------------------------------------------------------------- simple PCM helpers

// Any interleaved s16 -> channel count `out_ch`.
void remix(const int16_t* in, int frames, int in_ch, int out_ch, std::vector<int16_t>& out) {
    out.resize((size_t)frames * out_ch);
    for (int i = 0; i < frames; ++i) {
        const int16_t* s = in + (size_t)i * in_ch;
        int16_t* d = out.data() + (size_t)i * out_ch;
        if (out_ch == 1) {
            int sum = 0;
            for (int c = 0; c < in_ch; ++c) sum += s[c];
            d[0] = (int16_t)(sum / std::max(1, in_ch));
        } else {
            for (int c = 0; c < out_ch; ++c) d[c] = s[std::min(c, in_ch - 1)];
        }
    }
}

// Linear resampler with state carried between calls.
struct Resampler {
    int in_rate = 0, out_rate = 0, ch = 0;
    double pos = 0;  // position in input frames relative to `last`
    std::vector<int16_t> last;  // last input frame of the previous call
    void process(const int16_t* in, int frames, std::vector<int16_t>& out) {
        out.clear();
        if (in_rate == out_rate) {
            out.assign(in, in + (size_t)frames * ch);
            return;
        }
        if (last.empty()) last.assign(in, in + ch), pos = 1;  // start on the first sample
        const double step = (double)in_rate / out_rate;
        // virtual input: [last, in[0..frames-1]] with index 0 = last
        while (pos < frames) {
            int i0 = (int)pos;
            double t = pos - i0;
            for (int c = 0; c < ch; ++c) {
                double a = i0 == 0 ? last[c] : in[(size_t)(i0 - 1) * ch + c];
                double b = in[(size_t)i0 * ch + c];
                out.push_back((int16_t)std::lround(a + (b - a) * t));
            }
            pos += step;
        }
        pos -= frames;
        last.assign(in + (size_t)(frames - 1) * ch, in + (size_t)frames * ch);
    }
};

// Device mix format (float / s16 / s24 / s32) -> s16 interleaved.
void to_s16(const BYTE* data, int frames, const WAVEFORMATEX* wf, std::vector<int16_t>& out) {
    const int ch = wf->nChannels;
    out.resize((size_t)frames * ch);
    bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        is_float = ((const WAVEFORMATEXTENSIBLE*)wf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    const int bits = wf->wBitsPerSample;
    const size_t n = (size_t)frames * ch;
    if (is_float) {
        const float* f = (const float*)data;
        for (size_t i = 0; i < n; ++i) out[i] = (int16_t)std::lround(std::max(-1.0f, std::min(1.0f, f[i])) * 32767.0f);
    } else if (bits == 16) {
        memcpy(out.data(), data, n * 2);
    } else if (bits == 24) {
        for (size_t i = 0; i < n; ++i) out[i] = (int16_t)(data[i * 3 + 1] | (data[i * 3 + 2] << 8));
    } else if (bits == 32) {
        const int32_t* s = (const int32_t*)data;
        for (size_t i = 0; i < n; ++i) out[i] = (int16_t)(s[i] >> 16);
    } else {
        std::fill(out.begin(), out.end(), 0);
    }
}

WAVEFORMATEX s16_format(int rate, int ch) {
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)ch;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(ch * 2);
    wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
    return wf;
}

std::vector<DeviceInfo> list_endpoints(EDataFlow flow) {
    ComScope com;
    std::vector<DeviceInfo> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        DeviceInfo d;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) d.id = to_utf8(id), CoTaskMemFree(id);
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) d.name = to_utf8(v.pwszVal);
            PropVariantClear(&v);
        }
        out.push_back(d);
    }
    return out;
}

ComPtr<IMMDevice> find_endpoint(EDataFlow flow, const std::string& id, std::string& err) {
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) {
        err = hr_str("MMDeviceEnumerator", hr);
        return nullptr;
    }
    ComPtr<IMMDevice> dev;
    hr = id.empty() ? en->GetDefaultAudioEndpoint(flow, flow == eCapture ? eCommunications : eConsole, &dev)
                    : en->GetDevice(to_wide(id).c_str(), &dev);
    if (FAILED(hr) && id.empty()) hr = en->GetDefaultAudioEndpoint(flow, eConsole, &dev);
    if (FAILED(hr)) {
        err = flow == eCapture ? hr_str("no microphone", hr) : hr_str("no speaker", hr);
        return nullptr;
    }
    return dev;
}

std::string endpoint_name(IMMDevice* dev) {
    ComPtr<IPropertyStore> props;
    std::string name;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name = to_utf8(v.pwszVal);
        PropVariantClear(&v);
    }
    return name;
}

// Shared-mode client: try the wanted s16 format with Windows' converter, else the mix format.
// Returns the format in use (caller frees with CoTaskMemFree when `own` is set).
bool init_client(IAudioClient* client, int rate, int ch, WAVEFORMATEX** used, bool* converted, std::string& err) {
    const REFERENCE_TIME buffer = 2000000;  // 200 ms
    WAVEFORMATEX want = s16_format(rate, ch);
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    buffer, 0, &want, nullptr);
    if (SUCCEEDED(hr)) {
        *used = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        **used = want;
        *converted = true;
        return true;
    }
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) {
        err = hr_str("GetMixFormat", hr);
        return false;
    }
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buffer, 0, mix, nullptr);
    if (FAILED(hr)) {
        CoTaskMemFree(mix);
        err = hr_str("IAudioClient::Initialize", hr);
        return false;
    }
    *used = mix;
    *converted = false;
    return true;
}

}  // namespace

// ================================================================ cameras

std::vector<DeviceInfo> list_cameras() {
    ComScope com;
    mf_once();
    std::vector<DeviceInfo> out;
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 1);
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    if (FAILED(MFEnumDeviceSources(attr.Get(), &devs, &n))) return out;
    for (UINT32 i = 0; i < n; ++i) {
        DeviceInfo d;
        WCHAR* s = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &len))) d.name = to_utf8(s), CoTaskMemFree(s);
        if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &len)))
            d.id = to_utf8(s), CoTaskMemFree(s);
        out.push_back(d);
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    return out;
}

struct Camera::Impl {
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    int width = 0, height = 0;
    double fps = 0;
    LONG stride = 0;
    GUID subtype = GUID_NULL;  // what the reader delivers: NV12 / YUY2 / RGB32
    std::string name, err;
    std::thread th;
    std::atomic<bool> running{false};
    std::function<void(const VideoFrame&)> cb;
    bool test = false;
    TestSource test_src;

    void deliver(IMFSample* sample) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return;
        int64_t pts = now_us();
        UINT64 dev_ts = 0;
        if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DeviceTimestamp, &dev_ts)) && dev_ts) pts = qpc100ns_to_us((int64_t)dev_ts);
        BYTE* p = nullptr;
        LONG pitch = 0;
        DWORD len = 0;
        ComPtr<IMF2DBuffer> b2;
        bool locked2d = false;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch))) {
            locked2d = true;
            b2->GetContiguousLength(&len);
        } else {
            DWORD maxlen = 0;
            if (FAILED(buf->Lock(&p, &maxlen, &len))) return;
            pitch = stride ? stride : (subtype == MFVideoFormat_YUY2 ? width * 2 : subtype == MFVideoFormat_RGB32 ? width * 4 : width);
        }
        VideoFrame f;
        f.alloc(width, height, PixelFormat::NV12);
        f.pts_us = pts;
        if (subtype == MFVideoFormat_NV12) {
            const LONG ap = std::abs(pitch);
            int plane_h = height;  // some drivers pad the Y plane (1080 -> 1088)
            if (ap > 0 && len >= (DWORD)(ap * height * 3 / 2)) plane_h = std::max(height, (int)(len * 2 / 3 / ap));
            f.copy_plane(0, p, pitch, width, height);
            f.copy_plane(1, p + (size_t)ap * plane_h, pitch, (width + 1) / 2 * 2, (height + 1) / 2);
        } else if (subtype == MFVideoFormat_YUY2) {
            for (int y = 0; y < height; ++y) {
                const BYTE* s = p + (ptrdiff_t)y * pitch;
                uint8_t* dy = f.data[0] + (size_t)y * f.stride[0];
                for (int x = 0; x < width; ++x) dy[x] = s[x * 2];
                if ((y & 1) == 0) {
                    uint8_t* duv = f.data[1] + (size_t)(y / 2) * f.stride[1];
                    for (int x = 0; x + 1 < width; x += 2) duv[x] = s[x * 2 + 1], duv[x + 1] = s[x * 2 + 3];
                }
            }
        } else {  // RGB32 (BGRA), bottom-up when pitch is positive per MF convention for RGB
            for (int y = 0; y < height; ++y) {
                const BYTE* s = p + (ptrdiff_t)y * pitch;
                uint8_t* dy = f.data[0] + (size_t)y * f.stride[0];
                for (int x = 0; x < width; ++x) {
                    int b = s[x * 4], g = s[x * 4 + 1], r = s[x * 4 + 2];
                    dy[x] = (uint8_t)((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
                    if (!(y & 1) && !(x & 1)) {
                        uint8_t* duv = f.data[1] + (size_t)(y / 2) * f.stride[1] + x;
                        duv[0] = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
                        duv[1] = (uint8_t)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
                    }
                }
            }
        }
        if (locked2d) b2->Unlock2D();
        else buf->Unlock();
        if (cb) cb(f);
    }

    void loop() {
        ComScope com;
        while (running) {
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample);
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
                if (FAILED(hr)) err = hr_str("camera ReadSample", hr);
                else err = "camera stream ended";
                break;
            }
            if (sample && running) deliver(sample.Get());
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
        return true;
    }
    ComScope com;
    mf_once();
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 1);
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFEnumDeviceSources(attr.Get(), &devs, &n);
    if (FAILED(hr) || n == 0) {
        s.err = "no camera found";
        if (devs) CoTaskMemFree(devs);
        return false;
    }
    int pick = -1;
    for (UINT32 i = 0; i < n && pick < 0; ++i) {
        WCHAR* link = nullptr;
        UINT32 len = 0;
        if (cfg.device_id.empty()) pick = 0;
        else if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len))) {
            if (to_utf8(link) == cfg.device_id) pick = (int)i;
            CoTaskMemFree(link);
        }
    }
    if (pick < 0) {
        s.err = "camera not found: " + cfg.device_id;
    } else {
        WCHAR* nm = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devs[pick]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &nm, &len))) s.name = to_utf8(nm), CoTaskMemFree(nm);
        hr = devs[pick]->ActivateObject(IID_PPV_ARGS(&s.source));
        if (FAILED(hr)) s.err = hr_str("camera activate (in use or no permission?)", hr);
    }
    for (UINT32 i = 0; i < n; ++i) devs[i]->Release();
    CoTaskMemFree(devs);
    if (!s.source) return false;

    ComPtr<IMFAttributes> rattr;
    MFCreateAttributes(&rattr, 2);
    rattr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);  // YUY2 / MJPG -> NV12
    rattr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    hr = MFCreateSourceReaderFromMediaSource(s.source.Get(), rattr.Get(), &s.reader);
    if (FAILED(hr)) {
        s.err = hr_str("MFCreateSourceReaderFromMediaSource", hr);
        close();
        return false;
    }
    // Choose the native mode nearest to the request (size first, then fps, then cheap formats).
    ComPtr<IMFMediaType> best;
    double best_score = 1e30;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(s.reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t))) break;
        UINT32 w = 0, h = 0, num = 0, den = 1;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        GUID sub = GUID_NULL;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        double f = den ? (double)num / den : 0;
        double score = std::fabs(std::log((double)std::max(1u, w * h) / std::max(1, cfg.width * cfg.height))) * 100 +
                       std::fabs(f - cfg.fps) + (sub == MFVideoFormat_NV12 ? 0 : sub == MFVideoFormat_YUY2 ? 0.5 : 1);
        if (score < best_score) best_score = score, best = t;
    }
    if (best) s.reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, best.Get());
    const GUID wants[] = {MFVideoFormat_NV12, MFVideoFormat_YUY2, MFVideoFormat_RGB32};
    bool ok = false;
    for (const GUID& g : wants) {
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, g);
        if (SUCCEEDED(s.reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get()))) {
            ok = true;
            break;
        }
    }
    ComPtr<IMFMediaType> cur;
    if (!ok || FAILED(s.reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
        s.err = "camera has no NV12 / YUY2 / RGB32 output";
        close();
        return false;
    }
    UINT32 w = 0, h = 0, num = 0, den = 1;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &num, &den);
    cur->GetGUID(MF_MT_SUBTYPE, &s.subtype);
    UINT32 st = 0;
    s.stride = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? (LONG)(INT32)st : 0;
    s.width = (int)w & ~1, s.height = (int)h & ~1;
    s.fps = den ? (double)num / den : 0;
    return true;
}

bool Camera::start(std::function<void(const VideoFrame&)> on_frame) {
    Impl& s = *p_;
    if (s.test) {
        stop();
        s.test_src.start_video(s.width, s.height, s.fps, std::move(on_frame));
        return true;
    }
    if (!s.reader) {
        if (s.err.empty()) s.err = "camera not open";
        return false;
    }
    stop();
    s.cb = std::move(on_frame);
    s.running = true;
    s.th = std::thread([&s] { s.loop(); });
    return true;
}

void Camera::stop() {
    p_->test_src.stop();
    p_->running = false;
    if (p_->th.joinable()) p_->th.join();
}

void Camera::close() {
    stop();
    p_->reader.Reset();
    if (p_->source) p_->source->Shutdown();
    p_->source.Reset();
}

int Camera::width() const { return p_->width; }
int Camera::height() const { return p_->height; }
double Camera::fps() const { return p_->fps; }
std::string Camera::name() const { return p_->name; }
const std::string& Camera::error() const { return p_->err; }

// ================================================================ microphone

std::vector<DeviceInfo> list_microphones() { return list_endpoints(eCapture); }
std::vector<DeviceInfo> list_speakers() { return list_endpoints(eRender); }

struct Microphone::Impl {
    MicrophoneConfig cfg;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> cap;
    WAVEFORMATEX* wf = nullptr;
    bool converted = false;
    HANDLE ev = nullptr;
    std::thread th;
    std::atomic<bool> running{false};
    std::function<void(const AudioFrame&)> cb;
    Resampler rs;
    std::string name, err;
    bool test = false;
    TestSource test_src;

    void loop() {
        ComScope com;
        std::vector<int16_t> s16, mixed;
        client->Start();
        while (running) {
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
            for (;;) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 qpc = 0;
                HRESULT hr = cap->GetBuffer(&data, &frames, &flags, nullptr, &qpc);
                if (hr == AUDCLNT_S_BUFFER_EMPTY || frames == 0) {
                    if (SUCCEEDED(hr) && frames == 0 && hr != AUDCLNT_S_BUFFER_EMPTY) cap->ReleaseBuffer(0);
                    break;
                }
                if (FAILED(hr)) {
                    err = hr_str("capture GetBuffer", hr);
                    running = false;
                    break;
                }
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) s16.assign((size_t)frames * wf->nChannels, 0);
                else to_s16(data, frames, wf, s16);
                cap->ReleaseBuffer(frames);
                AudioFrame af;
                af.sample_rate = cfg.sample_rate;
                af.channels = cfg.channels;
                af.pts_us = qpc ? qpc100ns_to_us((int64_t)qpc) : now_us();
                if (converted) {
                    af.samples = std::move(s16);
                } else {
                    remix(s16.data(), (int)frames, wf->nChannels, cfg.channels, mixed);
                    rs.process(mixed.data(), (int)frames, af.samples);
                }
                if (cb && !af.samples.empty()) cb(af);
            }
        }
        client->Stop();
    }
};

Microphone::Microphone() : p_(new Impl) {}
Microphone::~Microphone() { close(); }

bool Microphone::open(const MicrophoneConfig& cfg) {
    close();
    ComScope com;
    Impl& s = *p_;
    s.cfg = cfg;
    s.err.clear();
    s.test = cfg.device_id == "test";
    if (s.test) {
        s.name = "test tone";
        return true;
    }
    s.dev = find_endpoint(eCapture, cfg.device_id, s.err);
    if (!s.dev) return false;
    s.name = endpoint_name(s.dev.Get());
    HRESULT hr = s.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)s.client.GetAddressOf());
    if (FAILED(hr)) {
        s.err = hr_str("microphone activate", hr);
        return false;
    }
    if (!init_client(s.client.Get(), cfg.sample_rate, cfg.channels, &s.wf, &s.converted, s.err)) return false;
    s.rs = Resampler();
    s.rs.in_rate = s.wf->nSamplesPerSec, s.rs.out_rate = cfg.sample_rate, s.rs.ch = cfg.channels;
    s.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.client->SetEventHandle(s.ev);
    hr = s.client->GetService(IID_PPV_ARGS(&s.cap));
    if (FAILED(hr)) {
        s.err = hr_str("IAudioCaptureClient", hr);
        return false;
    }
    return true;
}

bool Microphone::start(std::function<void(const AudioFrame&)> on_audio) {
    Impl& s = *p_;
    if (s.test) {
        stop();
        s.test_src.start_audio(s.cfg.sample_rate, s.cfg.channels, std::move(on_audio));
        return true;
    }
    if (!s.cap) {
        if (s.err.empty()) s.err = "microphone not open";
        return false;
    }
    stop();
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
    Impl& s = *p_;
    s.cap.Reset();
    s.client.Reset();
    s.dev.Reset();
    if (s.wf) CoTaskMemFree(s.wf), s.wf = nullptr;
    if (s.ev) CloseHandle(s.ev), s.ev = nullptr;
}

int Microphone::sample_rate() const { return p_->cfg.sample_rate; }
int Microphone::channels() const { return p_->cfg.channels; }
std::string Microphone::name() const { return p_->name; }
const std::string& Microphone::error() const { return p_->err; }

// ================================================================ speaker

struct Speaker::Impl {
    SpeakerConfig cfg;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    WAVEFORMATEX* wf = nullptr;
    bool converted = false;
    UINT32 buffer_frames = 0;
    HANDLE ev = nullptr;
    std::thread th;
    std::atomic<bool> running{false};
    std::string err;

    // queue of s16 at cfg rate/channels, with the media time of its first sample
    mutable std::mutex mu;
    std::deque<int16_t> q;
    int64_t q_pts = -1;
    Resampler rs;
    int rs_rate = 0, rs_ch = 0;

    // what has gone into the device: segments of real audio (pts) or silence (-1)
    struct Seg {
        int64_t start;  // device frame index
        int64_t frames;
        int64_t pts;
    };
    std::deque<Seg> segs;
    int64_t written = 0;      // frames handed to the device
    int64_t last_real_end = -1;
    UINT32 padding = 0;       // frames still in the device at last check

    void push_seg(int64_t frames, int64_t pts) {
        if (!segs.empty() && ((pts < 0 && segs.back().pts < 0) ||
                              (pts >= 0 && segs.back().pts >= 0 &&
                               segs.back().pts + segs.back().frames * 1000000 / cfg.sample_rate == pts))) {
            segs.back().frames += frames;
        } else {
            segs.push_back({written, frames, pts});
        }
        written += frames;
        while (segs.size() > 64) segs.pop_front();
    }

    void loop() {
        ComScope com;
        const int ch = cfg.channels;
        std::vector<int16_t> chunk, out_mixed;
        // prime with silence so Start has something to play
        fill();
        client->Start();
        while (running) {
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
            fill();
        }
        client->Stop();
        (void)ch;
    }

    void fill() {
        UINT32 pad = 0;
        if (FAILED(client->GetCurrentPadding(&pad))) return;
        UINT32 avail = buffer_frames - pad;
        if (avail == 0) {
            std::lock_guard<std::mutex> lk(mu);
            padding = pad;
            return;
        }
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(avail, &data))) return;
        const int ch = cfg.channels;
        std::lock_guard<std::mutex> lk(mu);
        UINT32 real = (UINT32)std::min<size_t>(avail, q.size() / ch);
        std::vector<int16_t> pcm((size_t)avail * ch, 0);
        for (size_t i = 0; i < (size_t)real * ch; ++i) pcm[i] = q[i];
        q.erase(q.begin(), q.begin() + (size_t)real * ch);
        if (real) {
            push_seg(real, q_pts);
            q_pts += (int64_t)real * 1000000 / cfg.sample_rate;
            last_real_end = q_pts;
        }
        if (avail > real) push_seg(avail - real, -1);
        if (converted) {
            memcpy(data, pcm.data(), pcm.size() * 2);
        } else {
            // device mix format: write float / s16 with channel copy (same rate assumed by init)
            const int dch = wf->nChannels;
            bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                            (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                             ((const WAVEFORMATEXTENSIBLE*)wf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            for (UINT32 i = 0; i < avail; ++i)
                for (int c = 0; c < dch; ++c) {
                    int16_t v = pcm[(size_t)i * ch + std::min(c, ch - 1)];
                    if (is_float) ((float*)data)[(size_t)i * dch + c] = v / 32768.0f;
                    else if (wf->wBitsPerSample == 16) ((int16_t*)data)[(size_t)i * dch + c] = v;
                    else if (wf->wBitsPerSample == 32) ((int32_t*)data)[(size_t)i * dch + c] = (int32_t)v << 16;
                }
        }
        render->ReleaseBuffer(avail, 0);
        padding = pad + avail;
    }
};

Speaker::Speaker() : p_(new Impl) {}
Speaker::~Speaker() { close(); }

bool Speaker::open(const SpeakerConfig& cfg) {
    close();
    ComScope com;
    Impl& s = *p_;
    s.cfg = cfg;
    s.err.clear();
    s.dev = find_endpoint(eRender, cfg.device_id, s.err);
    if (!s.dev) return false;
    HRESULT hr = s.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)s.client.GetAddressOf());
    if (FAILED(hr)) {
        s.err = hr_str("speaker activate", hr);
        return false;
    }
    if (!init_client(s.client.Get(), cfg.sample_rate, cfg.channels, &s.wf, &s.converted, s.err)) return false;
    if (!s.converted) s.cfg.sample_rate = s.wf->nSamplesPerSec;  // we resample to the device rate in write()
    s.client->GetBufferSize(&s.buffer_frames);
    s.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.client->SetEventHandle(s.ev);
    hr = s.client->GetService(IID_PPV_ARGS(&s.render));
    if (FAILED(hr)) {
        s.err = hr_str("IAudioRenderClient", hr);
        return false;
    }
    return true;
}

bool Speaker::start() {
    Impl& s = *p_;
    if (!s.render) {
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
    std::vector<int16_t> mixed, res;
    remix(f.samples.data(), f.sample_count(), f.channels, s.cfg.channels, mixed);
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.rs_rate != f.sample_rate || s.rs_ch != s.cfg.channels) {
        s.rs = Resampler();
        s.rs.in_rate = f.sample_rate, s.rs.out_rate = s.cfg.sample_rate, s.rs.ch = s.cfg.channels;
        s.rs_rate = f.sample_rate, s.rs_ch = s.cfg.channels;
    }
    s.rs.process(mixed.data(), f.sample_count(), res);
    if (s.q.empty()) s.q_pts = f.pts_us;
    s.q.insert(s.q.end(), res.begin(), res.end());
}

int64_t Speaker::played_pts_us() const {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    const int64_t heard = s.written - s.padding;  // device frame index being played
    int64_t pts = -1;
    for (auto& g : s.segs) {
        if (g.start > heard) break;
        if (g.pts >= 0) pts = g.pts + (std::min(heard, g.start + g.frames) - g.start) * 1000000 / s.cfg.sample_rate;
    }
    return pts;
}

int64_t Speaker::queued_us() const {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    return (int64_t)(s.q.size() / std::max(1, s.cfg.channels) + s.padding) * 1000000 / s.cfg.sample_rate;
}

void Speaker::clear() {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    s.q.clear();
    s.q_pts = -1;
    s.segs.clear();
}

void Speaker::stop() {
    p_->running = false;
    if (p_->th.joinable()) p_->th.join();
}

void Speaker::close() {
    stop();
    Impl& s = *p_;
    s.render.Reset();
    s.client.Reset();
    s.dev.Reset();
    if (s.wf) CoTaskMemFree(s.wf), s.wf = nullptr;
    if (s.ev) CloseHandle(s.ev), s.ev = nullptr;
    clear();
}

const std::string& Speaker::error() const { return p_->err; }

}  // namespace mk
