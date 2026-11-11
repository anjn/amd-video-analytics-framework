<!-- source: docs/dev/host-build.md 9472a651202aa7568312af2aa2aa7e7d8e4d20cc -->
# ホストでのビルドと参照パイプライン

短い手順は[導入](../guide/install.ja.md)と[クイックスタート](../guide/quickstart.ja.md)にあります。

## ホストでビルドする

Ubuntu 24.04／26.04上にROCm **10.1.0**とMIGraphX **2.18.0**を事前導入します。SDKルートは別々に指定できます。GStreamerとパッチはframework側、Mesa／libva／libdrm、QtとOSの開発パッケージはホスト側で管理します。旧 `.deps/rocm72` と、他のOSから取得した `.deps/sysroot` は使いません。

[導入手順](../guide/install.ja.md)に従い `python3 scripts/host.py --write`、`python3 scripts/bootstrap.py --jobs 8` を実行します。NPUでは準備済みの `--ryzen-ai-venv` を指定します。bootstrapはsudoやホストSDKの導入を実行しません。`scripts/env.sh` は記録したホストSDKとframeworkのprefixを参照します。

検証済みと未検証の構成は[対応表](support-matrix.ja.md)を参照してください。モデルmanifestはschema 3で、GPU、ROCm、MIGraphX、元ONNXのハッシュ、精度、バッチと生成物のハッシュを記録します。旧schema 1／2は再生成が必要です。SDK間でMIGraphXのシリアライズ済みモデルの互換性を仮定しません。

## モデルを準備して実行する

RGB入力 `[1,3,640,640]` と出力 `[1,84,8400]` の静的YOLOv8 COCO ONNXを用意します。モデル情報は[例](../../examples/yolov8n.json)に従い、80行のラベルをJSONの隣に置きます。

```bash
source scripts/env.sh
mkdir -p artifacts
build/src/gst/amdva-compile-model /path/yolov8n.onnx artifacts/yolov8n.mxr \
  > artifacts/model-compile.log 2>&1
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
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
