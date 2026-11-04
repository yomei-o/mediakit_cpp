# 作業メモ（続きをやる人向け）

## 経緯

元は `SafieGoproProxy_reenc_002.zip`（Android + Windows の GoPro → RTSP → 再エンコード → Safie 送信）。
Safie 接続部分（spyderClientCoreLib、curl、libressl、msgpack、libuv、rtsp、device/）は全部捨てた。
使ったのは平坦化済みの FFmpeg 7.1.1 だけ（`codec/ffmpeg/`）。Safie への出力は公開できないので入れない。
`PacketSink` を実装すれば外から（非公開の別リポで）いつでもつなげられる。

## 方針

- 3 層：`io/`（OS ごと）、`codec/`（全 OS 共通）、`ui/`（OS ごと）。`pipeline/` は io + codec で UI に依存しない。
- 映像の受け渡しは NV12 が基本、表示は BGRA。音は s16 インタリーブ。圧縮映像は H.264 Annex-B、音は生 AAC。
- 時刻は全部 `mk::now_us()`（steady_clock）。WASAPI の QPC 時刻、MF のデバイス時刻もここに換算。
- エンコーダは OS のもの（Windows: h264_mf、Android: h264_mediacodec、Apple: h264_videotoolbox、
  Linux: h264_vaapi / h264_v4l2m2m、最後に libopenh264）。x264 は GPL なので入れない。
- 各キーフレームの前に SPS/PPS を必ず付ける（ストリームの途中からでも、PacketSink の先でもデコードできるように）。

## 確認済み（Windows、ビルド機 192.168.6.14 は x64 + NVIDIA）

- `codec_test`：HW（NVIDIA H.264 Encoder MFT）で PSNR 57.8 dB、SW（Microsoft H264 Encoder MFT）で 45.4 dB。
  AAC は 440 Hz の成分比 1.000。RGB 変換は元フレームの変換と平均差 0.00。
- `mk_record --test`（疑似カメラ + 疑似マイク）→ `player_test`：全フレーム順番どおり、時刻ずれ最大 0.19 s（音声出力の遅延）。
- `mk_record --input`（ファイル入力をループして再エンコード）：9 秒で 273 フレーム、落ちゼロ。
- PacketSink：最初のパケットがキーフレームであること、生 H.264 ファイルが再生できることを確認。
- 実カメラは未確認（ビルド機にはカメラが無く、開発機の Windows（Mac 上の VM）にはカメラが見えていない。
  Parallels でカメラ共有を有効にすれば見えるはず）。マイク・スピーカーの列挙は確認済み。

## 落とし穴

- `VideoFrame` は data[] が自分の buffer を指す。コピー・ムーブでポインタを付け替える（最初これでデータが壊れていた）。
- FFmpeg の H.264 デコーダはフレーム並列だとスレッド数ぶん溜めて一気に出す（ループの継ぎ目でエンコーダの
  キューがあふれた）。スライス並列 + LOW_DELAY にしてある。
- 平坦化した FFmpeg は libavfilter / libavdevice のヘッダを参照するファイルがある（opus の bufferqueue.h など）。
  ソースは要らないがヘッダは置いてある。
- ビルド機へは tar で送る。展開時は `tar xzmf`（m：時刻を今にする）。時計がずれていて ninja が再ビルドしないことがあった。

## 次にやること

1. HW デコード（Windows: FFmpeg の d3d11va hwaccel を有効化 → NV12 に転送。今は SW のみ）
2. 実カメラでの確認（mk_studio）
3. macOS / iOS（AVFoundation + VideoToolbox、ui は AppKit / UIKit）— Mac 側で
4. Android（NDK Camera2 + AAudio + MediaCodec、ui は SurfaceView/ANativeWindow）
5. Linux（V4L2 + ALSA、ui は X11 か SDL）
6. FFmpeg の設定ヘッダ（`ffmpeg_gen/config_*.h`）を OS ごとに（今は Windows x64 用 + Android の分岐）
