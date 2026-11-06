// mediakit ui for the browser (Emscripten): a <canvas> with HTML buttons and a status line.
// The page goes into the element with id "mk-root" (tools/wasm_shell.html has one), else into <body>.
// run() loops with emscripten_sleep (ASYNCIFY), so programs look the same as on the other OSes:
// clicks, posted work and timers run inside run(), and camera frames arrive while it sleeps.
// File dialogs use the browser's file picker and the in-memory file system (MEMFS) under /tmp.
#include <emscripten.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <vector>

#include "mk_ui.h"

EM_JS_DEPS(mk_ui_wasm, "$UTF8ToString,$stringToNewUTF8,$FS");

EM_JS(void, mk_js_ui_create, (int id, const char* title, int width, int height), {
    Module.mkUi = Module.mkUi || {};
    const ui = { events: [], buttons: [] };
    Module.mkUi[id] = ui;
    document.title = UTF8ToString(title);
    const root = document.getElementById('mk-root') || document.body;
    ui.root = document.createElement('div');
    ui.root.className = 'mk-window';
    ui.bar = document.createElement('div');
    ui.bar.className = 'mk-bar';
    ui.canvas = document.createElement('canvas');
    ui.canvas.className = 'mk-video';
    ui.canvas.width = width, ui.canvas.height = height;
    ui.canvas.style.maxWidth = width + 'px';
    ui.ctx = ui.canvas.getContext('2d');
    ui.ctx.fillStyle = '#000';
    ui.ctx.fillRect(0, 0, width, height);
    ui.status = document.createElement('div');
    ui.status.className = 'mk-status';
    ui.root.append(ui.bar, ui.canvas, ui.status);
    root.append(ui.root);
});

EM_JS(void, mk_js_ui_add_button, (int id, int index, const char* label), {
    const ui = Module.mkUi[id];
    const b = document.createElement('button');
    b.textContent = UTF8ToString(label);
    b.onclick = () => ui.events.push(index);
    ui.buttons[index] = b;
    ui.bar.append(b);
});

EM_JS(void, mk_js_ui_button, (int id, int index, const char* label, int enabled), {
    const b = Module.mkUi[id].buttons[index];
    if (!b) return;
    if (label) b.textContent = UTF8ToString(label);
    b.disabled = !enabled;
});

EM_JS(void, mk_js_ui_status, (int id, const char* text), { Module.mkUi[id].status.textContent = UTF8ToString(text); });

EM_JS(void, mk_js_ui_title, (const char* title), { document.title = UTF8ToString(title); });

EM_JS(int, mk_js_ui_next_event, (int id), {
    const ui = Module.mkUi[id];
    return ui.events.length ? ui.events.shift() : -1;
});

// rgba: width*height*4 bytes in wasm memory, copied out before returning.
EM_JS(void, mk_js_ui_draw, (int id, const uint8_t* rgba, int width, int height), {
    const ui = Module.mkUi[id];
    if (!rgba) {
        ui.ctx.fillStyle = '#000';
        ui.ctx.fillRect(0, 0, ui.canvas.width, ui.canvas.height);
        return;
    }
    if (ui.canvas.width != width || ui.canvas.height != height) ui.canvas.width = width, ui.canvas.height = height;
    const px = new Uint8ClampedArray(HEAPU8.buffer, rgba, width * height * 4).slice();
    ui.ctx.putImageData(new ImageData(px, width, height), 0, 0);
});

EM_JS(void, mk_js_ui_destroy, (int id), {
    const ui = Module.mkUi && Module.mkUi[id];
    if (!ui) return;
    ui.root.remove();
    delete Module.mkUi[id];
});

// Copies the picked file into MEMFS and returns its path ("" when cancelled).
EM_ASYNC_JS(char*, mk_js_ui_open_file, (const char* accept), {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = UTF8ToString(accept);
    const file = await new Promise(resolve => {
        input.onchange = () => resolve(input.files[0] || null);
        input.oncancel = () => resolve(null);
        input.click();
    });
    if (!file) return stringToNewUTF8("");
    const data = new Uint8Array(await file.arrayBuffer());
    try { FS.mkdir('/tmp'); } catch (e) {}
    const path = '/tmp/' + file.name.split('/').join('_');  // no backslashes: this is a C string
    FS.writeFile(path, data);
    return stringToNewUTF8(path);
});

namespace mk {

struct Window::Impl {
    int id = 0;
    bool created = false, open = false;
    struct Button {
        std::string label;
        std::function<void()> click;
        bool enabled = true;
    };
    std::vector<Button> buttons;
    std::function<void()> closing;
    struct Timer {
        int interval_ms;
        int64_t next;
        std::function<void()> fn;
    };
    std::vector<Timer> timers;
    std::deque<std::function<void()>> posted;
    std::vector<uint8_t> rgba;
};

Window::Window() : p_(new Impl) {
    static int next_id = 1;
    p_->id = next_id++;
}

Window::~Window() {
    if (p_->created) mk_js_ui_destroy(p_->id);
}

bool Window::create(const std::string& title, int width, int height) {
    mk_js_ui_create(p_->id, title.c_str(), width, height);
    p_->created = p_->open = true;
    return true;
}

int Window::add_button(const std::string& label, std::function<void()> on_click) {
    const int index = (int)p_->buttons.size();
    p_->buttons.push_back({label, std::move(on_click)});
    if (p_->created) mk_js_ui_add_button(p_->id, index, label.c_str());
    return index;
}

void Window::set_button_text(int id, const std::string& label) {
    if (id < 0 || id >= (int)p_->buttons.size()) return;
    p_->buttons[id].label = label;
    if (p_->created) mk_js_ui_button(p_->id, id, label.c_str(), p_->buttons[id].enabled);
}

void Window::enable_button(int id, bool enable) {
    if (id < 0 || id >= (int)p_->buttons.size()) return;
    p_->buttons[id].enabled = enable;
    if (p_->created) mk_js_ui_button(p_->id, id, nullptr, enable);
}

void Window::set_status(const std::string& text) {
    if (p_->created) mk_js_ui_status(p_->id, text.c_str());
}

void Window::show_frame(const VideoFrame& f) {
    if (f.format != PixelFormat::BGRA || f.empty() || !p_->created) return;
    std::vector<uint8_t>& px = p_->rgba;
    px.resize((size_t)f.width * 4 * f.height);
    for (int y = 0; y < f.height; ++y) {
        const uint8_t* s = f.data[0] + (size_t)y * f.stride[0];
        uint8_t* d = px.data() + (size_t)y * f.width * 4;
        for (int x = 0; x < f.width; ++x, s += 4, d += 4) d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
    }
    mk_js_ui_draw(p_->id, px.data(), f.width, f.height);
}

void Window::clear_frame() {
    if (p_->created) mk_js_ui_draw(p_->id, nullptr, 0, 0);
}

void Window::set_title(const std::string& title) { mk_js_ui_title(title.c_str()); }

std::string Window::choose_open_file(const std::string& filter_name, const std::string& pattern) {
    (void)filter_name;
    std::string accept;  // "*.mp4;*.mov" -> ".mp4,.mov"
    size_t start = 0;
    while (start <= pattern.size()) {
        size_t end = pattern.find(';', start);
        if (end == std::string::npos) end = pattern.size();
        std::string p = pattern.substr(start, end - start);
        if (p.rfind("*", 0) == 0) p.erase(0, 1);
        if (!p.empty() && p != ".*") accept += (accept.empty() ? "" : ",") + p;
        start = end + 1;
    }
    char* s = mk_js_ui_open_file(accept.c_str());
    std::string path = s ? s : "";
    std::free(s);
    return path;
}

// The browser cannot write to the user's disk without a download; the file goes to MEMFS.
std::string Window::choose_save_file(const std::string& default_name) { return "/tmp/" + default_name; }

void Window::post(std::function<void()> fn) { p_->posted.push_back(std::move(fn)); }

void Window::on_close(std::function<void()> fn) { p_->closing = std::move(fn); }

void Window::set_timer(int interval_ms, std::function<void()> fn) {
    p_->timers.push_back({interval_ms, now_us() + interval_ms * 1000LL, std::move(fn)});
}

int Window::run() {
    Impl& s = *p_;
    if (!s.created) return 1;
    while (s.open) {
        for (int b; (b = mk_js_ui_next_event(s.id)) >= 0;)
            if (b < (int)s.buttons.size() && s.buttons[b].enabled && s.buttons[b].click) s.buttons[b].click();
        while (!s.posted.empty()) {
            std::function<void()> fn = std::move(s.posted.front());
            s.posted.pop_front();
            fn();
        }
        const int64_t now = now_us();
        int64_t wait_ms = 20;
        for (auto& t : s.timers) {
            if (now >= t.next) {
                t.next = now + t.interval_ms * 1000LL;
                t.fn();
            }
            wait_ms = std::min<int64_t>(wait_ms, std::max<int64_t>(1, (t.next - now_us()) / 1000));
        }
        if (s.open) emscripten_sleep((unsigned)wait_ms);  // the browser runs: clicks, camera frames
    }
    return 0;
}

void Window::close() {
    if (p_->closing) p_->closing();
    p_->open = false;
}

}  // namespace mk
