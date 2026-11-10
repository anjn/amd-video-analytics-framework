# 1440p・17入力デモを別の環境に移す

`artifacts/1440p-demo`のモデルや録画をGitへ追加する必要はありません。このディレクトリには、同じデモを組み立てる設定テンプレート、初期画面配置、動画の順序、COCOラベル、起動・移行スクリプトを置きます。環境固有の設定と大きなファイルは`artifacts/`に生成します。Python 3.11以上の標準ライブラリだけで設定とファイルの検証を実行できます。

テンプレートは16本の1440p動画とリソース画面の計17入力、16タイル、YOLO／ReID 3Hz、Embedding間隔3000msです。`cam15`は非表示にし、代わりにリソース画面を表示します。生成後の設定はNVRの画面から変更できます。

## Gitで管理するものと、別途用意するもの

| Git管理するファイル | 用途 |
| --- | --- |
| `config.template.json` | カメラ名、選択状態、頻度、録画・検索の設定 |
| `layout.template.json` | 初回のタイル配置とストリーム情報の表示設定 |
| `model-info.json`、`coco80.txt` | YOLO11n 640×384の入出力情報とラベル |
| `videos.template.txt` | 現在のM01〜M16のファイル名と順序 |
| `manage.py`、`run.sh`、`test_manage.py` | 搬出、設定生成、検証、起動、回帰テスト |

別途持ち運ぶファイルは次のとおりです。`export`コマンドが実際に必要なファイルだけをコピーし、`assets.json`へ相対パス・サイズ・SHA256を記録します。

- YOLOのバッチ1／4、ReIDのバッチ4の`.mxr`と、それぞれの`.manifest.json`。
- MobileCLIPの`image.onnx`、`text.onnx`、`tokenizer.json`、`manifest.json`。
- GPU用の`gpu-device.json`、バッチ1／4の`gpu-device-b*.mxr`とサイドカー。準備済みの場合に含めます。
- NPU用の`npu.json`、`npu-config.json`、対象キーの`npu-cache/`一式。準備済みの場合に含めます。
- 16本の動画。動画は既定ではコピーせず、`--include-videos`を指定した場合だけ含めます。

異なるGPUでYOLO／OSNetを再コンパイルする場合は、元のONNXまたは重みも別途移してください。`export`はこの2モデルの原本を自動収集しません。MobileCLIPのONNXは搬出対象に含まれます。

旧比較用の`gpu-b*.mxr`、`npu-work/`、ログ、録画、SQLite DB、`.deps/`、Python venv、旧`libmobileclip2.so`、EmbeddingGemma／TorchはこのMobileCLIPデモの搬出対象ではありません。Ryzen AI環境とフレームワークは移行先に用意します。EmbeddingGemmaを使うデモの搬出には、このプロファイルは対応していません。

## 1. 現在の環境でモデルを搬出する

リポジトリのルートで実行します。稼働中の設定・モデルは読み取るだけで変更しません。搬出先は新しいディレクトリを指定します。

```bash
python3 examples/nvr/demo/1440p/manage.py export \
  --config artifacts/1440p-demo/config.json \
  --videos-list artifacts/1440p-demo/videos.txt \
  --output artifacts/1440p-portable-assets
```

動画も含める場合は`--include-videos`を追加します。含めない場合は、移行先の動画ディレクトリに同じファイル名の16本を置きます。実際に使った順序は`assets.json`にも記録されます。

```text
1440p-portable-assets/
  assets.json
  detector-b1.mxr             # 各mxrには.manifest.jsonも付属
  detector-b4.mxr
  reid-b4.mxr
  mobileclip/
    manifest.json, image.onnx, text.onnx, tokenizer.json
    gpu-device.json, gpu-device-b*.mxr, ...
    npu.json, npu-config.json, npu-cache/<key>/...
  videos/                     # --include-videosを指定した場合
```

このディレクトリを`rsync -a`やアーカイブで転送します。アプリ本体は同じGitコミットをチェックアウトし、[インストール手順](../../../../docs/guide/install.ja.md)に従ってビルドします。未コミットのアプリ変更はcloneでは移らないため、移行前に必要な変更をコミットしてください。

## 2. 移行先に実行環境を用意する

検証環境はUbuntu 24.04、gfx1150、ROCm 7.2.0／MIGraphX 2.15.0です。コンパイル済み`.mxr`はGPUアーキテクチャと実行ライブラリに依存します。異なるGPU／ROCmでは、元のONNXや重みから移行先で再コンパイルします。元モデルは[AIモデルの準備](../../../../docs/dev/nvr/nvr-ai-performance.ja.md)も参照してください。Python venvをそのままコピーして使う構成にはしません。

[MobileCLIPの準備手順](../../../mobileclip/README.ja.md)に従い、移行先のRyzen AI環境でビルド機能を有効にします。既にパイプラインが動いている場合は停止してからビルドします。

```bash
source scripts/env.sh
source examples/nvr/scripts/mobileclip-env.sh /path/to/target-ryzen-ai-venv
cmake -S . -B build -DAMDVA_BUILD_MOBILECLIP=ON -DAMDVA_BUILD_NPU=ON \
  -DAMDVA_ORT_ROOT="$AMDVA_ORT_ROOT" -DAMDVA_HARDWARE_TESTS=ON
cmake --build build -j8
```

### NPUキャッシュは最終配置で再準備する

現在のキャッシュの`context.json`には、準備したPCの`/home/...`という絶対パスも含まれています。外側のconfigだけを書き換えてNPUが動くとは限りません。SDK、ORTバージョン、ドライバー、NPUと配置先を合わせて、移行先でキャッシュを再準備・検証します。実行時にCPUへ自動切り替えはしません。

準備ツールは別リポジトリ`https://github.com/anjn/embeddinggemma2-rocm`のコミット`102a9df65cd312529668daabb3e88e0a17cfc363`にある`embeddinggemma2_rocm.mobileclip2_prepare`です。このモジュールの依存環境を用意し、コピーしたキャッシュは退避してから新規に作成します。稼働中のバンドルでは実行しません。

```bash
bundle=/absolute/final/path/1440p-portable-assets/mobileclip
mv "$bundle/npu-cache" "$bundle/npu-cache.from-source"
PYTHONPATH=/path/to/embeddinggemma2-rocm/python \
  python -m embeddinggemma2_rocm.mobileclip2_prepare npu --bundle "$bundle"
python3 examples/nvr/demo/1440p/manage.py refresh \
  --assets /absolute/final/path/1440p-portable-assets
```

GPUのMobileCLIPプロファイルを再作成する場合は`python3 scripts/prepare_mobileclip_device.py "$bundle"`を使い、その後に同じ`refresh`を実行します。`refresh`はモデル側のマニフェストも照合するため、壊れた`.mxr`を単に再承認する操作にはなりません。キャッシュのJSONを手作業で書き換える方式は採りません。

## 3. ローカル設定を生成して起動する

```bash
python3 examples/nvr/demo/1440p/manage.py configure \
  --assets /absolute/final/path/1440p-portable-assets \
  --videos /path/to/videos \
  --venv /path/to/target-ryzen-ai-venv \
  --backend npu \
  --output artifacts/1440p-demo-local

examples/nvr/demo/1440p/run.sh --check artifacts/1440p-demo-local
examples/nvr/demo/1440p/run.sh artifacts/1440p-demo-local
```

動画を同梱した場合は`--videos /absolute/final/path/1440p-portable-assets/videos`です。バックエンドは`gpu`、`npu`、`gpu+npu`から、搬出済みのものを明示的に選びます。ポートを変える場合は`configure`に`--rtsp-port 18670 --dashboard-port 18672`を指定します。CPUの表示名は自動検出を使い、変更する場合だけ`--cpu-name`を指定します。

`--check`はファイル・チェックサム・実行ファイルの存在を確認するもので、GPU／NPUでの推論の成功を保証するものではありません。移行先でデモを起動する前に、他のパイプラインを止めて実行検証します。

```bash
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/examples/mobileclip/amdva-mobileclip-interop "$bundle" npu 100
```

`run.sh`は16動画のRTSPサーバー、リソース画面、NVRを起動し、終了時には自分が起動したプロセスを終了します。使用済みポートがあると、既存のデモに接続する前に停止します。`--windowed`ならウィンドウ表示です。ログは生成先の`logs/`へ保存します。移行先のデスクトップに応じて`QT_QPA_PLATFORM`、`WAYLAND_DISPLAY`などを指定してください。

生成先には`config.json`、`config.json.ui.json`、`model-info.json`、`coco80.txt`、`videos.txt`、`demo.json`ができます。モデル・動画のパスは生成先からの相対パスで、venvだけ移行先の絶対パスを記録します。再生成時は既存configを上書きしません。意図的に戻す場合だけ`--force`を使い、その場合も既存の画面配置は保持します。

## 録画・検索履歴も移す場合

既定では新しい`recordings/`を使います。履歴も移す場合は、元のNVRを正常終了してから録画ルートをDBごと丸ごとコピーし、生成先configの`recording_root`を合わせます。DBだけのコピーや、稼働中のDBのコピーは避けます。モデル／NPUキャッシュの再準備で検索の名前空間が変わる場合があり、その場合は古いEmbeddingを新しい設定からそのまま検索できません。自動の再Embeddingはありません。

## ファイル移行のテスト

```bash
python3 examples/nvr/demo/1440p/test_manage.py -v
```

小さいダミーファイルで、移動後の相対パス、空白を含むパス、16動画の順序、改変の検出、設定の上書き防止、画面配置の保持、再コンパイル後のマニフェスト更新を確認します。GPU／NPUや稼働中のデモを使わずに実行できます。CMakeでは`demo-portability-contract`として登録します。
