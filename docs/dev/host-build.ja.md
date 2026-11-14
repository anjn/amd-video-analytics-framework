<!-- source: docs/dev/host-build.md 9f7960a76c8391e4d19602c503d363e3532a842e -->
# ホストでのビルドと参照パイプライン

短い手順は[導入](../guide/install.ja.md)と[クイックスタート](../guide/quickstart.ja.md)にあります。

## ホストでビルドする

Ubuntu 24.04／26.04上にROCm **10.1.0**とMIGraphX **2.18.0**を事前導入します。SDKルートは別々に指定できます。GStreamerとパッチはframework側、Mesa／libva／libdrm、QtとOSの開発パッケージはホスト側で管理します。旧 `.deps/rocm72` と、他のOSから取得した `.deps/sysroot` は使いません。

[導入手順](../guide/install.ja.md)に従い `python3 scripts/host.py --write`、`python3 scripts/bootstrap.py --jobs 8` を実行します。NPUでは準備済みの `--ryzen-ai-venv` を指定します。bootstrapはsudoやホストSDKの導入を実行しません。`scripts/env.sh` は記録したホストSDKとframeworkのprefixを参照します。

検証済みと未検証の構成は[対応表](support-matrix.ja.md)を参照してください。モデルmanifestはschema 3で、GPU、ROCm、MIGraphX、元ONNXのハッシュ、精度、バッチと生成物のハッシュを記録します。旧schema 1／2は再生成が必要です。SDK間でMIGraphXのシリアライズ済みモデルの互換性を仮定しません。



既定のNVR GUIビルドには、ホストOS標準の`qt6-base-private-dev`、
`libwayland-dev`、`wayland-protocols`も必要です。
`scripts/host.py --packages`の一覧に含めています。非公開QPAヘッダーは導入済みQtと
版を合わせ、別UbuntuリリースのQtライブラリをコピーしないでください。
実験中のライブ表示FIFOはWaylandの`wp_presentation`通知とQt標準の描画・MSAA解決・
swapを使います。24.04/gfx1150の比較結果と未検証環境は
[表示経路の調査](nvr/input-decode-latency.md)に記録しています。

## モデルを準備して実行する

RGB入力 `[1,3,640,640]` と出力 `[1,84,8400]` の静的YOLOv8 COCO ONNXを用意します。モデル情報は[例](../../examples/yolov8n.json)に従い、80行のラベルをJSONの隣に置きます。

```bash
source scripts/env.sh
mkdir -p artifacts
build/src/gst/amdva-compile-model /path/yolov8n.onnx artifacts/yolov8n.mxr \
  > artifacts/model-compile.log 2>&1
LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/src/gst/amdva-run /path/input.h264 artifacts/yolov8n.mxr \
  /path/yolov8n.json --events=artifacts/events.jsonl
```

準備は内部FP16演算、`offload_copy=false`を使用し、SHA-256とGPUのmanifestを出力します。欠損・不一致・非互換は読み込み時に拒否します。モデルコンパイルは実行パイプラインとは分離します。

`amdva-run`はフレーム数、検出数、P95／P99処理時間、元メモリ／PTSとコピー監査を報告します。`--amd-only`は診断用にDL Streamerを省きます。監査ライブラリがなければゼロコピー検証はfalseです。`--duration=SECONDS`は30fpsのH.264入力を実時間で繰り返し、SIGINT／SIGTERMはEOSで排出します。

```bash
python3 tools/soak.py /path/input.h264 artifacts/yolov8n.mxr /path/yolov8n.json \
  --seconds=3600 --output=artifacts/soak
```

soakはRSS、FD、GPUメモリを記録し、ウォームアップ後の増加を検査します。DL Streamerとコピー監査の成功が必要です。[検証](validation.ja.md)を参照してください。

録画は圧縮RTSPパケットを保持します。リソースダッシュボードはGPU描画した監視画面をハードウェアH.264で配信します。現在のNPU要素については[要素の説明](elements.ja.md)も参照してください。
