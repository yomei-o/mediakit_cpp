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
- エンコーダは OS のもの（Windows: h264_mf、Android: h264_mediacodec、Apple: h264_videotoolbox、
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
- ビルドのみ。WSL1 には /dev/video も音のデバイスも X も無い（Parallels 上の ARM Windows では WSL2 が使えない：
  `HCS_E_HYPERV_NOT_INSTALLED`）。実機での確認は別の Linux マシンで。

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

## 次にやること

1. Linux 実機：V4L2（YUYV / MJPEG）、ALSA、X11 窓の確認（家の環境で）。Linux の HW エンコード（VA-API）
2. macOS / iOS（AVFoundation + VideoToolbox、ui は AppKit / UIKit）— Mac 側で
3. Android（NDK Camera2 + AAudio + MediaCodec、ui は SurfaceView/ANativeWindow）
4. FFmpeg の設定ヘッダ（`ffmpeg_gen/config_*.h`）を OS ごとに見直す（今は Windows x64 用 + `#ifdef` 分岐）
5. OpenH264（BSD）の SW エンコーダを LGPL ビルド用に
