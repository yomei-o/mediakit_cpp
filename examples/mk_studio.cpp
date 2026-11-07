// mediakit studio: camera preview, record to MP4 while previewing, play files back.
//   mk_studio [--size 1280x720]
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>

#include "mk_pipeline.h"
#include "mk_ui.h"

namespace {

std::string fmt_time(int64_t us) {
    char b[32];
    int s = (int)(us / 1000000);
    std::snprintf(b, sizeof(b), "%d:%02d", s / 60, s % 60);
    return b;
}

std::string default_name() {
    std::time_t t = std::time(nullptr);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[64];
    std::strftime(b, sizeof(b), "rec_%Y%m%d_%H%M%S.mp4", &tm);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    mk::RecorderConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &cfg.camera.width, &cfg.camera.height);
        else if (a == "--test") cfg.camera.device_id = "test";  // synthetic camera
        else if (a == "--sw") cfg.video.hardware = false;        // software H.264 encoder
    }
    cfg.preview_width = 960;  // the window scales anyway; smaller copy is cheaper

    mk::Window win;
    if (!win.create("mediakit studio", 960, 540)) return 1;
    std::unique_ptr<mk::Recorder> rec;
    std::unique_ptr<mk::Player> player;
    std::string last_file, note;
    int btn_rec = -1, btn_play = -1, btn_cam = -1;

    auto show = [&](const mk::VideoFrame& f) { win.show_frame(f); };
    auto start_camera = [&]() {
        player.reset();
        rec.reset(new mk::Recorder);
        win.clear_frame();
        if (!rec->open(cfg, show)) {
            note = "カメラを開けません: " + rec->error();
            rec.reset();
        } else {
            note = rec->input_name() + " " + std::to_string(rec->input_width()) + "x" + std::to_string(rec->input_height()) +
                   (rec->audio_name().empty() ? "  (マイクなし)" : "  / " + rec->audio_name());
        }
        win.set_button_text(btn_rec, "● 録画");
        win.enable_button(btn_rec, rec != nullptr);
    };
    auto play_file = [&](const std::string& path) {
        if (path.empty()) return;
        rec.reset();  // release the camera while playing
        player.reset(new mk::Player);
        win.clear_frame();
        if (!player->open(path, show, 960, 0)) {
            note = "開けません: " + player->error();
            player.reset();
            return;
        }
        player->play();
        const mk::MediaInfo& in = player->info();
        note = path + "  " + std::to_string(in.width) + "x" + std::to_string(in.height) + (in.has_audio ? " +音声" : "");
        win.enable_button(btn_rec, false);
    };

    btn_rec = win.add_button("● 録画", [&] {
        if (!rec) return;
        if (rec->recording()) {
            rec->stop_recording();
            mk::finish_saved_file(last_file);  // the browser: download it
            win.set_button_text(btn_rec, "● 録画");
            note = "保存しました: " + last_file + (rec->error().empty() ? "" : "  (" + rec->error() + ")");
        } else {
            std::string path = win.choose_save_file(default_name());
            if (path.empty()) return;
            if (rec->start_recording(path)) {
                last_file = path;
                win.set_button_text(btn_rec, "■ 停止");
            } else {
                note = "録画できません: " + rec->error();
            }
        }
    });
    btn_play = win.add_button("▶ 再生...", [&] {
        if (rec && rec->recording()) return;
        play_file(win.choose_open_file("動画", "*.mp4;*.mov;*.m4v;*.ts"));
    });
    win.add_button("⏯ 一時停止", [&] {
        if (!player) return;
        if (player->paused()) player->play();
        else player->pause();
    });
    btn_cam = win.add_button("カメラ", [&] { start_camera(); });
    (void)btn_play;
    (void)btn_cam;

    win.set_timer(200, [&] {
        std::string s;
        if (rec) {
            mk::RecorderStats st = rec->stats();
            char b[200];
            if (st.recording)
                std::snprintf(b, sizeof(b), "● 録画中 %s  %.1f MB  %lld フレーム  落ち %lld  [%s]  ", fmt_time(st.duration_us).c_str(),
                              st.bytes / 1048576.0, (long long)st.video_frames, (long long)st.dropped_frames, st.encoder.c_str());
            else
                std::snprintf(b, sizeof(b), "プレビュー %.1f fps  ", st.input_fps);
            s = b + note;
        } else if (player) {
            s = (player->finished() ? "再生終了 " : player->paused() ? "一時停止 " : "再生中 ") + fmt_time(player->position_us()) + " / " +
                fmt_time(player->info().duration_us) + "  " + note;
        } else {
            s = note;
        }
        win.set_status(s);
    });
    win.on_close([&] {
        rec.reset();  // finishes a recording in progress
        player.reset();
    });
    start_camera();
    return win.run();
}
