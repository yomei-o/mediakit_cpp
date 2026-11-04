// mediakit ui for macOS: AppKit. A window with the video on top, a row of buttons and a status line.
// Works from a plain command line binary (no .app bundle): the process is promoted to a regular
// application in create().
#import <Cocoa/Cocoa.h>
#if __has_include(<UniformTypeIdentifiers/UniformTypeIdentifiers.h>)
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "mk_ui.h"

namespace mk {
namespace {

const CGFloat kBarH = 44, kStatusH = 24, kBtnH = 30, kPad = 6;

void on_main(std::function<void()> fn) {
    if ([NSThread isMainThread]) {
        fn();
        return;
    }
    auto held = std::make_shared<std::function<void()>>(std::move(fn));
    dispatch_async(dispatch_get_main_queue(), ^{
      (*held)();
    });
}

// close() tears down state the block would otherwise touch after the caller is gone.
void on_main_sync(std::function<void()> fn) {
    if ([NSThread isMainThread]) {
        fn();
        return;
    }
    __block std::function<void()> held = std::move(fn);
    dispatch_sync(dispatch_get_main_queue(), ^{
      held();
    });
}

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

}  // namespace
}  // namespace mk

// The view that shows the frames: BGRA, scaled to fit, centred on black.
@interface MkVideoView : NSView {
    std::vector<uint8_t> _pixels;
    int _w, _h;
    std::mutex _mu;
}
- (void)setFrameBGRA:(const mk::VideoFrame&)f;
- (void)clearFrame;
@end

@implementation MkVideoView

- (BOOL)isOpaque { return YES; }

- (void)setFrameBGRA:(const mk::VideoFrame&)f {
    {
        std::lock_guard<std::mutex> lk(_mu);
        _w = f.width, _h = f.height;
        _pixels.resize((size_t)f.width * 4 * f.height);
        for (int y = 0; y < f.height; ++y)
            std::memcpy(_pixels.data() + (size_t)y * f.width * 4, f.data[0] + (size_t)y * f.stride[0], (size_t)f.width * 4);
    }
    mk::on_main([self] { [self setNeedsDisplay:YES]; });
}

- (void)clearFrame {
    {
        std::lock_guard<std::mutex> lk(_mu);
        _pixels.clear();
        _w = _h = 0;
    }
    mk::on_main([self] { [self setNeedsDisplay:YES]; });
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [[NSColor blackColor] setFill];
    NSRectFill(self.bounds);
    std::lock_guard<std::mutex> lk(_mu);
    if (_pixels.empty() || _w <= 0 || _h <= 0) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, _pixels.data(), _pixels.size(), nullptr);
    CGImageRef img = CGImageCreate(_w, _h, 8, 32, (size_t)_w * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, dp,
                                   nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    if (img) {
        const NSRect b = self.bounds;
        const double scale = std::min(b.size.width / _w, b.size.height / _h);
        const double w = _w * scale, h = _h * scale;
        CGContextRef ctx = (CGContextRef)NSGraphicsContext.currentContext.CGContext;
        CGContextDrawImage(ctx, CGRectMake(b.origin.x + (b.size.width - w) / 2, b.origin.y + (b.size.height - h) / 2, w, h), img);
        CGImageRelease(img);
    }
}
@end

// Target for the buttons and the ticker, and the window's delegate.
@interface MkWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) mk::Window::Impl* owner;
- (void)buttonClicked:(id)sender;
- (void)tick:(NSTimer*)t;
@end

namespace mk {

struct Window::Impl {
    NSWindow* window = nil;
    MkVideoView* video = nil;
    NSTextField* status = nil;
    NSView* bar = nil;
    MkWindowDelegate* delegate = nil;
    NSTimer* timer = nil;
    std::vector<NSButton*> buttons;
    std::vector<std::function<void()>> actions;
    std::function<void()> closing;
    std::function<void()> ticker;
    bool open = false;
    bool stopped = false;

    void layout_buttons() {
        CGFloat x = kPad;
        for (NSButton* b : buttons) {
            [b sizeToFit];
            NSRect r = b.frame;
            r.size.width = std::max<CGFloat>(96, r.size.width + 20);
            r.size.height = kBtnH;
            r.origin = NSMakePoint(x, (kBarH - kBtnH) / 2);
            b.frame = r;
            x += r.size.width + kPad;
        }
    }
};

}  // namespace mk

@implementation MkWindowDelegate

- (void)buttonClicked:(id)sender {
    mk::Window::Impl* s = self.owner;
    if (!s) return;
    for (size_t i = 0; i < s->buttons.size(); ++i)
        if (s->buttons[i] == sender) {
            if (i < s->actions.size() && s->actions[i]) s->actions[i]();
            return;
        }
}

- (void)tick:(NSTimer*)t {
    (void)t;
    if (self.owner && self.owner->ticker) self.owner->ticker();
}

- (void)windowWillClose:(NSNotification*)n {
    (void)n;
    mk::Window::Impl* s = self.owner;
    if (!s || s->stopped) return;
    s->stopped = true;
    s->open = false;
    if (s->closing) s->closing();
    [NSApp stop:nil];
    // stop: only takes effect once the next event is handled, so post one.
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0]
             atStart:YES];
}
@end

namespace mk {

Window::Window() : p_(new Impl) {}
Window::~Window() { close(); }

bool Window::create(const std::string& title, int width, int height) {
    if (![NSThread isMainThread]) return false;
    Impl& s = *p_;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        const NSRect content = NSMakeRect(0, 0, width, height + kBarH + kStatusH);
        s.window = [[NSWindow alloc] initWithContentRect:content
                                               styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
        s.window.title = ns(title);
        s.window.releasedWhenClosed = NO;
        [s.window center];

        NSView* root = s.window.contentView;
        root.autoresizesSubviews = YES;

        s.status = [[NSTextField alloc] initWithFrame:NSMakeRect(kPad, 2, content.size.width - 2 * kPad, kStatusH - 4)];
        s.status.editable = NO;
        s.status.selectable = NO;
        s.status.bezeled = NO;
        s.status.drawsBackground = NO;
        s.status.font = [NSFont systemFontOfSize:11];
        s.status.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
        [root addSubview:s.status];

        s.bar = [[NSView alloc] initWithFrame:NSMakeRect(0, kStatusH, content.size.width, kBarH)];
        s.bar.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
        [root addSubview:s.bar];

        s.video = [[MkVideoView alloc] initWithFrame:NSMakeRect(0, kStatusH + kBarH, content.size.width,
                                                                content.size.height - kStatusH - kBarH)];
        s.video.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [root addSubview:s.video];

        s.delegate = [[MkWindowDelegate alloc] init];
        s.delegate.owner = &s;
        s.window.delegate = s.delegate;

        [s.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        s.open = true;
    }
    return true;
}

int Window::add_button(const std::string& label, std::function<void()> on_click) {
    Impl& s = *p_;
    const int id_ = (int)s.buttons.size();
    on_main([&s, label, on_click] {
        @autoreleasepool {
            NSButton* b = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 96, kBtnH)];
            b.title = ns(label);
            b.bezelStyle = NSBezelStyleRounded;
            b.target = s.delegate;
            b.action = @selector(buttonClicked:);
            [s.bar addSubview:b];
            s.buttons.push_back(b);
            s.actions.push_back(on_click);
            s.layout_buttons();
        }
    });
    return id_;
}

void Window::set_button_text(int id, const std::string& label) {
    Impl& s = *p_;
    on_main([&s, id, label] {
        if (id >= 0 && id < (int)s.buttons.size()) {
            s.buttons[id].title = ns(label);
            s.layout_buttons();
        }
    });
}

void Window::enable_button(int id, bool enable) {
    Impl& s = *p_;
    on_main([&s, id, enable] {
        if (id >= 0 && id < (int)s.buttons.size()) s.buttons[id].enabled = enable;
    });
}

void Window::set_status(const std::string& text) {
    Impl& s = *p_;
    on_main([&s, text] {
        if (s.status) s.status.stringValue = ns(text);
    });
}

void Window::show_frame(const VideoFrame& bgra) {
    if (bgra.format != PixelFormat::BGRA || bgra.empty() || !p_->video) return;
    [p_->video setFrameBGRA:bgra];
}

void Window::clear_frame() {
    if (p_->video) [p_->video clearFrame];
}

void Window::set_title(const std::string& title) {
    Impl& s = *p_;
    on_main([&s, title] {
        if (s.window) s.window.title = ns(title);
    });
}

std::string Window::choose_open_file(const std::string& filter_name, const std::string& pattern) {
    (void)filter_name;
    __block std::string out;
    std::vector<std::string> exts;
    for (size_t i = 0, j; i < pattern.size(); i = j + 1) {
        j = std::min(pattern.find(';', i), pattern.size());
        std::string e = pattern.substr(i, j - i);
        const size_t dot = e.rfind('.');
        if (dot != std::string::npos) exts.push_back(e.substr(dot + 1));
    }
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
#if __has_include(<UniformTypeIdentifiers/UniformTypeIdentifiers.h>)
        if (@available(macOS 11.0, *)) {
            NSMutableArray<UTType*>* types = [NSMutableArray array];
            for (auto& e : exts)
                if (UTType* t = [UTType typeWithFilenameExtension:ns(e)]) [types addObject:t];
            if (types.count) panel.allowedContentTypes = types;
        }
#endif
        if ([panel runModal] == NSModalResponseOK && panel.URL) out = panel.URL.path.UTF8String;
    }
    return out;
}

std::string Window::choose_save_file(const std::string& default_name) {
    __block std::string out;
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.nameFieldStringValue = ns(default_name);
#if __has_include(<UniformTypeIdentifiers/UniformTypeIdentifiers.h>)
        if (@available(macOS 11.0, *)) {
            const size_t dot = default_name.rfind('.');
            if (dot != std::string::npos)
                if (UTType* t = [UTType typeWithFilenameExtension:ns(default_name.substr(dot + 1))]) panel.allowedContentTypes = @[ t ];
        }
#endif
        if ([panel runModal] == NSModalResponseOK && panel.URL) out = panel.URL.path.UTF8String;
    }
    return out;
}

void Window::post(std::function<void()> fn) { on_main(std::move(fn)); }

void Window::on_close(std::function<void()> fn) { p_->closing = std::move(fn); }

void Window::set_timer(int interval_ms, std::function<void()> fn) {
    Impl& s = *p_;
    s.ticker = std::move(fn);
    on_main([&s, interval_ms] {
        @autoreleasepool {
            if (s.timer) [s.timer invalidate];
            s.timer = [NSTimer scheduledTimerWithTimeInterval:interval_ms / 1000.0
                                                       target:s.delegate
                                                     selector:@selector(tick:)
                                                     userInfo:nil
                                                      repeats:YES];
        }
    });
}

int Window::run() {
    Impl& s = *p_;
    if (!s.open) return 1;
    @autoreleasepool {
        [NSApp run];
    }
    return 0;
}

void Window::close() {
    Impl& s = *p_;
    if (!s.window) return;
    on_main_sync([&s] {
        @autoreleasepool {
            if (s.timer) [s.timer invalidate], s.timer = nil;
            NSWindow* w = s.window;
            s.window = nil;  // a close() from inside on_close() then does nothing
            [w close];       // -[windowWillClose:] runs on_close and ends run(), as a user close does
            w.delegate = nil;
            if (s.delegate) s.delegate.owner = nullptr;
            s.buttons.clear();
            s.actions.clear();
            s.video = nil;
            s.status = nil;
            s.bar = nil;
            s.delegate = nil;
            s.open = false;
        }
    });
}

}  // namespace mk
