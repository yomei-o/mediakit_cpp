// mediakit ui layer: a window that shows video and has a row of buttons and a status line.
// Per OS under ui/<os>/ (Windows: Win32 + GDI). Frames are BGRA (convert with mk::FrameConverter).
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "mk_types.h"

namespace mk {

class Window {
public:
    Window();
    ~Window();
    bool create(const std::string& title, int width, int height);
    int add_button(const std::string& label, std::function<void()> on_click);  // returns an id
    void set_button_text(int id, const std::string& label);
    void enable_button(int id, bool enable);
    void set_status(const std::string& text);    // any thread
    void show_frame(const VideoFrame& bgra);      // any thread; kept until the next one
    void clear_frame();                           // any thread
    void set_title(const std::string& title);
    std::string choose_open_file(const std::string& filter_name, const std::string& pattern);  // "MP4", "*.mp4;*.mov"
    std::string choose_save_file(const std::string& default_name);
    void post(std::function<void()> fn);          // run on the UI thread
    void on_close(std::function<void()> fn);
    void set_timer(int interval_ms, std::function<void()> fn);  // UI-thread ticks
    int run();                                    // message loop until the window closes
    void close();
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

}  // namespace mk
