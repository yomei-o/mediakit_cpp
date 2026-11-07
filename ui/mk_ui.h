// mediakit ui layer: a window that shows video and has a row of buttons and a status line.
// Per OS under ui/<os>/ (Windows: Win32 + GDI, Linux: Xlib, macOS: AppKit).
// Frames are BGRA (convert with mk::FrameConverter). create() and run() belong to the main thread.
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "mk_types.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
// UIKit owns main() and the main thread, so the ui layer provides main(): it starts UIKit and
// runs the program's main() on a thread of its own. Nothing else about the program changes.
extern "C" int mk_ios_main(int argc, char** argv);
#define main mk_ios_main
#endif
#endif

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

// Call after writing a file at a path from choose_save_file(). In the browser the file is only in memory
// (MEMFS): this hands it to the user as a download. Elsewhere it is already on disk: nothing to do.
#ifdef __EMSCRIPTEN__
void finish_saved_file(const std::string& path);
#else
inline void finish_saved_file(const std::string&) {}
#endif

}  // namespace mk
