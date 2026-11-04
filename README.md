# mediakit_cpp

カメラ・マイクの取り込み、H.264 / AAC のエンコード、MP4 の保存と読み込み、デコード、RGB 変換、画面表示を、
Windows / Linux / macOS / Android / iOS で同じ C++ の API で使えるようにするライブラリ。

```
io/        カメラ・マイク・スピーカー（OS ごとに実装、API は共通）
codec/     H.264 / AAC エンコード・デコード、MP4 読み書き、画素変換（全 OS 共通、FFmpeg ベース）
ui/        映像を表示するウィンドウ（OS ごと）
pipeline/  io + codec の組み合わせ（Recorder / Player / FileSource、UI には依存しない）
common/    3 層で共有する型（VideoFrame / AudioFrame / Packet）
testdata/  chicago.h264 + chicago.aac（1280x720 30fps、415 秒）
```

## 対応状況

| | Windows | Linux | macOS | Android | iOS |
|---|---|---|---|---|---|
| io（カメラ・マイク・スピーカー） | ✓ Media Foundation + WASAPI | V4L2 + ALSA（ビルドのみ） | – | – | – |
| codec | ✓ | ✓（WSL で codec_test 通過） | – | – | – |
| ui | ✓ Win32 + GDI | X11（ビルドのみ） | – | – | – |
| pipeline | ✓ | ✓（疑似デバイスで録画→再生通過） | – | – | – |

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

録画中もプレビューは止まらない（入力の映像をプレビュー用とエンコード用に分けて流す）。

```
Recorder  入力 ──┬─> プレビュー（BGRA、最新フレームだけ）──> コールバック
(カメラ+マイク     └─> H.264 / AAC エンコード ──┬─> MP4 ファイル      start_recording / stop_recording
 または MP4)                                   └─> PacketSink        add_sink（配信・クラウドなど任意の出力先）
Player    MP4 ─> 読み込み ─┬─> H.264 デコード ─> BGRA ─> コールバック（音声の時計に合わせる）
                           └─> AAC デコード ──────────> スピーカー
```

部品だけでも使える：

```cpp
mk::Camera cam;  cam.open({"", 1280, 720, 30});  cam.start([](const mk::VideoFrame& f) { ... });  // NV12（Linux の MJPEG カメラは MJPEG）
mk::VideoEncoder enc;  enc.open(cfg);  enc.encode(frame, packets);     // H.264 Annex-B（キーフレームには毎回 SPS/PPS）
mk::Mp4Writer w;  w.open("a.mp4", &enc, &aenc);  w.write(packet);      // StreamInfo を渡せば再エンコードなしの書き出しにも
mk::Mp4Reader r;  r.open("a.mp4");  r.read(packet);                    // .mp4 / .mov / .ts / .h264 / .aac
mk::VideoDecoder dec;  dec.open(true);  dec.decode(packet, frames);    // true: GPU デコード（無ければ SW）
mk::FrameConverter conv;  conv.convert(frame, bgra, mk::PixelFormat::BGRA, 640, 360);  // MJPEG もここで展開
```

### HW / SW

| | HW | SW |
|---|---|---|
| H.264 エンコード | Windows: Media Foundation（GPU の MFT） | Microsoft の SW MFT、または x264（`-DMK_WITH_X264=ON`） |
| H.264 デコード | Windows: D3D11VA（`VideoDecoder::open(true)`） | FFmpeg |

- `VideoEncoderConfig::hardware`（既定 true）。HW が無いマシンでは自動で SW になる。`encoder = "libx264"` のように名前で指定もできる。
- **x264 は GPL**。`MK_WITH_X264=ON` でビルドした exe は GPL になる。既定（OFF）は LGPL の部品だけ。

### デバイスが無い環境

デバイス ID に `"test"` を渡すと、動くテスト画像と 440 Hz の音を出す疑似カメラ・疑似マイクになる
（ビルドサーバや CI でパイプライン全体を試せる）。

### 出力先を足す（PacketSink）

`PacketSink`（open / write / close）を実装して `Recorder::add_sink()` すると、MP4 と同時に任意の出力先へ
エンコード済みパケットを流せる。最初に届く映像はキーフレームで、キーフレームには SPS/PPS が付いている。
配信サーバやクラウドへの送信はこの形で外から足す（`mk_record --h264` が最小の例）。

## サンプル

| | |
|---|---|
| `mk_studio` | プレビュー、録画（プレビューしながら）、再生のウィンドウアプリ。`--test` で疑似カメラ、`--sw` で SW エンコード |
| `mk_play file.mp4` | ウィンドウで再生（音付き） |
| `mk_record out.mp4 [秒]` | コンソールで録画。`--input in.mp4` でファイル入力、`--h264 out.h264` で PacketSink の例、`--test` で疑似デバイス |
| `mk_mux out.mp4 a.h264 [a.aac]` | H.264 と AAC を再エンコードなしで MP4 に（`mk_mux chicago.mp4 testdata/chicago.h264 testdata/chicago.aac`） |
| `mk_devices [--grab]` | カメラ（対応モードも）・マイク・スピーカーの一覧 |

## ビルド

Windows（VS 2022 の x64 開発者プロンプト）：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release        # x264 も使うなら -DMK_WITH_X264=ON
cmake --build build
build/codec_test.exe            # エンコード→MP4→読み込み→デコード→RGB を画質（PSNR）と音（440 Hz）で検査
build/codec_test.exe --sw --hwdec
build/mk_record.exe a.mp4 3 --test && build/player_test.exe a.mp4
```

exe は CRT を静的リンクしているので、そのままコピーして動く。

Linux（`libx11-dev libasound2-dev` が必要。OS の H.264 エンコーダは未対応なので、エンコードには `-DMK_WITH_X264=ON`）：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMK_WITH_X264=ON
cmake --build build -j
```

## ライセンス

- `codec/ffmpeg/` は FFmpeg 7.1.1 のソースを 1 フォルダずつに平坦化したもの（LGPL 2.1 以降）。
- `codec/x264/` は x264（GPL 2 以降）。`MK_WITH_X264=ON` のときだけ使う。
- H.264 / AAC には特許がある。OS のエンコーダ（Media Foundation など）を使う分は OS 側でライセンスされている。
