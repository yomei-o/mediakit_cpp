// mediakit ui for Android: NativeActivity + ANativeWindow. There is no toolkit in the NDK, so the
// button row and the status line are drawn by hand, the way the X11 implementation does it; the
// text itself goes through JNI to android.graphics.Paint, which is the only thing in reach that
// can draw Japanese.
//
// The entry point of a NativeActivity is android_main(), not main(). This file provides one: it
// hands FFmpeg the app's JavaVM (so MediaCodec can be used) and then calls the program's main(),
// so the examples build here unchanged.
//
// There is no file picker in the NDK. choose_save_file() returns a path in the app's external
// files directory, and choose_open_file() returns the newest matching file already there.
#include <android/bitmap.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "mk_codec.h"
#include "mk_ui.h"

extern int main(int argc, char** argv);

namespace mk {
namespace {

android_app* g_app = nullptr;
float g_scale = 2.0f;  // pixels per design unit (screen density / 160)

const float kBarH = 52, kStatusH = 28, kBtnH = 40, kPad = 8, kTextSize = 15, kStatusSize = 12;

void logi(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "mediakit", fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------- text through android.graphics

struct Jni {
    JNIEnv* env = nullptr;
    jclass bitmap = nullptr, config = nullptr, canvas = nullptr, paint = nullptr;
    jmethodID create_bitmap = nullptr, canvas_ctor = nullptr, draw_text = nullptr;
    jmethodID paint_ctor = nullptr, set_text_size = nullptr, set_color = nullptr, set_aa = nullptr;
    jmethodID measure_text = nullptr, get_font_metrics = nullptr;
    jobject argb8888 = nullptr;
    bool ok = false;

    void init() {
        if (ok || !g_app) return;
        JavaVM* vm = g_app->activity->vm;
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
        auto find = [&](const char* n) {
            jclass c = env->FindClass(n);
            return c ? (jclass)env->NewGlobalRef(c) : nullptr;
        };
        bitmap = find("android/graphics/Bitmap");
        config = find("android/graphics/Bitmap$Config");
        canvas = find("android/graphics/Canvas");
        paint = find("android/graphics/Paint");
        if (!bitmap || !config || !canvas || !paint) return;
        create_bitmap = env->GetStaticMethodID(bitmap, "createBitmap",
                                               "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
        canvas_ctor = env->GetMethodID(canvas, "<init>", "(Landroid/graphics/Bitmap;)V");
        draw_text = env->GetMethodID(canvas, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
        paint_ctor = env->GetMethodID(paint, "<init>", "()V");
        set_text_size = env->GetMethodID(paint, "setTextSize", "(F)V");
        set_color = env->GetMethodID(paint, "setColor", "(I)V");
        set_aa = env->GetMethodID(paint, "setAntiAlias", "(Z)V");
        measure_text = env->GetMethodID(paint, "measureText", "(Ljava/lang/String;)F");
        jfieldID f = env->GetStaticFieldID(config, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
        jobject local = f ? env->GetStaticObjectField(config, f) : nullptr;
        if (local) argb8888 = env->NewGlobalRef(local);
        ok = create_bitmap && canvas_ctor && draw_text && paint_ctor && set_text_size && set_color && measure_text && argb8888;
        if (!ok) logi("ui: android.graphics not reachable, text will be missing");
    }
};

Jni& jni() {
    static Jni j;
    j.init();
    return j;
}

// A string rendered once into BGRA pixels; drawing it later is a blend.
struct TextImage {
    int w = 0, h = 0, baseline = 0;
    std::vector<uint8_t> argb;  // premultiplied ARGB_8888 as android gives it
};

bool render_text(const std::string& utf8, float size_px, TextImage& out) {
    Jni& j = jni();
    if (!j.ok || utf8.empty()) return false;
    JNIEnv* e = j.env;
    jstring s = e->NewStringUTF(utf8.c_str());
    jobject p = e->NewObject(j.paint, j.paint_ctor);
    e->CallVoidMethod(p, j.set_aa, JNI_TRUE);
    e->CallVoidMethod(p, j.set_text_size, size_px);
    e->CallVoidMethod(p, j.set_color, (jint)0xffffffff);
    const float width = e->CallFloatMethod(p, j.measure_text, s);
    const int w = std::max(1, (int)(width + 2));
    const int h = std::max(1, (int)(size_px * 1.5f + 2));
    const int baseline = (int)(size_px * 1.15f);
    jobject bmp = e->CallStaticObjectMethod(j.bitmap, j.create_bitmap, w, h, j.argb8888);
    if (!bmp) {
        e->DeleteLocalRef(s), e->DeleteLocalRef(p);
        return false;
    }
    jobject cv = e->NewObject(j.canvas, j.canvas_ctor, bmp);
    e->CallVoidMethod(cv, j.draw_text, s, 1.0f, (jfloat)baseline, p);
    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(e, bmp, &pixels) == ANDROID_BITMAP_RESULT_SUCCESS && pixels) {
        out.w = w, out.h = h, out.baseline = baseline;
        out.argb.assign((uint8_t*)pixels, (uint8_t*)pixels + (size_t)w * 4 * h);
        AndroidBitmap_unlockPixels(e, bmp);
    }
    e->DeleteLocalRef(cv), e->DeleteLocalRef(bmp), e->DeleteLocalRef(p), e->DeleteLocalRef(s);
    return !out.argb.empty();
}

}  // namespace

// ---------------------------------------------------------------- the window

struct Window::Impl {
    ANativeWindow* win = nullptr;
    int w = 0, h = 0;
    bool open = false;
    bool want_close = false;

    struct Button {
        std::string label;
        std::function<void()> click;
        bool enabled = true;
        bool down = false;
        float x = 0, w = 0;
        TextImage text;
        bool text_ready = false;
    };
    std::vector<Button> buttons;
    std::function<void()> closing;

    struct Timer {
        int interval_ms = 0;
        int64_t next = 0;
        std::function<void()> fn;
    };
    std::vector<Timer> timers;

    std::mutex mu;  // everything below is touched from other threads
    std::vector<uint8_t> pixels;  // BGRA top-down, the latest frame
    int fw = 0, fh = 0;
    bool frame_dirty = true;
    std::string status;
    bool status_dirty = true;
    TextImage status_text;
    std::deque<std::function<void()>> posted;

    float sc(float v) const { return v * g_scale; }

    void draw_text_at(uint32_t* dst, int dw, int dh, const TextImage& t, int x, int y, uint32_t rgb) {
        if (t.argb.empty()) return;
        for (int ty = 0; ty < t.h; ++ty) {
            const int py = y + ty;
            if (py < 0 || py >= dh) continue;
            const uint8_t* src = t.argb.data() + (size_t)ty * t.w * 4;
            uint32_t* row = dst + (size_t)py * dw;
            for (int tx = 0; tx < t.w; ++tx) {
                const int px = x + tx;
                if (px < 0 || px >= dw) continue;
                const uint8_t a = src[tx * 4 + 3];  // the glyph is drawn white, so alpha is coverage
                if (!a) continue;
                const uint32_t bg = row[px];
                auto mix = [&](int shift) {
                    const int s = (rgb >> shift) & 0xff, b = (bg >> shift) & 0xff;
                    return (uint32_t)((s * a + b * (255 - a)) / 255) << shift;
                };
                row[px] = 0xff000000u | mix(0) | mix(8) | mix(16);
            }
        }
    }

    // The window is RGBA_8888: byte 0 is red. Frames are BGRA, so red and blue swap.
    void blit_frame(uint32_t* dst, int dw, int dh, int top, int bottom) {
        std::lock_guard<std::mutex> lk(mu);
        const int vh = bottom - top;
        if (vh <= 0) return;
        for (int y = top; y < bottom; ++y) std::fill(dst + (size_t)y * dw, dst + (size_t)y * dw + dw, 0xff000000u);
        if (pixels.empty() || fw <= 0 || fh <= 0) return;
        const double scale = std::min((double)dw / fw, (double)vh / fh);
        const int ow = (int)(fw * scale), oh = (int)(fh * scale);
        const int ox = (dw - ow) / 2, oy = top + (vh - oh) / 2;
        for (int y = 0; y < oh; ++y) {
            const uint8_t* src = pixels.data() + (size_t)(y * fh / oh) * fw * 4;
            uint32_t* row = dst + (size_t)(oy + y) * dw + ox;
            for (int x = 0; x < ow; ++x) {
                const uint8_t* p = src + (size_t)(x * fw / ow) * 4;
                row[x] = 0xff000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];  // BGRA -> RGBA
            }
        }
    }

    void layout() {
        float x = sc(kPad);
        for (auto& b : buttons) {
            if (!b.text_ready) b.text_ready = render_text(b.label, sc(kTextSize), b.text);
            b.w = std::max(sc(72), b.text.w + sc(2 * kPad));
            b.x = x;
            x += b.w + sc(kPad);
        }
    }

    void redraw() {
        if (!win) return;
        ANativeWindow_Buffer buf;
        if (ANativeWindow_lock(win, &buf, nullptr) != 0) return;
        uint32_t* dst = (uint32_t*)buf.bits;
        const int dw = buf.stride, dh = buf.height;
        const int bar_top = (int)(buf.height - sc(kBarH + kStatusH));
        const int status_top = (int)(buf.height - sc(kStatusH));
        blit_frame(dst, dw, dh, 0, std::max(0, bar_top));
        for (int y = std::max(0, bar_top); y < dh; ++y)
            std::fill(dst + (size_t)y * dw, dst + (size_t)y * dw + buf.width, 0xff202020u);
        layout();
        const int btn_top = bar_top + (int)(sc(kBarH - kBtnH) / 2);
        for (auto& b : buttons) {
            const int x0 = (int)b.x, x1 = (int)(b.x + b.w), y1 = btn_top + (int)sc(kBtnH);
            const uint32_t face = !b.enabled ? 0xff303030u : b.down ? 0xff5a5a5au : 0xff444444u;
            for (int y = btn_top; y < y1 && y < dh; ++y)
                for (int x = x0; x < x1 && x < buf.width; ++x) dst[(size_t)y * dw + x] = face;
            const uint32_t ink = b.enabled ? 0xffffff : 0x808080;
            draw_text_at(dst, dw, dh, b.text, x0 + (int)((b.w - b.text.w) / 2), btn_top + (int)(sc(kBtnH) - b.text.h) / 2, ink);
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            if (status_dirty) {
                status_text = TextImage();
                render_text(status, sc(kStatusSize), status_text);
                status_dirty = false;
            }
            draw_text_at(dst, dw, dh, status_text, (int)sc(kPad), status_top, 0xdddddd);
        }
        ANativeWindow_unlockAndPost(win);
    }

    void on_touch(AInputEvent* ev) {
        const int32_t action = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
        const float x = AMotionEvent_getX(ev, 0), y = AMotionEvent_getY(ev, 0);
        const int bar_top = (int)(h - sc(kBarH + kStatusH));
        const int btn_top = bar_top + (int)(sc(kBarH - kBtnH) / 2);
        const bool in_bar = y >= btn_top && y <= btn_top + sc(kBtnH);
        for (auto& b : buttons) {
            const bool hit = in_bar && b.enabled && x >= b.x && x <= b.x + b.w;
            if (action == AMOTION_EVENT_ACTION_DOWN) b.down = hit;
            else if (action == AMOTION_EVENT_ACTION_UP) {
                const bool fire = b.down && hit;
                b.down = false;
                if (fire && b.click) b.click();
            } else if (action == AMOTION_EVENT_ACTION_CANCEL) {
                b.down = false;
            }
        }
        redraw();
    }

    void run_posted() {
        for (;;) {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lk(mu);
                if (posted.empty()) return;
                fn = std::move(posted.front());
                posted.pop_front();
            }
            if (fn) fn();
        }
    }

    // Pump the looper once; returns false when the activity is going away.
    bool pump(int timeout_ms) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int id = ALooper_pollOnce(timeout_ms, nullptr, &events, (void**)&source);
        if (id >= 0 && source) source->process(g_app, source);
        if (g_app->destroyRequested) return false;
        return true;
    }
};

namespace {

Window::Impl* g_impl = nullptr;

void on_app_cmd(android_app* app, int32_t cmd) {
    Window::Impl* s = g_impl;
    if (!s) return;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            s->win = app->window;
            if (s->win) {
                ANativeWindow_setBuffersGeometry(s->win, 0, 0, WINDOW_FORMAT_RGBA_8888);
                s->w = ANativeWindow_getWidth(s->win);
                s->h = ANativeWindow_getHeight(s->win);
                s->open = true;
                s->redraw();
            }
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            if (s->win) {
                s->w = ANativeWindow_getWidth(s->win);
                s->h = ANativeWindow_getHeight(s->win);
                s->redraw();
            }
            break;
        case APP_CMD_TERM_WINDOW:
            s->win = nullptr;
            break;
        case APP_CMD_DESTROY:
            s->want_close = true;
            break;
        default: break;
    }
}

int32_t on_input(android_app*, AInputEvent* ev) {
    Window::Impl* s = g_impl;
    if (!s) return 0;
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
        s->on_touch(ev);
        return 1;
    }
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY && AKeyEvent_getKeyCode(ev) == AKEYCODE_BACK &&
        AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_UP) {
        s->want_close = true;
        return 1;
    }
    return 0;
}

std::string external_dir() {
    if (g_app && g_app->activity->externalDataPath) return g_app->activity->externalDataPath;
    if (g_app && g_app->activity->internalDataPath) return g_app->activity->internalDataPath;
    return "/data/local/tmp";
}

// CAMERA and RECORD_AUDIO are runtime permissions. A NativeActivity is still an Activity, so
// they can be asked for through JNI; without this the camera opens and never delivers anything.
void request_permissions() {
    Jni& j = jni();
    if (!j.ok || !g_app) return;
    JNIEnv* e = j.env;
    jobject act = g_app->activity->clazz;
    jclass cls = e->GetObjectClass(act);
    jmethodID check = e->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    jmethodID request = e->GetMethodID(cls, "requestPermissions", "([Ljava/lang/String;I)V");
    if (!check || !request) {
        e->ExceptionClear();
        return;
    }
    const char* names[] = {"android.permission.CAMERA", "android.permission.RECORD_AUDIO"};
    auto granted = [&](const char* n) {
        jstring s = e->NewStringUTF(n);
        const bool ok = e->CallIntMethod(act, check, s) == 0;  // PERMISSION_GRANTED
        e->DeleteLocalRef(s);
        return ok;
    };
    std::vector<const char*> missing;
    for (const char* n : names)
        if (!granted(n)) missing.push_back(n);
    if (missing.empty()) return;
    jclass str = e->FindClass("java/lang/String");
    jobjectArray arr = e->NewObjectArray((jsize)missing.size(), str, nullptr);
    for (size_t i = 0; i < missing.size(); ++i) {
        jstring s = e->NewStringUTF(missing[i]);
        e->SetObjectArrayElement(arr, (jsize)i, s);
        e->DeleteLocalRef(s);
    }
    e->CallVoidMethod(act, request, arr, 0);
    e->DeleteLocalRef(arr);
    // the dialog is modal for the user, not for us: wait for an answer, but not forever
    for (int i = 0; i < 600; ++i) {
        bool all = true;
        for (const char* n : missing) all = all && granted(n);
        if (all) break;
        usleep(100000);
    }
}

}  // namespace

Window::Window() : p_(new Impl) {}
Window::~Window() { close(); }

bool Window::create(const std::string& title, int width, int height) {
    (void)title, (void)width, (void)height;  // the activity owns the window and fills the screen
    if (!g_app) return false;
    Impl& s = *p_;
    g_impl = &s;
    g_app->onAppCmd = on_app_cmd;
    g_app->onInputEvent = on_input;
    if (g_app->config) {
        const int32_t density = AConfiguration_getDensity(g_app->config);
        if (density > 0 && density < 2000) g_scale = density / 160.0f;
    }
    for (int i = 0; i < 2000 && !s.open; ++i)
        if (!s.pump(10)) return false;
    return s.open;
}

int Window::add_button(const std::string& label, std::function<void()> on_click) {
    Impl& s = *p_;
    Impl::Button b;
    b.label = label;
    b.click = std::move(on_click);
    s.buttons.push_back(std::move(b));
    s.redraw();
    return (int)s.buttons.size() - 1;
}

void Window::set_button_text(int id, const std::string& label) {
    Impl& s = *p_;
    if (id < 0 || id >= (int)s.buttons.size()) return;
    s.buttons[id].label = label;
    s.buttons[id].text_ready = false;
    s.buttons[id].text = TextImage();
    s.redraw();
}

void Window::enable_button(int id, bool enable) {
    Impl& s = *p_;
    if (id < 0 || id >= (int)s.buttons.size()) return;
    s.buttons[id].enabled = enable;
    s.redraw();
}

void Window::set_status(const std::string& text) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.status == text) return;
    s.status = text;
    s.status_dirty = true;
}

void Window::show_frame(const VideoFrame& bgra) {
    if (bgra.format != PixelFormat::BGRA || bgra.empty()) return;
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    s.fw = bgra.width, s.fh = bgra.height;
    s.pixels.resize((size_t)bgra.width * 4 * bgra.height);
    for (int y = 0; y < bgra.height; ++y)
        std::memcpy(s.pixels.data() + (size_t)y * bgra.width * 4, bgra.data[0] + (size_t)y * bgra.stride[0], (size_t)bgra.width * 4);
    s.frame_dirty = true;
}

void Window::clear_frame() {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    s.pixels.clear();
    s.fw = s.fh = 0;
    s.frame_dirty = true;
}

void Window::set_title(const std::string&) {}  // the activity's label is what shows

std::string Window::choose_open_file(const std::string&, const std::string& pattern) {
    // No file picker in the NDK: take the newest matching file out of the app's own folder.
    std::vector<std::string> exts;
    for (size_t i = 0, j; i < pattern.size(); i = j + 1) {
        j = std::min(pattern.find(';', i), pattern.size());
        const std::string e = pattern.substr(i, j - i);
        const size_t dot = e.rfind('.');
        if (dot != std::string::npos) exts.push_back(e.substr(dot));
    }
    const std::string dir = external_dir();
    DIR* d = opendir(dir.c_str());
    if (!d) return {};
    std::string best;
    time_t best_t = 0;
    while (dirent* de = readdir(d)) {
        const std::string name = de->d_name;
        bool match = exts.empty();
        for (auto& e : exts)
            if (name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0) match = true;
        if (!match) continue;
        const std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_mtime >= best_t) best_t = st.st_mtime, best = full;
    }
    closedir(d);
    return best;
}

std::string Window::choose_save_file(const std::string& default_name) { return external_dir() + "/" + default_name; }

void Window::post(std::function<void()> fn) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    s.posted.push_back(std::move(fn));
}

void Window::on_close(std::function<void()> fn) { p_->closing = std::move(fn); }

void Window::set_timer(int interval_ms, std::function<void()> fn) {
    Impl& s = *p_;
    s.timers.clear();
    s.timers.push_back({interval_ms, now_us() + (int64_t)interval_ms * 1000, std::move(fn)});
}

int Window::run() {
    Impl& s = *p_;
    if (!s.open) return 1;
    while (!s.want_close) {
        int64_t wait_ms = 16;
        const int64_t t = now_us();
        for (auto& tm : s.timers) wait_ms = std::min(wait_ms, std::max<int64_t>(0, (tm.next - t) / 1000));
        if (!s.pump((int)wait_ms)) break;
        s.run_posted();
        const int64_t now = now_us();
        for (auto& tm : s.timers)
            if (now >= tm.next) {
                tm.next = now + (int64_t)tm.interval_ms * 1000;
                if (tm.fn) tm.fn();
            }
        bool dirty;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            dirty = s.frame_dirty || s.status_dirty;
            s.frame_dirty = false;
        }
        if (dirty) s.redraw();
    }
    if (s.closing) s.closing();
    s.open = false;
    ANativeActivity_finish(g_app->activity);
    for (int i = 0; i < 200 && s.pump(10); ++i) {}
    return 0;
}

void Window::close() {
    Impl& s = *p_;
    if (!s.open) return;
    s.want_close = true;
}

}  // namespace mk

// ---------------------------------------------------------------- NativeActivity entry point

extern "C" void android_main(struct android_app* app) {
    mk::g_app = app;
    mk::set_java_vm(app->activity->vm);  // so the codec layer can reach MediaCodec through JNI
    mk::request_permissions();
    mk::logi("mediakit: starting, external dir %s", app->activity->externalDataPath ? app->activity->externalDataPath : "-");
    char arg0[] = "mediakit";
    char* argv[] = {arg0, nullptr};
    const int rc = main(1, argv);
    mk::logi("mediakit: main returned %d", rc);
    ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        if (ALooper_pollOnce(100, nullptr, &events, (void**)&source) >= 0 && source) source->process(app, source);
    }
}
