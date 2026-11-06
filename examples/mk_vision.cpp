// mediakit vision: the base for analysing camera frames at a low rate (e.g. number-plate recognition).
//   mk_vision [--fps 2] [--size 1280x720] [--device <id>] [--test]
// Each frame, as BGRA, goes through analyze(); its boxes are drawn on the frame and the frame is shown.
// analyze() here only marks plate-like areas (dense vertical edges in a 2:1 box): replace it with the
// real detector / OCR. In the browser (wasm) the camera itself delivers at --fps; elsewhere the camera
// runs at its own rate and frames in between are skipped.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "mk_io.h"
#include "mk_ui.h"
#ifndef __EMSCRIPTEN__
#include "mk_codec.h"  // native cameras deliver NV12 / MJPEG / ...; the browser delivers BGRA
#endif

namespace {

struct Detection {
    int x = 0, y = 0, w = 0, h = 0;  // in frame pixels
    double score = 0;
    std::string text;                // what the recognizer read (empty: not read)
};

// Placeholder detector: plates are a 2:1 box full of character strokes, i.e. dense vertical edges.
std::vector<Detection> analyze(const mk::VideoFrame& bgra) {
    const int gw = 320, gh = std::max(1, bgra.height * gw / std::max(1, bgra.width));
    std::vector<uint8_t> gray((size_t)gw * gh);
    for (int y = 0; y < gh; ++y) {
        const uint8_t* row = bgra.data[0] + (size_t)(y * bgra.height / gh) * bgra.stride[0];
        for (int x = 0; x < gw; ++x) {
            const uint8_t* p = row + (size_t)(x * bgra.width / gw) * 4;
            gray[(size_t)y * gw + x] = (uint8_t)((p[0] * 29 + p[1] * 150 + p[2] * 77) >> 8);
        }
    }
    // integral image of "strong horizontal gradient" pixels
    std::vector<int> sum((size_t)(gw + 1) * (gh + 1), 0);
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            const int g = x > 0 && x < gw - 1 ? std::abs(gray[(size_t)y * gw + x + 1] - gray[(size_t)y * gw + x - 1]) : 0;
            sum[(size_t)(y + 1) * (gw + 1) + x + 1] = (g > 48) + sum[(size_t)y * (gw + 1) + x + 1] + sum[(size_t)(y + 1) * (gw + 1) + x] -
                                                    sum[(size_t)y * (gw + 1) + x];
        }
    auto edges = [&](int x, int y, int w, int h) {
        return sum[(size_t)(y + h) * (gw + 1) + x + w] - sum[(size_t)y * (gw + 1) + x + w] - sum[(size_t)(y + h) * (gw + 1) + x] +
               sum[(size_t)y * (gw + 1) + x];
    };
    std::vector<Detection> cand;
    for (int w : {40, 60, 84, 120}) {
        const int h = w / 2;
        for (int y = 0; y + h <= gh; y += 4)
            for (int x = 0; x + w <= gw; x += 4) {
                const double d = edges(x, y, w, h) / (double)(w * h);
                if (d > 0.22) cand.push_back({x, y, w, h, d, ""});
            }
    }
    std::sort(cand.begin(), cand.end(), [](const Detection& a, const Detection& b) { return a.score > b.score; });
    std::vector<Detection> out;
    for (const Detection& c : cand) {  // keep the best of overlapping boxes
        bool overlaps = false;
        for (const Detection& o : out) {
            const int ix = std::max(0, std::min(c.x + c.w, o.x + o.w) - std::max(c.x, o.x));
            const int iy = std::max(0, std::min(c.y + c.h, o.y + o.h) - std::max(c.y, o.y));
            if (ix * iy > 0) overlaps = true;
        }
        if (!overlaps) out.push_back(c);
        if (out.size() == 3) break;
    }
    for (Detection& d : out) {  // back to frame pixels
        d.x = d.x * bgra.width / gw, d.w = d.w * bgra.width / gw;
        d.y = d.y * bgra.height / gh, d.h = d.h * bgra.height / gh;
    }
    return out;
}

void draw_box(mk::VideoFrame& f, const Detection& d, uint32_t bgra, int thick) {
    auto fill = [&](int x0, int y0, int x1, int y1) {
        x0 = std::max(0, x0), y0 = std::max(0, y0), x1 = std::min(f.width, x1), y1 = std::min(f.height, y1);
        for (int y = y0; y < y1; ++y) {
            uint32_t* row = (uint32_t*)(f.data[0] + (size_t)y * f.stride[0]);
            std::fill(row + x0, row + std::max(x0, x1), bgra);
        }
    };
    fill(d.x, d.y, d.x + d.w, d.y + thick);
    fill(d.x, d.y + d.h - thick, d.x + d.w, d.y + d.h);
    fill(d.x, d.y, d.x + thick, d.y + d.h);
    fill(d.x + d.w - thick, d.y, d.x + d.w, d.y + d.h);
}

}  // namespace

int main(int argc, char** argv) {
    mk::CameraConfig cfg;
    cfg.fps = 2;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fps" && i + 1 < argc) cfg.fps = std::max(1, std::atoi(argv[++i]));
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &cfg.width, &cfg.height);
        else if (a == "--device" && i + 1 < argc) cfg.device_id = argv[++i];
        else if (a == "--test") cfg.device_id = "test";  // synthetic camera
    }
    const int analyze_fps = cfg.fps;
#ifndef __EMSCRIPTEN__
    cfg.fps = 30;  // native cameras rarely go as low as 2 fps; skip frames instead
#endif

    mk::Window win;
    if (!win.create("mediakit vision", 960, 540)) return 1;
    mk::Camera cam;
    std::mutex mu;  // the camera callback runs on a capture thread except in the browser
    struct {
        int64_t frames = 0, first_us = 0, last_us = 0;
        double analyze_ms = 0;
        size_t found = 0;
        std::string text;
    } stats;
    int64_t next_us = 0;
#ifndef __EMSCRIPTEN__
    mk::FrameConverter conv;
#endif

    auto on_frame = [&](const mk::VideoFrame& in) {
        if (in.pts_us < next_us) return;
        next_us = std::max(next_us + 1000000 / analyze_fps, in.pts_us - 1000000 / analyze_fps / 2);
        mk::VideoFrame f;
#ifndef __EMSCRIPTEN__
        if (in.format != mk::PixelFormat::BGRA) {
            if (!conv.convert(in, f, mk::PixelFormat::BGRA)) return;
            f.pts_us = in.pts_us;
        } else
#endif
            f = in;
        const int64_t t0 = mk::now_us();
        std::vector<Detection> found = analyze(f);
        const double ms = (mk::now_us() - t0) / 1000.0;
        std::string text;
        for (const Detection& d : found) {
            draw_box(f, d, d.text.empty() ? 0xffffc800u : 0xff00ff00u, std::max(2, f.width / 400));  // 0xAARRGGBB: amber candidate, green read
            if (!d.text.empty()) text += (text.empty() ? "" : " / ") + d.text;
        }
        win.show_frame(f);
        std::lock_guard<std::mutex> lk(mu);
        if (!stats.frames) stats.first_us = f.pts_us;
        ++stats.frames;
        stats.last_us = f.pts_us;
        stats.analyze_ms = ms;
        stats.found = found.size();
        stats.text = text;
    };

    bool running = false;
    int btn = -1;
    auto start = [&] {
        if (!cam.open(cfg)) {
            win.set_status("カメラを開けません: " + cam.error());
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            stats.frames = 0;
        }
        running = cam.start(on_frame);
        win.set_button_text(btn, running ? "■ 停止" : "▶ 開始");
    };
    btn = win.add_button("■ 停止", [&] {
        if (running) {
            cam.stop();
            running = false;
            win.set_button_text(btn, "▶ 開始");
        } else {
            start();
        }
    });
    win.set_timer(500, [&] {
        if (!running) return;
        std::lock_guard<std::mutex> lk(mu);
        char b[256];
        const double fps = stats.frames > 1 ? (stats.frames - 1) * 1e6 / (double)(stats.last_us - stats.first_us) : 0;
        std::snprintf(b, sizeof(b), "解析 %.1f fps  処理 %.1f ms  候補 %zu  %s |  %s %dx%d", fps, stats.analyze_ms, stats.found,
                      stats.text.c_str(), cam.name().c_str(), cam.width(), cam.height());
        win.set_status(b);
    });
    win.on_close([&] { cam.close(); });
    start();
    return win.run();
}
