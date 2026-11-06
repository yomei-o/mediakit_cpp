# 作業メモ（続きをやる人向け）

## 経緯

元は `SafieGoproProxy_reenc_002.zip`（Android + Windows の GoPro → RTSP → 再エンコード → Safie 送信）。
Safie 接続部分（spyderClientCoreLib、curl、libressl、msgpack、libuv、rtsp、device/）は全部捨てた。
使ったのは平坦化済みの FFmpeg 7.1.1（`codec/ffmpeg/`）と x264（`codec/x264/`、任意）だけ。
OpenH264（`codec/openh264/`、任意）は後から足した。
Safie への出力は公開できないので入れない。`PacketSink` を実装すれば外から（非公開の別リポで）いつでもつなげられる。

## 方針

- 3 層：`io/`（OS ごと）、`codec/`（全 OS 共通）、`ui/`（OS ごと）。`pipeline/` は io + codec で UI に依存しない。
- 映像の受け渡しは NV12 が基本、表示は BGRA。音は s16 インタリーブ。圧縮映像は H.264 Annex-B、音は生 AAC。
- MJPEG しか出さないカメラ（Linux の UVC など）は io が MJPEG のまま渡し、codec の FrameConverter が展開する
  （io を codec に依存させないため）。
- 時刻は全部 `mk::now_us()`（steady_clock）。WASAPI の QPC 時刻、MF のデバイス時刻、V4L2 の MONOTONIC はここに換算。
- エンコーダは OS のもの（Windows: h264_mf、macOS/iOS: h264_videotoolbox、Android: h264_mediacodec、
  Linux: h264_vaapi / h264_v4l2m2m（未ビルド））、SW は OpenH264（BSD、任意）か x264（GPL、任意）。
- 各キーフレームの前に SPS/PPS を必ず付ける（ストリームの途中からでも、PacketSink の先でもデコードできるように）。
- AAC エンコーダは入力の時刻に穴があったら無音で埋める（USB マイクの取りこぼしで音の時間軸が縮まないように）。

## 確認済み

Windows（ビルド機 192.168.6.14：x64 + NVIDIA、開発機：Mac 上の ARM64 Windows で x64 をエミュレーション）
- `codec_test`：HW エンコード（NVIDIA MFT）PSNR 57.8 dB、SW（Microsoft MFT）45.4 dB、x264 46.2 dB。
  HW デコード（d3d11va）も同じ値。AAC は 440 Hz の成分比 1.000。RGB 変換は元フレームの変換と平均差 0.00。
- 実カメラ（USB 2.0 Camera、720p30 NV12）＋そのマイクで録画→再生：182 フレーム、落ちゼロ、A/V ずれ 0.3 s 以内。
  （このカメラのマイクは VM 経由だと 1/3 しか届かない。無音埋めで時間軸は保たれる）
- `mk_record --input`（ファイル入力をループ）：9 秒で 273 フレーム、落ちゼロ。PacketSink の最初はキーフレーム。
- chicago（415 秒）を mk_mux で MP4 にして最後まで再生：12456 フレーム、A/V 0.25 s 以内。

Linux（WSL1、aarch64、g++ 9.4）
- ビルド通過。codec_test（x264、PSNR 46.1 dB）と疑似デバイスでの録画→再生（90 フレーム、ずれ 36 ms）も通過。
  WSL1 には /dev/video も音のデバイスも X も無い（Parallels 上の ARM Windows では WSL2 が使えない：
  `HCS_E_HYPERV_NOT_INSTALLED`）。実機での確認は別の Linux マシンで。

Linux（WSL2、x86-64、Ubuntu 20.04、g++ 9.3、カーネル 6.18、2026-11-05）
- USB カメラ（Logitech C505）を usbipd-win で WSL2 に渡して確認（`usbipd bind --busid <id> --force`
  → `usbipd attach --wsl --busid <id>`、WSL 側で `modprobe uvcvideo`）。
- `mk_devices --grab`：/dev/video0 を見つけ、1280x720 MJPEG で取り込み通過（33 フレーム）。
- `mk_studio`：WSLg の X11 窓にカメラのライブ映像（MJPEG 展開→BGRA→XPutImage）、約 15 fps。
- 分かったこと
  - fps は 11〜15 程度。C505 の `exposure_dynamic_framerate=1`（暗いと fps を落とす）を 0 にすると 11.5→15.4 fps。
  - YUYV は USB/IP 経由だとフレームが来ない（320x240 でも）。MJPEG を優先するので影響なし。
  - WSLg の X サーバには組み込みフォントしか無く、ボタンの日本語が文字化けする（直すなら Xft）。
- 音声：C505 のマイクは WSL2 の ALSA に直接見える（`modprobe snd-usb-audio`）。スピーカーは WSLg の
  PulseAudio へ（`libasound2-plugins`）。`~/.asoundrc` で default を振り分ける：
  `pcm.!default { type asym; playback.pcm "pulse"; capture.pcm "plughw:Webcam" }`
  - `mk_devices --grab`：マイク 4 秒で 191040 サンプル（48 kHz、99.5%）。
  - `player_test`（chicago の先頭 15 秒）：ALL OK（15.2 s、最大ずれ 254〜284 ms）。初回だけ 19.7 s かかって FAIL
    （WSLg の音声出力の立ち上がり待ちと思われる、未確認）。
  - `audio_loopback_test`（新規）：スピーカー→空気→マイクで 777 Hz を検出、+46 dB、3.00 s。
    440 Hz だと約 30 dB 弱く、50 Hz の電源ハムの 9 倍音（450 Hz）も混ざるので、既定を 777 Hz にした。
  - デバイス一覧（`snd_device_name_hint`）に出力専用やプラグインもマイクとして並ぶ（取り込みには影響なし）。
  - WSL はしばらく使わないと VM ごと止まり、USB の attach が外れる（`usbipd attach` をやり直す）。
  - FFmpeg が MJPEG のフレームごとに `unable to decode APP fields` / `deprecated pixel format` を出す（実害なし）。

macOS（Mac mini、Apple Silicon、macOS 15.7、Xcode 26.3）
- `codec_test`：VideoToolbox エンコード PSNR 49.9 dB、HW デコード（videotoolbox）も同じ。x264 は 46.1 dB。
  AAC は 440 Hz の成分比 1.000、RGB 変換の平均差 0.42（x264 では 0.02）。
- 実カメラ（USB 2.0 Camera）の全モードを要求どおりに開けた：320x240/640x480/800x600@20/1280x720/
  1920x1080 とも実測 29.5〜29.8 fps、720p YUY2 はカメラ側の上限どおり 9.8 fps。
- そのマイクで 4 秒に 192000 サンプル（48 kHz ちょうど、落ちゼロ）。
- 録画→再生：182 フレーム、落ちゼロ、A/V は一定の 0.29 s 遅れ（音の立ち上がり分）でずれていかない。
  `--input` でのファイル入力、`--h264` の PacketSink、mk_mux もすべて通過。
- ウィンドウは AppKit。ボタン・ステータス・映像の描画をアプリ自身で PNG に焼いて確認した
  （画面収録の許可が無いと `screencapture` は壁紙しか返さないので、外からのスクショでは確認できない）。

Android（A301KC、arm64、Android 13、MediaTek。NDK r27 / API 26 でビルド）
- `codec_test --hwdec`：MediaCodec のエンコードもデコードも通過、PSNR 47.4 dB、440 Hz 成分比 1.000、
  RGB 変換の平均差 0.02。x264 ビルドでも 46.1 dB で通過。
- 実カメラ（背面 1280x720）＋マイクで 8 秒録画→再生：239 フレーム、落ちゼロ、A/V ずれ 195 ms。
  マイクは 4 秒で 188160/192000 サンプル。`--input`、`--h264` の PacketSink、mk_mux も通過。
- エミュレータ（API 36、arm64）でも疑似デバイスと virtualscene カメラで同じところまで通過。
- ui は NativeActivity + ANativeWindow。`tools/build_apk.sh` で Java の無い APK を作って実機に入れ、
  プレビュー → 録画（MediaCodec、181 フレーム・落ち 0）→ 保存 → 再生（0:40）→ カメラに戻る、まで
  画面を見て確認した。BACK で main() が返り、カメラも解放される。

iOS（iPhone 16 実機 + シミュレータ、arm64）
- io は macOS と同じ `io/apple/mk_io_apple.mm`（`AVAudioSession` の設定だけ iOS のときに入る）、
  ui は `ui/ios/mk_ui_ios.mm`。実機 SDK とシミュレータ SDK のどちらでもビルドが通る。
- `codec_test` も `codec_test --hwdec` も通過：VideoToolbox のエンコード（PSNR 45.9 dB）とデコード、
  AAC 440 Hz の成分比 1.000、RGB 変換の平均差 0.23。`mk_mux` も通過。
  コンソールのテストは `xcrun simctl spawn <udid> <bundle>/codec_test` でそのまま動く。
- `mk_studio --test` を入れて画面で確認：UIKit の窓、疑似カメラのプレビュー 30 fps、ボタンとステータス、
  録画（h264_videotoolbox）→ Documents に保存、まで動いた。マイクの許可ダイアログも出る
  （`simctl privacy grant microphone` はこのランタイムでは効かないので、手で押す必要がある）。
- ただし **シミュレータの VideoToolbox はソフトウェアで、720p30 の実時間エンコードに追いつかない**。
  疑似カメラから 30 fps を流すと大半のフレームが落ちて `player_test` が落ちる。640x360 の `codec_test` は
  実時間ではないので全部通る。ここは実機で測り直すこと。
- **2026-11-05、実機で動いた。** `tools/build_ios_app.sh mk_studio` で iPhone 16（yomei12、iOS 26.6.2）に
  入れて、プレビュー・録画・再生・一時停止まで画面で確認した。署名は無料の Personal Team
  （`isFreeProvisioningTeam`、プロファイルの有効期間は 7 日なので切れたら流し直す）。
- 初回だけ iPhone 側で「設定 → 一般 → VPN とデバイス管理 → デベロッパ APP」からデベロッパを
  信頼する操作が要る。これをしないと起動が
  `its profile has not been explicitly trusted by the user` で拒否される（インストールは通る）。
- ここに至るまでに潰したもの（同じ所で止まったら思い出すこと）：
  - **USB で端末を掴めなかった**（`ioreg -p IOUSB` に出るのにインタフェースが生えない）。のちに解消し、
    今は `pairingState: paired` / `tunnelState: connected`。
  - iPhone 側の**デベロッパモード**が off。解消。
  - **Xcode と iOS のバージョン差**（Xcode 26.3 の DDI は iOS 26.3.1 まで、iPhone は 26.6.2）。
    macOS 27.2 / Xcode 27.2 に上げて解消。ただし古い Xcode が
    `/Library/Developer/DeveloperDiskImages/iOS_DDI`（17C52）を置き去りにしていて、新しい Xcode を
    入れてもそちらが選ばれ続けるので、`/Library/Developer/` の外に退かす必要があった
    （退かした残骸は `/Library/Developer/iOS_DDI.old`。古くて使えないので消してよい）。
  - **この Mac ではディスクイメージをマウントできなかった**。構成プロファイル
    `/Library/Managed Preferences/com.apple.systemuiserver.plist` の `mount-controls` が
    `disk-image` も含め全部空の配列＝許可操作ゼロで、`hdiutil attach` が
    `SUIS premount dissented`（DissenterPID = SystemUIServer）で蹴られる。sudo でも同じ
    （権限ではなくポリシーによる拒否）。DDI は DMG なので、端末登録 ← 端末の開発準備 ←
    iPhone への DDI マウント ← Mac 側での DDI 展開 ← DMG のマウント、の一番下が塞がっていた。
    情シスに解除してもらって解決。解除後は `xcrun devicectl manage ddis update` で
    `isUsable: true` になる。
  - **`xcodebuild` が端末を Team に登録してくれなかった**（`Your team has no devices from which to
    generate a provisioning profile` がずっと出ていた）。原因は二つ。`-target` 指定だと
    xcodebuild は `-destination` を捨てる（`Ignoring provided run destination because no scheme was
    passed`）ので、どの端末を登録すべきか分からない。CMake に `CMAKE_XCODE_GENERATE_SCHEME=ON` で
    スキームを作らせて `-scheme` で渡すようにした。もう一つ、新しい端末の登録には
    `-allowProvisioningUpdates` だけでは足りず **`-allowProvisioningDeviceRegistration`** が要る。
  - **バンドル ID の不一致**で installd に `0xe8008016 invalid entitlements` で弾かれた。
    Info.plist が `org.mediakit.mk_studio`、署名した App ID が `org.mediakit.mkstudio` だった。
    App ID に使える文字は英数字と `-` `.` だけでアンダースコアが使えないので、CMakeLists 側で
    `string(REPLACE "_" "" bundle_id ${prog})` して両方を `mkstudio` に揃えた。

ブラウザ（wasm、Emscripten 6.0.13、2026-11-06）
- io は `io/wasm/mk_io_wasm.cpp`（カメラだけ：getUserMedia）、ui は `ui/wasm/mk_ui_wasm.cpp`（canvas + HTML のボタン）。
  スレッドは使わない。`Camera::open()` は許可ダイアログの Promise を EM_ASYNC_JS で待ち、`Window::run()` は
  `emscripten_sleep` で回す（どちらも ASYNCIFY）。こうするとサンプルのソースは他の OS と同じ形のままで済む。
- フレームは JS の setTimeout が fps どおりに `<video>` → canvas → getImageData で取り、エクスポートした
  `mk_wasm_camera_frame` を呼ぶ（BGRA に並べ替えてコールバックへ）。遅れたら溜めずに次の刻みへ飛ぶ。
- ボタンのクリックは JS のキューに積み、`run()` の中で C++ のコールバックを呼ぶ（JS から直接呼ぶと、
  クリックの処理の中で ASYNCIFY の待ち（カメラを開くなど）ができない）。
- `mk_vision`（新しいサンプル、全 OS 共通）を Edge のヘッドレス + 疑似カメラで自動テスト（puppeteer-core）：
  `--test` 2.0 fps、疑似カメラ 2.0 fps、`--fps 4` で 4.0 fps、停止→開始、プレート風の Y4M を
  `--use-file-for-fake-video-capture` で流すと候補の枠が出る。解析は 1280x720 で 0.5 ms 程度。
  `mk_vision.wasm` は 270 KB（codec をリンクしないので小さい）。
- codec も wasm で通る：`mk_mux`（chicago 415 秒を 3.7 秒、30356 パケット）、OpenH264 入りの `codec_test`
  （PSNR 48.0 dB、AAC 440 Hz 成分比 1.000、RGB 変換の平均差 0.26）を node で。wasm は 13 MB。
- 同じ変更で Windows（MSVC、`mk_vision --test` を画面で確認）と Linux（WSL2）もビルド。
- 実カメラでの確認はまだ（この PC の C505 は usbipd に渡してあり、Windows のブラウザからは見えない）。

OpenH264 2.6.0（SW エンコーダ、BSD 2-clause）
- `codec/openh264/` に平坦化して同梱。エンコーダだけ（common + encoder/core + encoder/plus + processing）で
  166 ファイル、decoder は入れていない（FFmpeg 側のデコーダで足りるので）。アセンブラ（`.S` / `.asm`）は
  x264 や FFmpeg と同じく一つも持ってきていない＝全部 C++ で動く。
- 元の `#include` はほぼ全部ベースネーム指定だったので、平坦化しても名前の衝突は無く書き換え不要だった。
  直したのは processing の `#include "../denoise/denoise.h"` 系と `#include <wels/codec_api.h>` だけ。
- `version_gen.h` は本来 git describe から生成されるので、2.6.0 固定の中身を一度だけ書いて同梱した
  （`GENERATED_VERSION_HEADER` を付けないと「openh264 default: 1.4」と名乗ってしまう）。
- `-DMK_WITH_OPENH264=ON` で `mk_openh264` と FFmpeg の `libopenh264.c` / `libopenh264enc.c` が入る。
  x264 と違って BSD なので `CONFIG_GPL` は 0 のまま＝LGPL ビルドで SW エンコーダが使える。
- macOS で `codec_test --encoder libopenh264` 通過：PSNR 48.0 dB（同じテストで x264 46.1 dB、
  VideoToolbox 49.9 dB）。`libavcodec/libopenh264enc.c` は平坦化で `internal.h` を指したままだった（＝
  一度もビルドされていなかった）ので `internal_avcodec.h` に直した。

FFmpeg の設定ヘッダ（`ffmpeg_gen/config_ffmpeg.h`）の作り直し
- 元は MSVC + x64 のマシンで `configure` が吐いた答えがそのまま入っていて、他の OS は後から
  `#ifdef` を足して継ぎ当てていた。アーキテクチャまわりをコンパイラの定義済みマクロから
  決めるように書き換えて、どの OS でビルドしても自分のことを正しく言うようにした。
- `ARCH_X86` / `ARCH_X86_32` / `ARCH_X86_64` は `__x86_64__` / `_M_X64` / `__i386__` から決める。
  以前は「`_WIN32` なら x86-64、それ以外は x86 ではない」だったので、Linux の x86-64 では
  ARCH_X86 が 0、Windows on ARM では 1 という嘘になっていた。
- `ARCH_AARCH64` / `ARCH_ARM` は ARM 向けでも 0 のまま（理由はヘッダのコメントに書いた）。
  平坦化した木に `.S` が無く、aarch64 の `*_init.c` も `ffmpeg.cmake` に入っていないので、
  名乗ると汎用の dsp から存在しない初期化関数を呼んでしまう。x86 の方は元の作者が
  `ff_*_init_x86()` の呼び出しを全部コメントアウト済みなので、正直に 1 で問題ない。
- `HAVE_MMX` / `HAVE_SSE*` / `HAVE_AVX*` などは**全ターゲットで 0** にした。これは
  「このビルドがその命令を吐けるか」を表す値で、`HAVE_X86ASM` も `HAVE_INLINE_ASM` も 0、
  x86 の `.c` も一つもビルドしていない以上、答えは 0 しかない。読んでいるのは
  libswscale のフィルタ整列の調整と警告、`rectangle.h` の `av_assert2` の整列定数だけ。
- `HAVE_RDTSC` は MSVC のときだけ 1。`libavutil/x86_timer.h` のその分岐は `<intrin.h>` と
  `__rdtsc()`、つまり MSVC の書き方で、clang / gcc の x86 ビルドはこれで即死する
  （実際 ARCH_X86 を正しくした途端に macOS の x86-64 ビルドが落ちて見つかった）。
- `HAVE_ALIGNED_STACK` / `HAVE_FAST_64BIT` は 64 ビットかどうかで決める。
  `HAVE_MACH_ABSOLUTE_TIME` は Apple で 1（configure が Mac を見ていなかっただけ）。
  `OS_NAME` / `CC_IDENT` / `FFMPEG_CONFIGURATION` / `SLIBSUF` も OS ごとに。
- 確認：macOS arm64（`codec_test`、`--hwdec`、`--encoder libopenh264`、`mk_mux` 全部通過）、
  **macOS x86-64**（`-DCMAKE_OSX_ARCHITECTURES=x86_64` でビルドして Rosetta で実行、通過）、
  Android arm64 と iOS シミュレータ（ビルド）。x86-64 を clang でビルドできたので、Linux x86-64 も通るはず。
  Windows（x64、MSVC）と Linux（WSL1 aarch64、g++ 9.4）でも 2026-11-04 にクリーンビルドして確認済み：
  Windows は既定と OpenH264+x264 の両方、Linux は OpenH264+x264 でビルドでき、codec_test（h264_mf HW 57.8 dB・SW 45.4 dB、d3d11va、
  OpenH264 48.0 dB、x264 46.2 dB）、疑似デバイスでの録画→再生、chicago の mk_mux が通る。

## 落とし穴

- EM_JS / EM_ASYNC_JS の本体は C の文字列になる：JS の `''` は空の文字定数として警告され、正規表現の `\\` は
  `\` に減って壊れる。空文字列は `""`、バックスラッシュは使わない。
- 生成済みの `ffmpeg_gen/demuxer_list.c` は配列の型が古い `AVInputFormat` だった（中身は `FFInputFormat`）。
  MSVC / gcc は警告だけだが、Emscripten の clang はエラーにする。`FFInputFormat` に直した。
- Emscripten は `__linux__` を定義しない。`config_ffmpeg.h` の POSIX 系（unistd・mmap・posix_memalign・usleep・
  pthreads）は `__EMSCRIPTEN__` も足した。pthread はスタブなので、FFmpeg はスレッド数 1（CPU 数が 1 と出る）で動く。
- `VideoFrame` は data[] が自分の buffer を指す。コピー・ムーブでポインタを付け替える。
- FFmpeg の H.264 デコーダはフレーム並列だとスレッド数ぶん溜めて一気に出す。スライス並列 + LOW_DELAY にしてある。
- Player：表示スレッドは先頭フレームの時刻まで「眠る」（ロックを持って空回りするとスピーカーのスレッドが詰まり、
  音の時計が遅れた）。音声が 350 ms を切ったら映像キューを最大 64 まで溜めてでも読み進める（ファイル内で映像が
  音声より先行していると音が枯れた）。`Speaker::queued_us()` は無音の詰め物を数えない。
- 平坦化した FFmpeg は libavfilter / libavdevice のヘッダを参照するファイルがある（ソースは無いがヘッダは置いてある）。
  `compat/win32` は Windows のときだけ include パスに入れる（Linux では system の stdatomic.h を使う）。
- ビルド機へは tar で送る。展開時は `tar xzmf`（時計がずれていて ninja が再ビルドしないことがあった）。
  cmd は引数を `=` で切るので、cmake のオプションは環境変数 MKOPTS で渡す（`tools/build_win.bat`）。
- macOS：`AVCaptureSession` は自分のプリセットをデバイスに当てるので、`activeFormat` は
  **`startRunning` の後に**設定しないと捨てられる（このカメラは 1920x1080 の 5 fps に落ちていた）。
  一度効けば stop / start しても残るが、セッションは open() から close() まで回しっぱなしにして、
  start() / stop() は配送の開閉だけにしてある。
- macOS：古い DAL 経路のカメラは `activeVideoMinFrameDuration` に
  **レンジが持っている CMTime そのもの**しか受け付けず、等価な有理数（1/30）でも NSException を投げる。
  投げると握りつぶしてレンジの値で入れ直す。`activeVideoMaxFrameDuration` も同じ値にしないと、
  自動露出が暗いところで勝手にフレームレートを落とす。
- macOS：カメラ・マイクの用途説明が無いとプロセスごと殺される。バンドルの無い実行ファイルには
  `-sectcreate __TEXT __info_plist` で `tools/mac_info.plist` を埋め込む。
- 平坦化で `libavutil/time.h` は `time_avutil.h` になったのに 16 個のファイルが `#include "time.h"` のままで、
  システムのヘッダを拾っていた。gcc は暗黙宣言の警告で通してしまい、`av_gettime()` の int64_t が
  int に切り詰められていた（clang は拒否する）。全部直した。
- Android：**Camera2 の結果は binder で返ってくる**。Java のあるアプリなら binder のスレッドプールは
  既に回っているが、ネイティブだけのプロセスでは誰も起動しないので、カメラは開けて
  `setRepeatingRequest` も成功するのに 1 フレームも来ない。`io` が `libbinder_ndk.so` を dlopen して
  `ABinderProcess_startThreadPool()` を呼んでいる。mk_record だけ動いて mk_devices が動かなかったのは、
  mk_record が MediaCodec / AAudio を先に触っていて、そちらがプールを起こしていたから。
- Android：`AV_CODEC_FLAG_GLOBAL_HEADER` を付けると FFmpeg が MediaCodec エンコーダの前に
  `extract_extradata` の bsf を挟み、**パケットが 1 つも出てこなくなる**。MediaCodec は codec-config を
  最初のパケットの頭に付けてくるので、この OS だけ GLOBAL_HEADER を付けない。
- Android：MediaCodec のデコーダは開く前に解像度と SPS/PPS が要る。`VideoDecoder` は SW で開いておいて、
  最初のキーフレームが来たところで差し替える。H.264 パーサは `PARSER_FLAG_COMPLETE_FRAMES` を立てないと
  次のアクセスユニットが来るまで解像度を返さない。
- Android：FFmpeg の `mediacodecdec_common.c` は、最初の出力フォーマットに入っていた crop 矩形を
  保持したまま二度と読み直さない（`AMEDIAFORMAT_GET_INT32` はキーが無いと値を変えず、NDK の "crop" は
  crop_right が 0 のときしか読まれない）。この端末は最初に出鱈目な crop(0,0,318,238) を返してくるので、
  640x360 が 319x239 になっていた。毎回クリアするように直してある。
- 平坦化ツリーにはアセンブリが 1 つも入っていないので、`ARCH_AARCH64` は 0（C だけでビルドする）。
  1 にすると aarch64 の *_init.c が NEON のシンボルを参照してリンクが通らない。
- エンコーダが EAGAIN を返したときは、ドレインしながら何度か出し直す。1 回しか再試行しない実装では
  MediaCodec の深いパイプラインでフレームの 3 分の 1 を黙って捨てていた。
- Android：binder のスレッドプールは `ABinderProcess_startThreadPool()` だけ呼ぶ。
  `setThreadPoolMaxThreadCount()` は、既に大きい上限で動いているプロセス（＝普通のアプリ）だと
  「cannot be shrunk after starting」で abort する。
- iOS：UIKit は `main()` とメインスレッドを譲らない。`ui` が `main()` を持って `UIApplicationMain` を回し、
  プログラムの `main()` は別スレッドで走らせる（`mk_ui.h` が iOS のときだけ `main` を `mk_ios_main` に
  付け替える）。UIKit に触る所は全部メインキューに渡す。`Window::run()` は close されるまで眠るだけ。
- Android の ui：NDK には文字を描く手段が無いので、ボタンとステータスだけ JNI で
  `android.graphics.Paint` に描かせて、出てきた ARGB を自前の画面に合成している。
  ファイル選択のダイアログも無いので、保存はアプリの外部ファイル領域、読み込みはそこの最新ファイル。

## 次にやること

1. Linux：V4L2 MJPEG・ALSA・X11 窓は WSL2 で確認済み。残りは X11 の日本語（Xft）。Linux の HW エンコード（VA-API）
2. wasm：実カメラで確認（スマホのブラウザも）、マイク・スピーカー（WebAudio）、重い解析は Web Worker に逃がす
3. iOS 実機：動くようになったので、あとは数字を取る。シミュレータの VideoToolbox は
   ソフトウェアで 720p30 の実時間エンコードに追いつかなかったので、実機で
   プレビューの fps・落ちフレーム・エンコーダ名を `mk_studio` のステータス行で測り直す。
   `player_test` / `codec_test` も実機のバンドルで回す
