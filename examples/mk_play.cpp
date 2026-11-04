// Play an MP4 in a window with sound.
//   mk_play file.mp4     (no argument: a file dialog opens)
#include <cstdio>
#include <string>

#include "mk_pipeline.h"
#include "mk_ui.h"

int main(int argc, char** argv) {
    mk::Window win;
    if (!win.create("mediakit play", 960, 540)) return 1;
    std::string path = argc > 1 ? argv[1] : win.choose_open_file("動画", "*.mp4;*.mov;*.m4v;*.ts");
    if (path.empty()) return 0;
    mk::Player player;
    if (!player.open(path, [&](const mk::VideoFrame& f) { win.show_frame(f); })) {
        win.set_status("開けません: " + player.error());
        return win.run();
    }
    win.set_title("mediakit play - " + path);
    win.add_button("⏯ 一時停止", [&] {
        if (player.paused()) player.play();
        else player.pause();
    });
    win.set_timer(200, [&] {
        char b[128];
        std::snprintf(b, sizeof(b), "%s %.1f / %.1f s  %s", player.finished() ? "終了" : player.paused() ? "一時停止" : "再生中",
                      player.position_us() / 1e6, player.info().duration_us / 1e6, player.error().c_str());
        win.set_status(b);
    });
    win.on_close([&] { player.close(); });
    player.play();
    return win.run();
}
