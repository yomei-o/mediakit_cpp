// mediakit ui for Windows: Win32 window, GDI StretchDIBits for video.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first
#include <commdlg.h>

#include <mutex>
#include <vector>

#include "mk_ui.h"

namespace mk {

namespace {

std::wstring to_wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string to_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

const UINT WM_MK_POST = WM_APP + 1;
const UINT WM_MK_STATUS = WM_APP + 2;
const int kBarH = 40, kStatusH = 26, kBtnW = 110, kBtnH = 30, kPad = 5;
const wchar_t* kClass = L"mediakit_window";

}  // namespace

struct Window::Impl {
    HWND hwnd = nullptr;
    HWND status = nullptr;
    HFONT font = nullptr;
    std::vector<HWND> buttons;
    std::vector<std::function<void()>> clicks;
    std::function<void()> closing;
    std::vector<std::function<void()>> timers;

    std::mutex mu;
    std::vector<uint8_t> pixels;  // BGRA, top-down
    int fw = 0, fh = 0;
    std::string status_text;
    bool status_dirty = false;

    RECT video_rect() const {
        RECT r;
        GetClientRect(hwnd, &r);
        r.top += kBarH;
        r.bottom -= kStatusH;
        if (r.bottom < r.top) r.bottom = r.top;
        return r;
    }

    void layout() {
        RECT r;
        GetClientRect(hwnd, &r);
        if (status) MoveWindow(status, kPad, r.bottom - kStatusH + 4, r.right - 2 * kPad, kStatusH - 6, TRUE);
        RECT v = video_rect();
        InvalidateRect(hwnd, &v, FALSE);
    }

    void paint(HDC dc) {
        RECT v = video_rect();
        const int vw = v.right - v.left, vh = v.bottom - v.top;
        if (vw <= 0 || vh <= 0) return;
        // back buffer: no flicker
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, vw, vh);
        HGDIOBJ old = SelectObject(mem, bmp);
        RECT all = {0, 0, vw, vh};
        FillRect(mem, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
        {
            std::lock_guard<std::mutex> lk(mu);
            if (fw > 0 && fh > 0) {
                // fit, keep aspect
                int dw = vw, dh = (int)((long long)vw * fh / fw);
                if (dh > vh) dh = vh, dw = (int)((long long)vh * fw / fh);
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = fw;
                bi.bmiHeader.biHeight = -fh;  // top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(mem, HALFTONE);
                SetBrushOrgEx(mem, 0, 0, nullptr);
                StretchDIBits(mem, (vw - dw) / 2, (vh - dh) / 2, dw, dh, 0, 0, fw, fh, pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            }
        }
        BitBlt(dc, v.left, v.top, vw, vh, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Impl* s = (Impl*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_NCCREATE) {
            s = (Impl*)((CREATESTRUCTW*)l)->lpCreateParams;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);
            s->hwnd = h;
        }
        if (!s) return DefWindowProcW(h, m, w, l);
        switch (m) {
            case WM_SIZE: s->layout(); return 0;
            case WM_ERASEBKGND: {
                // erase only the bar and status area; the video area is painted fully
                HDC dc = (HDC)w;
                RECT r;
                GetClientRect(h, &r);
                RECT top = {0, 0, r.right, kBarH}, bot = {0, r.bottom - kStatusH, r.right, r.bottom};
                FillRect(dc, &top, GetSysColorBrush(COLOR_BTNFACE));
                FillRect(dc, &bot, GetSysColorBrush(COLOR_BTNFACE));
                return 1;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                s->paint(dc);
                EndPaint(h, &ps);
                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(w);
                if (id >= 100 && id - 100 < (int)s->clicks.size() && HIWORD(w) == BN_CLICKED) s->clicks[id - 100]();
                return 0;
            }
            case WM_TIMER:
                if (w >= 1 && w <= s->timers.size()) s->timers[w - 1]();
                return 0;
            case WM_MK_POST: {
                auto* fn = (std::function<void()>*)l;
                (*fn)();
                delete fn;
                return 0;
            }
            case WM_MK_STATUS: {
                std::string t;
                {
                    std::lock_guard<std::mutex> lk(s->mu);
                    t = s->status_text;
                    s->status_dirty = false;
                }
                SetWindowTextW(s->status, to_wide(t).c_str());
                return 0;
            }
            case WM_CTLCOLORSTATIC: {
                HDC dc = (HDC)w;
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            case WM_CLOSE:
                if (s->closing) s->closing();
                DestroyWindow(h);
                return 0;
            case WM_DESTROY:
                s->hwnd = nullptr;
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

Window::Window() : p_(new Impl) {}
Window::~Window() {
    if (p_->hwnd) DestroyWindow(p_->hwnd);
    if (p_->font) DeleteObject(p_->font);
}

bool Window::create(const std::string& title, int width, int height) {
    SetProcessDPIAware();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Impl::proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);  // fails harmlessly when already registered
    RECT r = {0, 0, width, height + kBarH + kStatusH};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowExW(0, kClass, to_wide(title).c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, p_.get());
    if (!h) return false;
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    p_->font = CreateFontIndirectW(&ncm.lfMessageFont);
    p_->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE, 0, 0, 10, 10, h,
                                 nullptr, inst, nullptr);
    SendMessageW(p_->status, WM_SETFONT, (WPARAM)p_->font, TRUE);
    ShowWindow(h, SW_SHOW);
    p_->layout();
    return true;
}

int Window::add_button(const std::string& label, std::function<void()> on_click) {
    const int idx = (int)p_->buttons.size();
    HWND b = CreateWindowExW(0, L"BUTTON", to_wide(label).c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, kPad + idx * (kBtnW + kPad),
                             (kBarH - kBtnH) / 2, kBtnW, kBtnH, p_->hwnd, (HMENU)(INT_PTR)(100 + idx), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(b, WM_SETFONT, (WPARAM)p_->font, TRUE);
    p_->buttons.push_back(b);
    p_->clicks.push_back(std::move(on_click));
    return idx;
}

void Window::set_button_text(int id, const std::string& label) {
    if (id >= 0 && id < (int)p_->buttons.size()) SetWindowTextW(p_->buttons[id], to_wide(label).c_str());
}

void Window::enable_button(int id, bool enable) {
    if (id >= 0 && id < (int)p_->buttons.size()) EnableWindow(p_->buttons[id], enable);
}

void Window::set_status(const std::string& text) {
    bool post;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->status_text = text;
        post = !p_->status_dirty;
        p_->status_dirty = true;
    }
    if (post && p_->hwnd) PostMessageW(p_->hwnd, WM_MK_STATUS, 0, 0);
}

void Window::show_frame(const VideoFrame& f) {
    if (f.format != PixelFormat::BGRA || f.empty()) return;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->fw = f.width, p_->fh = f.height;
        p_->pixels.resize((size_t)f.width * 4 * f.height);
        for (int y = 0; y < f.height; ++y)
            memcpy(p_->pixels.data() + (size_t)y * f.width * 4, f.data[0] + (size_t)y * f.stride[0], (size_t)f.width * 4);
    }
    if (p_->hwnd) {
        RECT v = p_->video_rect();
        InvalidateRect(p_->hwnd, &v, FALSE);
    }
}

void Window::clear_frame() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->fw = p_->fh = 0;
        p_->pixels.clear();
    }
    if (p_->hwnd) {
        RECT v = p_->video_rect();
        InvalidateRect(p_->hwnd, &v, FALSE);
    }
}

void Window::set_title(const std::string& title) {
    if (p_->hwnd) SetWindowTextW(p_->hwnd, to_wide(title).c_str());
}

std::string Window::choose_open_file(const std::string& filter_name, const std::string& pattern) {
    std::wstring filter = to_wide(filter_name + " (" + pattern + ")");
    filter.push_back(L'\0');
    filter += to_wide(pattern);
    filter.push_back(L'\0');
    filter += L"All files (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = p_->hwnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? to_utf8(buf) : std::string();
}

std::string Window::choose_save_file(const std::string& default_name) {
    wchar_t buf[MAX_PATH] = L"";
    wcsncpy_s(buf, to_wide(default_name).c_str(), _TRUNCATE);
    const wchar_t filter[] = L"MP4 (*.mp4)\0*.mp4\0All files (*.*)\0*.*\0";
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = p_->hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"mp4";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn) ? to_utf8(buf) : std::string();
}

void Window::post(std::function<void()> fn) {
    if (!p_->hwnd) return;
    auto* heap = new std::function<void()>(std::move(fn));
    if (!PostMessageW(p_->hwnd, WM_MK_POST, 0, (LPARAM)heap)) delete heap;
}

void Window::on_close(std::function<void()> fn) { p_->closing = std::move(fn); }

void Window::set_timer(int interval_ms, std::function<void()> fn) {
    p_->timers.push_back(std::move(fn));
    SetTimer(p_->hwnd, p_->timers.size(), interval_ms, nullptr);
}

int Window::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

void Window::close() {
    if (p_->hwnd) PostMessageW(p_->hwnd, WM_CLOSE, 0, 0);
}

}  // namespace mk
