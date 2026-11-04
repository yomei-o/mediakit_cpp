// mediakit ui for iOS: UIKit. Same window as elsewhere -- video on top, a row of buttons and a
// status line -- but UIKit owns main() and the main thread, so the shape is different:
// main() here calls UIApplicationMain, and the app delegate starts the program's own main() on a
// thread of its own. Everything that touches UIKit hops to the main queue. mk_ui.h renames the
// program's main() to mk_ios_main() for this, so the examples build unchanged.
#import <UIKit/UIKit.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "mk_ui.h"
#undef main  // mk_ui.h renames the program's main(); the real one lives here

@class MkRootController;

namespace mk {
namespace {

const CGFloat kBarH = 56, kStatusH = 22, kBtnH = 40, kPad = 8;

MkRootController* g_root = nil;
std::mutex g_ready_mu;
std::condition_variable g_ready_cv;
bool g_ready = false;

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

std::string documents_dir() {
    NSArray* dirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return dirs.count ? std::string([dirs[0] UTF8String]) : std::string("/tmp");
}

}  // namespace
}  // namespace mk

// The view that shows the frames: BGRA, scaled to fit, centred on black.
@interface MkVideoView : UIView {
    std::vector<uint8_t> _pixels;
    int _w, _h;
    std::mutex _mu;
}
- (void)setFrameBGRA:(const mk::VideoFrame&)f;
- (void)clearFrame;
@end

@implementation MkVideoView

- (void)setFrameBGRA:(const mk::VideoFrame&)f {
    {
        std::lock_guard<std::mutex> lk(_mu);
        _w = f.width, _h = f.height;
        _pixels.resize((size_t)f.width * 4 * f.height);
        for (int y = 0; y < f.height; ++y)
            std::memcpy(_pixels.data() + (size_t)y * f.width * 4, f.data[0] + (size_t)y * f.stride[0], (size_t)f.width * 4);
    }
    mk::on_main([self] { [self setNeedsDisplay]; });
}

- (void)clearFrame {
    {
        std::lock_guard<std::mutex> lk(_mu);
        _pixels.clear();
        _w = _h = 0;
    }
    mk::on_main([self] { [self setNeedsDisplay]; });
}

- (void)drawRect:(CGRect)dirty {
    (void)dirty;
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
    CGContextFillRect(ctx, self.bounds);
    std::lock_guard<std::mutex> lk(_mu);
    if (_pixels.empty() || _w <= 0 || _h <= 0) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, _pixels.data(), _pixels.size(), nullptr);
    CGImageRef img = CGImageCreate(_w, _h, 8, 32, (size_t)_w * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, dp,
                                   nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    if (img) {
        const CGRect b = self.bounds;
        const double scale = std::min(b.size.width / _w, b.size.height / _h);
        const double w = _w * scale, h = _h * scale;
        const CGRect r = CGRectMake(b.origin.x + (b.size.width - w) / 2, b.origin.y + (b.size.height - h) / 2, w, h);
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, 0, b.size.height);  // UIKit's y grows downwards
        CGContextScaleCTM(ctx, 1, -1);
        CGContextDrawImage(ctx, CGRectMake(r.origin.x, b.size.height - r.origin.y - r.size.height, r.size.width, r.size.height), img);
        CGContextRestoreGState(ctx);
        CGImageRelease(img);
    }
}
@end

// Owns the views and forwards the taps.
@interface MkRootController : UIViewController
@property(nonatomic, strong) MkVideoView* video;
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UIView* bar;
@property(nonatomic, strong) NSMutableArray<UIButton*>* buttons;
@property(nonatomic, assign) mk::Window::Impl* owner;
- (void)buttonTapped:(UIButton*)sender;
@end

namespace mk {

struct Window::Impl {
    bool open = false;
    bool closed = false;
    std::vector<std::function<void()>> actions;
    std::function<void()> closing;
    std::function<void()> ticker;
    NSTimer* timer = nil;
    std::mutex mu;
    std::condition_variable cv;
};

}  // namespace mk

@implementation MkRootController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.buttons = [NSMutableArray array];
    self.view.backgroundColor = UIColor.blackColor;

    self.video = [[MkVideoView alloc] initWithFrame:CGRectZero];
    self.video.backgroundColor = UIColor.blackColor;
    [self.view addSubview:self.video];

    self.bar = [[UIView alloc] initWithFrame:CGRectZero];
    self.bar.backgroundColor = [UIColor colorWithWhite:0.12 alpha:1];
    [self.view addSubview:self.bar];

    self.status = [[UILabel alloc] initWithFrame:CGRectZero];
    self.status.font = [UIFont systemFontOfSize:11];
    self.status.textColor = UIColor.lightGrayColor;
    self.status.backgroundColor = [UIColor colorWithWhite:0.12 alpha:1];
    self.status.adjustsFontSizeToFitWidth = YES;
    [self.view addSubview:self.status];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    const CGRect b = self.view.bounds;
    const CGFloat top = self.view.safeAreaInsets.top;
    const CGFloat bottom = self.view.safeAreaInsets.bottom;
    const CGFloat status_y = b.size.height - bottom - mk::kStatusH;
    const CGFloat bar_y = status_y - mk::kBarH;
    self.video.frame = CGRectMake(0, top, b.size.width, bar_y - top);
    self.bar.frame = CGRectMake(0, bar_y, b.size.width, mk::kBarH);
    self.status.frame = CGRectMake(mk::kPad, status_y, b.size.width - 2 * mk::kPad, mk::kStatusH);
    CGFloat x = mk::kPad;
    for (UIButton* btn in self.buttons) {
        [btn sizeToFit];
        CGFloat w = std::max<CGFloat>(72, btn.frame.size.width + 2 * mk::kPad);
        btn.frame = CGRectMake(x, (mk::kBarH - mk::kBtnH) / 2, w, mk::kBtnH);
        x += w + mk::kPad;
    }
}

- (void)buttonTapped:(UIButton*)sender {
    mk::Window::Impl* s = self.owner;
    if (!s) return;
    const NSUInteger i = [self.buttons indexOfObject:sender];
    if (i != NSNotFound && i < s->actions.size() && s->actions[i]) s->actions[i]();
}
@end

// ---------------------------------------------------------------- app delegate and main()

@interface MkAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation MkAppDelegate

- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts {
    (void)app, (void)opts;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    MkRootController* root = [[MkRootController alloc] init];
    self.window.rootViewController = root;
    [self.window makeKeyAndVisible];
    mk::g_root = root;
    {
        std::lock_guard<std::mutex> lk(mk::g_ready_mu);
        mk::g_ready = true;
    }
    mk::g_ready_cv.notify_all();
    return YES;
}
@end

static int g_argc = 0;
static char** g_argv = nullptr;

int main(int argc, char** argv) {
    g_argc = argc, g_argv = argv;
    @autoreleasepool {
        // The program's own main() runs on a thread of its own; UIKit keeps this one.
        std::thread([] {
            mk_ios_main(g_argc, g_argv);
            dispatch_async(dispatch_get_main_queue(), ^{
              // nothing to do: an iOS app does not exit, it goes back to the home screen
            });
        }).detach();
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([MkAppDelegate class]));
    }
}

namespace mk {

Window::Window() : p_(new Impl) {}
Window::~Window() { close(); }

bool Window::create(const std::string& title, int width, int height) {
    (void)title, (void)width, (void)height;  // the app fills the screen
    Impl& s = *p_;
    {
        std::unique_lock<std::mutex> lk(g_ready_mu);
        if (!g_ready_cv.wait_for(lk, std::chrono::seconds(30), [] { return g_ready; })) return false;
    }
    on_main_sync([&s] { g_root.owner = &s; });
    s.open = true;
    return true;
}

int Window::add_button(const std::string& label, std::function<void()> on_click) {
    Impl& s = *p_;
    const int id_ = (int)s.actions.size();
    s.actions.push_back(std::move(on_click));
    on_main_sync([label] {
        @autoreleasepool {
            UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
            [b setTitle:ns(label) forState:UIControlStateNormal];
            b.titleLabel.font = [UIFont systemFontOfSize:15];
            b.backgroundColor = [UIColor colorWithWhite:0.25 alpha:1];
            [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
            [b setTitleColor:UIColor.grayColor forState:UIControlStateDisabled];
            b.layer.cornerRadius = 6;
            [b addTarget:g_root action:@selector(buttonTapped:) forControlEvents:UIControlEventTouchUpInside];
            [g_root.bar addSubview:b];
            [g_root.buttons addObject:b];
            [g_root.view setNeedsLayout];
        }
    });
    return id_;
}

void Window::set_button_text(int id, const std::string& label) {
    on_main([id, label] {
        if (id >= 0 && id < (int)g_root.buttons.count) {
            [g_root.buttons[id] setTitle:ns(label) forState:UIControlStateNormal];
            [g_root.view setNeedsLayout];
        }
    });
}

void Window::enable_button(int id, bool enable) {
    on_main([id, enable] {
        if (id >= 0 && id < (int)g_root.buttons.count) g_root.buttons[id].enabled = enable;
    });
}

void Window::set_status(const std::string& text) {
    on_main([text] { g_root.status.text = ns(text); });
}

void Window::show_frame(const VideoFrame& bgra) {
    if (bgra.format != PixelFormat::BGRA || bgra.empty() || !g_root) return;
    [g_root.video setFrameBGRA:bgra];
}

void Window::clear_frame() {
    if (g_root) [g_root.video clearFrame];
}

void Window::set_title(const std::string&) {}  // an iOS app has no title bar

std::string Window::choose_open_file(const std::string&, const std::string& pattern) {
    // No picker without a document browser: take the newest matching file in Documents.
    std::vector<std::string> exts;
    for (size_t i = 0, j; i < pattern.size(); i = j + 1) {
        j = std::min(pattern.find(';', i), pattern.size());
        const std::string e = pattern.substr(i, j - i);
        const size_t dot = e.rfind('.');
        if (dot != std::string::npos) exts.push_back(e.substr(dot));
    }
    @autoreleasepool {
        NSString* dir = ns(documents_dir());
        NSArray<NSString*>* names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil];
        std::string best;
        NSDate* best_t = nil;
        for (NSString* n in names) {
            const std::string name = n.UTF8String;
            bool match = exts.empty();
            for (auto& e : exts)
                if (name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0) match = true;
            if (!match) continue;
            NSString* full = [dir stringByAppendingPathComponent:n];
            NSDate* t = [NSFileManager.defaultManager attributesOfItemAtPath:full error:nil].fileModificationDate;
            if (!best_t || (t && [t compare:best_t] != NSOrderedAscending)) best_t = t, best = full.UTF8String;
        }
        return best;
    }
}

std::string Window::choose_save_file(const std::string& default_name) { return documents_dir() + "/" + default_name; }

void Window::post(std::function<void()> fn) { on_main(std::move(fn)); }

void Window::on_close(std::function<void()> fn) { p_->closing = std::move(fn); }

void Window::set_timer(int interval_ms, std::function<void()> fn) {
    Impl& s = *p_;
    s.ticker = std::move(fn);
    on_main([&s, interval_ms] {
        @autoreleasepool {
            if (s.timer) [s.timer invalidate];
            s.timer = [NSTimer scheduledTimerWithTimeInterval:interval_ms / 1000.0
                                                      repeats:YES
                                                        block:^(NSTimer*) {
                                                          if (s.ticker) s.ticker();
                                                        }];
        }
    });
}

int Window::run() {
    Impl& s = *p_;
    if (!s.open) return 1;
    // UIKit's loop is on the main thread; this one just waits for close().
    std::unique_lock<std::mutex> lk(s.mu);
    s.cv.wait(lk, [&s] { return s.closed; });
    return 0;
}

void Window::close() {
    Impl& s = *p_;
    if (!s.open || s.closed) return;
    if (s.closing) s.closing();
    on_main_sync([&s] {
        if (s.timer) [s.timer invalidate], s.timer = nil;
        g_root.owner = nullptr;
    });
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.closed = true;
    }
    s.cv.notify_all();
}

}  // namespace mk
