# 作業メモ（続きをやる人向け）

## 経緯

元は `SafieGoproProxy_reenc_002.zip`（Android + Windows の GoPro → RTSP → 再エンコード → Safie 送信）。
Safie 接続部分（spyderClientCoreLib、curl、libressl、msgpack、libuv、rtsp、device/）は全部捨てた。
使ったのは平坦化済みの FFmpeg 7.1.1（`codec/ffmpeg/`）と x264（`codec/x264/`、任意）だけ。
Safie への出力は公開できないので入れない。`PacketSink` を実装すれば外から（非公開の別リポで）いつでもつなげられる。

## 方針

- 3 層：`io/`（OS ごと）、`codec/`（全 OS 共通）、`ui/`（OS ごと）。`pipeline/` は io + codec で UI に依存しない。
- 映像の受け渡しは NV12 が基本、表示は BGRA。音は s16 インタリーブ。圧縮映像は H.264 Annex-B、音は生 AAC。
- MJPEG しか出さないカメラ（Linux の UVC など）は io が MJPEG のまま渡し、codec の FrameConverter が展開する
  （io を codec に依存させないため）。
- 時刻は全部 `mk::now_us()`（steady_clock）。WASAPI の QPC 時刻、MF のデバイス時刻、V4L2 の MONOTONIC はここに換算。
- エンコーダは OS のもの（Windows: h264_mf、macOS/iOS: h264_videotoolbox、Android: h264_mediacodec（未）、
  Linux: h264_vaapi / h264_v4l2m2m（未ビルド））、SW は x264（GPL、任意）か OpenH264（未）。
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

## 落とし穴

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

## 次にやること

1. Android（NDK Camera2 + AAudio + MediaCodec、ui は SurfaceView/ANativeWindow）
2. Linux 実機：V4L2（YUYV / MJPEG）、ALSA、X11 窓の確認（家の環境で）。Linux の HW エンコード（VA-API）
3. iOS（io は macOS 版とほぼ同じ AVFoundation + AudioQueue のはず、ui だけ UIKit）
4. FFmpeg の設定ヘッダ（`ffmpeg_gen/config_*.h`）を OS ごとに見直す（今は Windows x64 用 + `#ifdef` 分岐）
5. OpenH264（BSD）の SW エンコーダを LGPL ビルド用に
6. `testdata/chicago.h264` が `.gitignore` の `*.h264` に引っかかって repo に入っていない
   （README と mk_mux の例はこれを指している）。`!testdata/*.h264` を足すか、例を直すか
