# mediakit_cpp

カメラ・マイクの取り込み、H.264 / AAC のエンコード、MP4 の保存と読み込み、デコード、RGB 変換、画面表示を、
Windows / Linux / macOS / Android / iOS で同じ C++ の API で使えるようにするライブラリ。

```
io/        カメラ・マイク・スピーカー（OS ごとに実装、API は共通）
codec/     H.264 / AAC エンコード・デコード、MP4 読み書き、画素変換（全 OS 共通、FFmpeg ベース）
ui/        映像を表示するウィンドウ（OS ごと）
pipeline/  io + codec の組み合わせ（Recorder / Player / FileSource、UI には依存しない）
common/    3 層で共有する型（VideoFrame / AudioFrame / Packet）
```

## 対応状況

| | Windows | Linux | macOS | Android | iOS |
|---|---|---|---|---|---|
| io（カメラ・マイク・スピーカー） | ✓ Media Foundation + WASAPI | – | – | – | – |
| codec | ✓ | （共通コード、ビルド設定は未） | 〃 | 〃 | 〃 |
| ui | ✓ Win32 + GDI | – | – | – | – |
| pipeline | ✓ | 〃 | 〃 | 〃 | 〃 |

## 使い方

```cpp
#include "mk_pipeline.h"
#include "mk_ui.h"

mk::Window win;
win.create("preview", 960, 540);
mk::Recorder rec;
mk::RecorderConfig cfg;                         // 既定のカメラとマイク（cfg.input_file = "in.mp4" でファイル入力）
rec.open(cfg, [&](const mk::VideoFrame& bgra) { win.show_frame(bgra); });   // プレビューはすぐ始まる
win.add_button("録画", [&] { rec.recording() ? rec.stop_recording() : rec.start_recording("out.mp4"); });
win.run();
```

録画中もプレビューは止まらない（カメラの映像をプレビュー用とエンコード用に分けて流す）。

```
Recorder  入力 ──┬─> プレビュー（BGRA、最新フレームだけ）──> コールバック
(カメラ+マイク     └─> H.264 / AAC エンコード ──┬─> MP4 ファイル      start_recording / stop_recording
 または MP4)                                   └─> PacketSink        add_sink（配信・クラウドなど任意の出力先）
Player    MP4 ─> 読み込み ─┬─> H.264 デコード ─> BGRA ─> コールバック（音声の時計に合わせる）
                           └─> AAC デコード ──────────> スピーカー
```

部品だけでも使える：

```cpp
mk::Camera cam;  cam.open({"", 1280, 720, 30});  cam.start([](const mk::VideoFrame& nv12) { ... });
mk::VideoEncoder enc;  enc.open(cfg);  enc.encode(frame, packets);     // H.264 Annex-B
mk::Mp4Writer w;  w.open("a.mp4", &enc, &aenc);  w.write(packet);
mk::Mp4Reader r;  r.open("a.mp4");  r.read(packet);
mk::VideoDecoder dec;  dec.decode(packet, frames);
mk::FrameConverter conv;  conv.convert(frame, bgra, mk::PixelFormat::BGRA, 640, 360);
```

### HW / SW

- エンコード：`VideoEncoderConfig::hardware`（既定 true）。Windows は Media Foundation の H.264 エンコーダで、
  GPU（NVIDIA / Intel / AMD）のものがあればそれ、無ければ / false なら Microsoft のソフトウェアエンコーダ。
  `encoder = "h264_mf"` のように名前で指定もできる。
- デコード：FFmpeg のソフトウェア H.264 デコーダ（全 OS 共通）。

### デバイスが無い環境

デバイス ID に `"test"` を渡すと、動くテスト画像と 440 Hz の音を出す疑似カメラ・疑似マイクになる
（ビルドサーバや CI でパイプライン全体を試せる）。

## サンプル

| | |
|---|---|
| `mk_studio` | プレビュー、録画（プレビューしながら）、再生のウィンドウアプリ。`--test` で疑似カメラ、`--sw` でソフトウェアエンコード |
| `mk_play file.mp4` | ウィンドウで再生（音付き） |
| `mk_record out.mp4 [秒]` | コンソールで録画。`--input in.mp4` でファイル入力、`--h264 out.h264` で PacketSink の例、`--test` で疑似デバイス |
| `mk_devices [--grab]` | カメラ・マイク・スピーカーの一覧 |

## ビルド（Windows）

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release     # VS 2022 の x64 開発者プロンプトで
cmake --build build
build/codec_test.exe            # エンコード→MP4→読み込み→デコード→RGB を画質（PSNR）と音（440 Hz）で検査
build/codec_test.exe --sw       # ソフトウェアエンコーダで同じ検査
build/mk_record.exe a.mp4 3 --test && build/player_test.exe a.mp4
```

exe は CRT を静的リンクしているので、そのままコピーして動く。

## ライセンス

- `codec/ffmpeg/` は FFmpeg 7.1.1 のソースを 1 フォルダずつに平坦化したもの（LGPL 2.1 以降）。
  GPL の部品（x264、libpostproc、GPL のフィルタ）は入れていない。
- H.264 / AAC には特許がある。OS のエンコーダ（Media Foundation など）を使う分は OS 側でライセンスされている。
