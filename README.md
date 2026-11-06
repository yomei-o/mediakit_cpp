# mediakit_cpp

カメラ・マイクの取り込み、H.264 / AAC のエンコード、MP4 の保存と読み込み、デコード、RGB 変換、画面表示を、
Windows / Linux / macOS / Android / iOS / ブラウザ（wasm）で同じ C++ の API で使えるようにするライブラリ。

```
io/        カメラ・マイク・スピーカー（OS ごとに実装、API は共通）
codec/     H.264 / AAC エンコード・デコード、MP4 読み書き、画素変換（全 OS 共通、FFmpeg ベース）
ui/        映像を表示するウィンドウ（OS ごと）
pipeline/  io + codec の組み合わせ（Recorder / Player / FileSource、UI には依存しない）
common/    3 層で共有する型（VideoFrame / AudioFrame / Packet）
testdata/  chicago.h264 + chicago.aac（1280x720 30fps、415 秒）
codec/ffmpeg/, codec/x264/, codec/openh264/  平坦化して同梱した第三者ソース
```

## 対応状況

| | Windows | Linux | macOS | Android | iOS | ブラウザ（wasm） |
|---|---|---|---|---|---|---|
| io（カメラ・マイク・スピーカー） | ✓ Media Foundation + WASAPI | ✓ V4L2 + ALSA（WSL2 で USB カメラとそのマイク、WSLg のスピーカーを確認） | ✓ AVFoundation + AudioQueue | ✓ Camera2 NDK + AAudio | ✓ AVFoundation + AudioQueue（iPhone 16 実機で確認） | ✓ getUserMedia + Web Audio（Edge + USB カメラ・そのマイク・PC のスピーカーで確認） |
| codec | ✓ | ✓（WSL で codec_test 通過） | ✓ VideoToolbox | ✓ MediaCodec | ✓ VideoToolbox | ✓（node で codec_test 通過、エンコードは `-DMK_WITH_OPENH264=ON`） |
| ui | ✓ Win32 + GDI | ✓ X11（WSL2 の WSLg で確認） | ✓ AppKit | ✓ NativeActivity + ANativeWindow | ✓ UIKit | ✓ canvas + HTML のボタン |
| pipeline | ✓ | ✓（疑似デバイスで録画→再生通過） | ✓ | ✓ | ✓（iPhone 16 実機でプレビュー→録画→再生） | ―（スレッドが要るので未対応） |

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
| H.264 エンコード | Windows: Media Foundation（GPU の MFT）／macOS: VideoToolbox／Android: MediaCodec | Microsoft の SW MFT、OpenH264（`-DMK_WITH_OPENH264=ON`）、x264（`-DMK_WITH_X264=ON`） |
| H.264 デコード | Windows: D3D11VA／macOS: VideoToolbox／Android: MediaCodec（`VideoDecoder::open(true)`） | FFmpeg |

- `VideoEncoderConfig::hardware`（既定 true）。HW が無いマシンでは自動で SW になる。`encoder = "libx264"` のように名前で指定もできる。
- **x264 は GPL**。`MK_WITH_X264=ON` でビルドした exe は GPL になる。既定（OFF）は LGPL の部品だけ。
- **OpenH264 は BSD 2-clause**なので `MK_WITH_OPENH264=ON` でも LGPL のまま。HW エンコーダが無い環境に
  SW エンコーダを入れたいが GPL は困る、という場合はこちら（`encoder = "libopenh264"`）。
  同じビットレートでの画質は x264 と同等〜少し上（`codec_test` で 48.0 dB、x264 46.1 dB、VideoToolbox 49.9 dB）。

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
| `mk_vision [--fps 2]` | 低い fps でフレームを解析するアプリの土台（車番認識など）。`analyze()` を本物の認識に差し替えて使う。ブラウザでも動く |

## ビルド

Windows（VS 2022 の x64 開発者プロンプト）：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release        # SW エンコーダも要るなら -DMK_WITH_OPENH264=ON（BSD）か -DMK_WITH_X264=ON（GPL）
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
build/audio_loopback_test       # 既定のスピーカーで 777 Hz を鳴らし、既定のマイクで聞こえるかを検査（実機が要る）
```

macOS（Xcode のコマンドラインツールだけで足りる）：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
build/codec_test && build/codec_test --hwdec
build/mk_record a.mp4 3 --test && build/player_test a.mp4
```

カメラとマイクは初回に許可を聞かれる。バンドルの無いコマンドラインの実行ファイルでも聞けるよう、
`tools/mac_info.plist`（`NSCameraUsageDescription` など）を `-sectcreate __TEXT __info_plist` で
実行ファイルに埋め込んである。これが無いと、カメラに触れた瞬間に OS がプロセスを殺す。

Android（NDK r27、API 26 以上）：

```sh
NDK=$ANDROID_HOME/ndk/27.0.12077973
cmake -S . -B build-android -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j
adb push build-android/codec_test build-android/mk_record build-android/player_test /data/local/tmp/
adb shell "cd /data/local/tmp && chmod 755 * && ./codec_test --hwdec"

tools/build_apk.sh build-android/libmk_studio.so      # NativeActivity の APK を作る（Java は無し）
adb install -r build-android/mk_studio.apk
adb shell am start -n org.mediakit.mk_studio/android.app.NativeActivity
```

`mk_studio` と `mk_play` は Android では共有ライブラリになる。NativeActivity がそれを読み込んで
`android_main()` を呼び、`ui` 側の `android_main()` がプログラムの `main()` を呼ぶので、サンプルの
ソースはそのまま。カメラとマイクの許可も `ui` が起動時に JNI で聞く。

Android のウィンドウには NDK に文字を描く手段が無いので、ボタンとステータスの文字だけ JNI 経由で
`android.graphics.Paint` に描かせている。ファイル選択のダイアログも無いので、`choose_save_file()` は
アプリの外部ファイル領域のパスを返し、`choose_open_file()` はそこにある一番新しいファイルを返す。

- MediaCodec を Java から使うアプリでは `mk::set_java_vm(vm)` を `JNI_OnLoad` で一度呼ぶ。
  呼ばなければ FFmpeg は NDK の MediaCodec を使う（それも無理なら SW にそのまま落ちる）。
- カメラの結果は binder で返ってくるので、Java を持たないネイティブだけのプロセスでは
  `io` が `ABinderProcess_startThreadPool()` を自分で呼んでいる（API 29 以上）。

iOS（実機 SDK とシミュレータ SDK のどちらもビルドが通る。署名は Xcode の Apple ID 任せで、無料の Personal Team で足りる）：

```sh
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
tools/build_ios_app.sh mk_studio          # 署名して接続中の iPhone に入れる
xcrun devicectl device process launch --device <UDID> org.mediakit.mkstudio
```

初回だけ iPhone 側で「設定 → 一般 → VPN とデバイス管理 → デベロッパ APP」から
そのデベロッパを信頼する操作が要る。これをしないと起動が
`its profile has not been explicitly trusted by the user` で拒否される。
バンドル ID からアンダースコアが抜けている（`mk_studio` → `org.mediakit.mkstudio`）のは、
App ID に使える文字が英数字と `-` `.` だけで、Info.plist と署名した App ID が食い違うと
installd が `invalid entitlements` で弾くため。

シミュレータで動かすなら：

```sh
cmake -S . -B build-ios-sim -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
cmake --build build-ios-sim --config Release
xcrun simctl install booted build-ios-sim/Release-iphonesimulator/mk_studio.app
xcrun simctl launch booted org.mediakit.mkstudio --test     # シミュレータにカメラは無い
# コンソールのテストはバンドルの中身をそのまま動かせる
xcrun simctl spawn booted build-ios-sim/Release-iphonesimulator/codec_test.app/codec_test --hwdec
```

UIKit が `main()` とメインスレッドを持っていくので、`ui` 側が `main()` を用意して
`UIApplicationMain` を回し、プログラムの `main()` は別スレッドで走らせる。そのために `mk_ui.h` が
iOS のときだけ `main` を `mk_ios_main` に付け替えるので、サンプルのソースはそのまま。
ファイル選択のダイアログは使っていないので、保存先は `Documents`、読み込みはそこの最新ファイル。

ブラウザ（wasm、Emscripten。emsdk の環境で）：

```sh
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --target mk_vision audio_loopback_test
cd build-wasm && python -m http.server 8000      # http://localhost:8000/mk_vision.html を開く
# 引数は URL で渡す：mk_vision.html?args=--test%20--fps%204
# audio_loopback_test.html：スピーカー→マイクのテスト（ページをクリックすると始まる）
cmake --build build-wasm --target mk_mux codec_test   # codec は node で動く（node build-wasm/mk_mux.js ...）
```

カメラ・マイクは `https://` か `http://localhost` のページでないと使えない（ブラウザの決まり）。
wasm 版はスレッドを使わない（SharedArrayBuffer や COOP/COEP のヘッダが要らない）代わりに、次の点が他の OS と違う。

- `Camera::open()` / `Microphone::open()` はブラウザの許可ダイアログを待つ。そのためプログラムは `-sASYNCIFY` でリンクする（CMake が付ける）。
- フレームは JS のタイマーが `CameraConfig::fps` どおりに `<video>` から取り出し、メインスレッドで
  コールバックに渡す。届くのはプログラムが待っている間（`Window::run()` の中など）だけなので、
  コールバックの処理が重いと画面も止まる（2 fps で数十 ms 程度なら問題ない）。
- フレームは BGRA で届くので、FrameConverter（codec）をリンクしなくても使える。
- マイク・スピーカーは Web Audio の AudioWorklet で 10 ms ごとに受け渡す。マイクの音もカメラと同じく
  プログラムが待っている間にメインスレッドで届く。ブラウザはページがクリック（かキー入力）されるまで
  音を止めておくので、それまでマイクは何も届けず、スピーカーも鳴らない（最初のクリックで動き出す）。
  スピーカーの出力先を選ぶ（`device_id`）のは `setSinkId` のあるブラウザ（Chrome / Edge）だけ。
- `Window` は `id="mk-root"` の要素（`tools/wasm_shell.html`）に canvas とボタンを置く。
  `choose_open_file()` はブラウザのファイル選択で選んだファイルを MEMFS の `/tmp` にコピーして返す。
- Recorder / Player（pipeline）はまだ無い。H.264 のエンコーダは OS のものが無いので、
  エンコードするなら `-DMK_WITH_OPENH264=ON`。

## ライセンス

- `codec/ffmpeg/` は FFmpeg 7.1.1 のソースを 1 フォルダずつに平坦化したもの（LGPL 2.1 以降）。
- `codec/x264/` は x264（GPL 2 以降）。`MK_WITH_X264=ON` のときだけ使う。
- `codec/openh264/` は OpenH264 2.6.0（BSD 2-clause、`codec/openh264/LICENSE`）をエンコーダだけ平坦化したもの。
  `MK_WITH_OPENH264=ON` のときだけ使う。
- H.264 / AAC には特許がある。OS のエンコーダ（Media Foundation など）を使う分は OS 側でライセンスされている。
