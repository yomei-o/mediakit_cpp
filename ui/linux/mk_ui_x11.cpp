// mediakit ui for Linux: plain Xlib (no toolkit). Buttons are drawn by hand; text uses a UTF-8 font set.
// File dialogs use zenity / kdialog when installed, else they ask on the terminal.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "mk_ui.h"

namespace mk {

namespace {

const int kBarH = 40, kStatusH = 26, kBtnW = 110, kBtnH = 30, kPad = 5;

std::string run_dialog(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    char buf[4096];
    std::string out;
    while (fgets(buf, sizeof(buf), p)) out += buf;
    int rc = pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return rc == 0 ? out : std::string();
}

bool have(const char* tool) { return system((std::string("command -v ") + tool + " >/dev/null 2>&1").c_str()) == 0; }

std::string ask_terminal(const std::string& prompt, const std::string& def) {
    std::printf("%s [%s]: ", prompt.c_str(), def.c_str());
    std::fflush(stdout);
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return {};
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s.empty() ? def : s;
}

}  // namespace

struct Window::Impl {
    Display* dpy = nullptr;
    ::Window win = 0;
    GC gc = nullptr;
    XFontSet fonts = nullptr;
    Atom wm_delete = 0;
    Visual* visual = nullptr;
    int depth = 24;
    int w = 0, h = 0;
    int wake[2] = {-1, -1};  // self-pipe: other threads poke the loop
    bool open = false;

    struct Button {
        std::string label;
        std::function<void()> click;
        bool enabled = true;
        bool down = false;
    };
    std::vector<Button> buttons;
    std::function<void()> closing;
    struct Timer {
        int interval_ms;
        int64_t next;
        std::function<void()> fn;
    };
    std::vector<Timer> timers;

    std::mutex mu;  // shared with other threads
    std::vector<uint8_t> pixels;  // BGRA top-down
    int fw = 0, fh = 0;
    bool frame_dirty = false;
    std::string status;
    bool status_dirty = false;
    std::deque<std::function<void()>> posted;

    std::vector<uint8_t> scaled;  // window-sized copy for XPutImage

    void poke() {
        if (wake[1] >= 0) {
            char c = 1;
            (void)!::write(wake[1], &c, 1);
        }
    }

    int text_width(const std::string& s) {
        if (!fonts) return (int)s.size() * 7;
        XRectangle ink, logical;
        Xutf8TextExtents(fonts, s.c_str(), (int)s.size(), &ink, &logical);
        return logical.width;
    }
    void draw_text(int x, int y, const std::string& s, unsigned long color) {
        XSetForeground(dpy, gc, color);
        if (fonts) Xutf8DrawString(dpy, win, fonts, gc, x, y, s.c_str(), (int)s.size());
        else XDrawString(dpy, win, gc, x, y, s.c_str(), (int)s.size());
    }

    void draw_bar() {
        XSetForeground(dpy, gc, 0xe0e0e0);
        XFillRectangle(dpy, win, gc, 0, 0, w, kBarH);
        XFillRectangle(dpy, win, gc, 0, h - kStatusH, w, kStatusH);
        for (size_t i = 0; i < buttons.size(); ++i) {
            const int x = kPad + (int)i * (kBtnW + kPad), y = (kBarH - kBtnH) / 2;
            XSetForeground(dpy, gc, buttons[i].down ? 0xb0b0b0 : 0xf8f8f8);
            XFillRectangle(dpy, win, gc, x, y, kBtnW, kBtnH);
            XSetForeground(dpy, gc, 0x707070);
            XDrawRectangle(dpy, win, gc, x, y, kBtnW - 1, kBtnH - 1);
            const std::string& l = buttons[i].label;
            draw_text(x + std::max(4, (kBtnW - text_width(l)) / 2), y + kBtnH / 2 + 5, l, buttons[i].enabled ? 0x000000 : 0x909090);
        }
        std::string st;
        {
            std::lock_guard<std::mutex> lk(mu);
            st = status;
            status_dirty = false;
        }
        draw_text(kPad, h - 8, st, 0x000000);
    }

    void draw_video() {
        const int vy = kBarH, vw = w, vh = h - kBarH - kStatusH;
        if (vw <= 0 || vh <= 0) return;
        std::lock_guard<std::mutex> lk(mu);
        frame_dirty = false;
        scaled.assign((size_t)vw * vh * 4, 0);
        if (fw > 0 && fh > 0) {
            int dw = vw, dh = (int)((long long)vw * fh / fw);
            if (dh > vh) dh = vh, dw = (int)((long long)vh * fw / fh);
            const int ox = (vw - dw) / 2, oy = (vh - dh) / 2;
            for (int y = 0; y < dh; ++y) {  // nearest neighbour
                const uint32_t* src = (const uint32_t*)(pixels.data() + (size_t)((long long)y * fh / dh) * fw * 4);
                uint32_t* dst = (uint32_t*)(scaled.data() + ((size_t)(y + oy) * vw + ox) * 4);
                for (int x = 0; x < dw; ++x) dst[x] = src[(long long)x * fw / dw];
            }
        }
        XImage* img = XCreateImage(dpy, visual, depth, ZPixmap, 0, (char*)scaled.data(), vw, vh, 32, vw * 4);
        if (img) {
            XPutImage(dpy, win, gc, img, 0, 0, 0, vy, vw, vh);
            img->data = nullptr;  // ours
            XDestroyImage(img);
        }
    }

    int hit(int x, int y) const {
        if (y < (kBarH - kBtnH) / 2 || y > (kBarH + kBtnH) / 2) return -1;
        for (size_t i = 0; i < buttons.size(); ++i) {
            const int bx = kPad + (int)i * (kBtnW + kPad);
            if (x >= bx && x < bx + kBtnW) return (int)i;
        }
        return -1;
    }
};

Window::Window() : p_(new Impl) {}
Window::~Window() {
    Impl& s = *p_;
    if (s.dpy) {
        if (s.fonts) XFreeFontSet(s.dpy, s.fonts);
        if (s.gc) XFreeGC(s.dpy, s.gc);
        if (s.open) XDestroyWindow(s.dpy, s.win);
        XCloseDisplay(s.dpy);
    }
    if (s.wake[0] >= 0) ::close(s.wake[0]), ::close(s.wake[1]);
}

bool Window::create(const std::string& title, int width, int height) {
    Impl& s = *p_;
    XInitThreads();
    setlocale(LC_CTYPE, "");
    s.dpy = XOpenDisplay(nullptr);
    if (!s.dpy) {
        std::fprintf(stderr, "mediakit: cannot open X display (DISPLAY=%s)\n", getenv("DISPLAY") ? getenv("DISPLAY") : "");
        return false;
    }
    const int scr = DefaultScreen(s.dpy);
    s.visual = DefaultVisual(s.dpy, scr);
    s.depth = DefaultDepth(s.dpy, scr);
    s.w = width, s.h = height + kBarH + kStatusH;
    s.win = XCreateSimpleWindow(s.dpy, RootWindow(s.dpy, scr), 0, 0, s.w, s.h, 0, 0, 0x000000);
    Xutf8SetWMProperties(s.dpy, s.win, title.c_str(), title.c_str(), nullptr, 0, nullptr, nullptr, nullptr);
    XSelectInput(s.dpy, s.win, ExposureMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask);
    s.wm_delete = XInternAtom(s.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s.dpy, s.win, &s.wm_delete, 1);
    s.gc = XCreateGC(s.dpy, s.win, 0, nullptr);
    char** missing = nullptr;
    int nmissing = 0;
    char* def = nullptr;
    s.fonts = XCreateFontSet(s.dpy, "-*-*-medium-r-normal--14-*-*-*-*-*-*-*,*", &missing, &nmissing, &def);
    if (missing) XFreeStringList(missing);
    if (pipe(s.wake) == 0) fcntl(s.wake[0], F_SETFL, O_NONBLOCK);
    XMapWindow(s.dpy, s.win);
    XFlush(s.dpy);
    s.open = true;
    return true;
}

int Window::add_button(const std::string& label, std::function<void()> on_click) {
    p_->buttons.push_back({label, std::move(on_click)});
    p_->poke();
    return (int)p_->buttons.size() - 1;
}

void Window::set_button_text(int id, const std::string& label) {
    if (id >= 0 && id < (int)p_->buttons.size()) p_->buttons[id].label = label;
    p_->poke();
}

void Window::enable_button(int id, bool enable) {
    if (id >= 0 && id < (int)p_->buttons.size()) p_->buttons[id].enabled = enable;
    p_->poke();
}

void Window::set_status(const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->status = text;
        p_->status_dirty = true;
    }
    p_->poke();
}

void Window::show_frame(const VideoFrame& f) {
    if (f.format != PixelFormat::BGRA || f.empty()) return;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->fw = f.width, p_->fh = f.height;
        p_->pixels.resize((size_t)f.width * 4 * f.height);
        for (int y = 0; y < f.height; ++y)
            memcpy(p_->pixels.data() + (size_t)y * f.width * 4, f.data[0] + (size_t)y * f.stride[0], (size_t)f.width * 4);
        p_->frame_dirty = true;
    }
    p_->poke();
}

void Window::clear_frame() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->fw = p_->fh = 0;
        p_->pixels.clear();
        p_->frame_dirty = true;
    }
    p_->poke();
}

void Window::set_title(const std::string& title) {
    if (p_->dpy) Xutf8SetWMProperties(p_->dpy, p_->win, title.c_str(), title.c_str(), nullptr, 0, nullptr, nullptr, nullptr);
}

std::string Window::choose_open_file(const std::string& filter_name, const std::string& pattern) {
    std::string pats = pattern;
    std::replace(pats.begin(), pats.end(), ';', ' ');
    if (have("zenity")) return run_dialog("zenity --file-selection --file-filter='" + filter_name + " | " + pats + "' 2>/dev/null");
    if (have("kdialog")) return run_dialog("kdialog --getopenfilename . '" + pats + "' 2>/dev/null");
    return ask_terminal("open file", "");
}

std::string Window::choose_save_file(const std::string& default_name) {
    if (have("zenity")) return run_dialog("zenity --file-selection --save --confirm-overwrite --filename='" + default_name + "' 2>/dev/null");
    if (have("kdialog")) return run_dialog("kdialog --getsavefilename '" + default_name + "' 2>/dev/null");
    return ask_terminal("save as", default_name);
}

void Window::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->posted.push_back(std::move(fn));
    }
    p_->poke();
}

void Window::on_close(std::function<void()> fn) { p_->closing = std::move(fn); }

void Window::set_timer(int interval_ms, std::function<void()> fn) {
    p_->timers.push_back({interval_ms, now_us() + interval_ms * 1000LL, std::move(fn)});
}

int Window::run() {
    Impl& s = *p_;
    if (!s.dpy) return 1;
    bool redraw_bar = true;
    while (s.open) {
        // events
        while (XPending(s.dpy)) {
            XEvent e;
            XNextEvent(s.dpy, &e);
            switch (e.type) {
                case Expose:
                    if (e.xexpose.count == 0) redraw_bar = true, s.frame_dirty = true;
                    break;
                case ConfigureNotify:
                    s.w = e.xconfigure.width, s.h = e.xconfigure.height;
                    redraw_bar = true, s.frame_dirty = true;
                    break;
                case ButtonPress: {
                    int b = s.hit(e.xbutton.x, e.xbutton.y);
                    if (b >= 0 && s.buttons[b].enabled) s.buttons[b].down = true, redraw_bar = true;
                    break;
                }
                case ButtonRelease: {
                    int b = s.hit(e.xbutton.x, e.xbutton.y);
                    bool fire = b >= 0 && s.buttons[b].down && s.buttons[b].enabled;
                    for (auto& x : s.buttons) x.down = false;
                    redraw_bar = true;
                    if (fire && s.buttons[b].click) s.buttons[b].click();
                    break;
                }
                case ClientMessage:
                    if ((Atom)e.xclient.data.l[0] == s.wm_delete) {
                        if (s.closing) s.closing();
                        s.open = false;
                    }
                    break;
            }
        }
        if (!s.open) break;
        // posted work, timers
        std::deque<std::function<void()>> work;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            work.swap(s.posted);
            if (s.status_dirty) redraw_bar = true;
        }
        for (auto& fn : work) fn();
        const int64_t now = now_us();
        for (auto& t : s.timers)
            if (now >= t.next) {
                t.next = now + t.interval_ms * 1000LL;
                t.fn();
                redraw_bar = true;
            }
        // draw
        bool frame;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            frame = s.frame_dirty;
        }
        if (frame) s.draw_video();
        if (redraw_bar) s.draw_bar(), redraw_bar = false;
        XFlush(s.dpy);
        // sleep until X, a poke, or the next timer
        int64_t wait_ms = 100;
        for (auto& t : s.timers) wait_ms = std::min<int64_t>(wait_ms, std::max<int64_t>(0, (t.next - now_us()) / 1000));
        pollfd pf[2] = {{ConnectionNumber(s.dpy), POLLIN, 0}, {s.wake[0], POLLIN, 0}};
        poll(pf, s.wake[0] >= 0 ? 2 : 1, (int)wait_ms);
        if (pf[1].revents & POLLIN) {
            char buf[64];
            while (::read(s.wake[0], buf, sizeof(buf)) > 0) {
            }
            redraw_bar = true;
        }
    }
    XDestroyWindow(s.dpy, s.win);
    s.open = false;
    XFlush(s.dpy);
    return 0;
}

void Window::close() {
    if (p_->closing) p_->closing();
    p_->open = false;
    p_->poke();
}

}  // namespace mk
