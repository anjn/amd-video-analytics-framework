<!-- source: docs/dev/elements.md 80c69bd8df37f04a26e88c8a50ba054ac210fb19 -->
# 要素リファレンス

同梱のDL Streamer要素と独立した`amdnpu`を除き、すべての要素は `amdanalytics` プラグイン（`libgstamdanalytics.so`）に含まれます。ランクは NONE のため、自動で接続されることはありません。*READY* と記したプロパティは NULL/READY 状態でのみ変更できます。正確な一覧は、お使いのビルドで
`gst-inspect-1.0 <element>` を実行して確認してください。

`GST_PLUGIN_PATH` にプラグインのディレクトリ（`build/plugins` または
`<prefix>/lib/gstreamer-1.0`）を設定してください。

`amdnpu`は独立した任意プラグインです。以下に専用の説明があります。

## 推論: `amddetect`、`amdclassify`、`amdinference`

実装は 1 つで、既定値だけが異なる 3 つの要素です。インプレースで動作し、メタデータを追加するだけで画素は変更しません。

| | `amddetect` | `amdclassify` | `amdinference` |
| --- | --- | --- | --- |
| `inference-region` の既定値 | `full-frame` | `roi-list` | `full-frame` |
| タスク | 検出、インスタンスセグメンテーション、姿勢推定 | 分類 | 任意の `task_type`（セマンティックセグメンテーション、生のテンソルなど） |

パッド（sink と src）: `video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]`
または `multistream/x-analytics-batch(meta:GstAnalyticsBatchMeta)`（`gvastreammux` が生成するコンテナー）。フレームは BT.709 limited range で、幅と高さが偶数である必要があります。

| プロパティ | 型 / 範囲 | 既定値 | 意味 |
| --- | --- | --- | --- |
| `model` | string | | `.manifest.json` を伴うコンパイル済み `.mxr` モデル（READY） |
| `model-info` | string | | [model-info](model-info.ja.md) JSON（READY） |
| `model-instance-id` | string | | ID、モデル、設定が同じ要素は 1 つの実行器を共有し、まとめてバッチ処理します |
| `inference-region` | `full-frame` \| `roi-list` | 上記参照 | `roi-list` では上流の `GstAnalytics` ボックスを最大 300 個まで VPE で切り出します |
| `device` | 0-16 | 0 | HIP デバイス番号 |
| `batch-size` | 1-16 | 1 | コンパイル時のバッチと一致させる必要があります |
| `batch-timeout` | 0-10000 ms | 20 | 不完全なバッチを待つ時間 |
| `inference-interval` | 1-100000 | 1 | N フレームごとに推論します |
| `confidence-threshold` | 0-1 | 0.25 | 物体スコアの最小値 |
| `nms-threshold` | 0-1 | 0.45 | NMS の IoU |
| `require-zero-copy` | bool | true | 画像やテンソルを CPU でコピーする経路をすべて拒否します |

付加するメタデータは、`GstAnalytics` の物体検出、分類、キーポイントグループの mtd と、GPU のマスク/テンソル、逆変換のジオメトリ、フレーム識別情報を保持する `AmdAnalysisMeta` です。キューのあふれはエラーとして扱い、際限なくバッファーリングすることはしません。

## `amdwatermark`

ボックス、ラベル、トラッキング ID、インスタンス/セマンティックのマスク、COCO-17 の骨格を
GPU で描画します。

| | |
| --- | --- |
| Sink | `video/x-raw(memory:VAMemory),format=NV12,width=[2,4096],height=[2,4096]` |
| Src | `video/x-raw(memory:DMABuf),format=BGRA,width=[2,4096],height=[2,4096]`（リニア、エクスポートした VA サーフェス） |
| `device` | HIP デバイス番号（既定値 0、READY） |
| `require-zero-copy` | 既定値 true（READY） |

BGRA サーフェスは EGL（`amdva::EglDmaBufImporter`）で表示するか、`amdencode` でエンコードできます。通常の `vapostproc` ではこのサーフェスをネゴシエーションできません。

## `amdencode`

VA のハードウェアエンコーダーを内包する bin です。[エンコードと配信](encode-and-serve.ja.md)を参照してください。

| | |
| --- | --- |
| Sink | `video/x-raw(memory:VAMemory),format={NV12,BGRA,BGRx,RGBA}` または `video/x-raw(memory:DMABuf),format={NV12,BGRA,BGRx,RGBA}` |
| Src | `video/x-h264` または `video/x-h265` の byte-stream（AU 単位）、`video/x-av1` の OBU/TU |
| `codec` | `h264`（既定値）、`h265`、`av1` |
| `bitrate` | 1-2048000 kbit/s（既定値 4000） |
| `key-int-max` | 1-10000（既定値 30） |
| `b-frames` | 0-4（既定値 0。AV1 では使用不可） |
| `signal-bt709` | 既定値 true。H.264 の SPS VUI を書き換えます |

すべてのプロパティは READY です。内部構成は、DMA-BUF のインポートと変換の段、`vapostproc`、caps フィルター、`vah264enc`/`vah265enc`/`vaav1enc`、パーサーです。

## `amdpython`

メタデータのみを扱うコールバックです。[メタデータと Python](metadata-and-python.ja.md) を参照してください。

| プロパティ | 既定値 | |
| --- | --- | --- |
| `module` | | Python のソースファイル（READY） |
| `function` | `process` | `callback(frame_dict)`（READY） |

caps: 任意（パススルー）。

## `amdmetapublish`

上限付きの非同期送信要素です。caps は任意で、パススルーです。

| プロパティ | 既定値 | |
| --- | --- | --- |
| `method` | `none` | `none`、`file`、`mqtt`、`kafka` |
| `file-path` | | JSON Lines の出力先 |
| `address` | | ブローカーのアドレス |
| `topic` | `amdva/analytics` | |
| `queue-limit` | 256 | 1-65536 レコード |
| `delivered`、`dropped`、`errors` | 読み取り専用 | uint64 のカウンター |

## NPU推論: `amdnpu` { #npu-inference-amdnpu }

`amdnpu`は独立した任意プラグイン`libgstamdnpu.so`です。`AMDVA_BUILD_NPU=ON`でビルドし、`AMDVA_ORT_ROOT`にRyzen AIのC++ SDKを指定します。起動前に`scripts/env.sh`と`examples/nvr/scripts/mobileclip-env.sh RYZEN_AI_VENV`を読み込みます。NPUを無効にしたビルドでも、GPU用エレメントはNPUのSDKなしで使えます。

両パッドのcapsは`application/x-amd-shared-tensor`または`video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]`です。各バッファーには`AmdSharedTensorMeta`が必要です。映像だけではエラーになります。入力のcaps、GstMemory、タイムスタンプ、メタデータを保持し、バッファーごとに同期推論を1回実行して`AmdNpuResultMeta`を追加します。

| プロパティ（すべてREADY） | 内容 |
| --- | --- |
| `model` | 静的なfloat32 `[1,3,H,W]`入力を1つ持つ画像ONNXモデル |
| `provider-config` | Vitis AIの設定JSON |
| `cache-dir`、`cache-key` | 事前準備したキャッシュのルートとキー。`context.json`が必要 |
| `gpu-device` | HIP前処理のデバイス番号。既定値0 |

名前が`batch`の可変次元は1に固定し、ほかの可変次元は拒否します。CPU EPへのフォールバックは無効です。出力は名前と静的な形状を持つ数値テンソルで、float32、float16、int32、int64、uint8に対応します。モデル固有の後処理は行いません。初期化や推論の失敗はバスエラーになり、PAUSED/PLAYING中のプロパティ変更は受け付けません。

`amdva::SharedTensor`はHIPのmapped host allocation、GPU側の別名ポインター、前処理の完了イベントを所有します。`device_data()`へ書き込み、`record_ready(stream)`を呼び、`add_shared_tensor_meta(buffer, tensor)`で添付します。推論側はイベントを待ってから、同じ`host_data()`の領域でORT入力を作ります。`NpuResult::input_address`でこのポインターを確認できます。推論と入力を参照するすべての処理が終わるまで、領域を保持し、再書き込みを避けてください。

`NpuResult::tensors`は型・名前・形状、読み取り専用のデータポインター、ORT出力領域の共有所有者を保持します。後続推論やパイプライン停止後も、出力全体をコピーせずに参照できます。GstBuffer全体のコピーでは入力・出力の所有権を共有し、部分コピーでは両メタデータを引き継ぎません。EP内部の転送やキャッシュ同期はこの契約の対象外です。

`amdpreproc output-memory=host-mapped`はHIP前処理の結果を新しい共有領域に直接書き込み、`AmdSharedTensorMeta`を添付します。既定値の`output-memory=device`は従来のGPUテンソル出力です。どちらもmodel-infoで入力名、寸法、正規化を指定します。

```text
filesrc location=in.mp4 ! qtdemux ! h265parse ! vah265dec !
  amdpreproc output-memory=host-mapped model-info=image.json !
  amdnpu model=image.onnx provider-config=provider.json cache-dir=cache cache-key=model !
  appsink
```

ネイティブのテンソル生成側では、`AMDVA::amdva_npu_pipeline`と`amdva::NpuPipeline`で常設の`appsrc → amdnpu → appsink`を使えます。呼び出しを直列化し、両キューの上限を1バッファーにしています。エラーやタイムアウト後はパイプラインを作り直してください。MobileCLIPとNVRのNPU画像推論もこの経路を使います。Pillow互換のHIP前処理はネイティブライブラリに残し、CPUテキスト推論、GPU画像推論、検索のフィンガープリントは維持します。

### 実機検証の再実行

```bash
source scripts/env.sh
source examples/nvr/scripts/mobileclip-env.sh /path/to/ryzen-ai-venv
cmake -S . -B build -DAMDVA_BUILD_NPU=ON -DAMDVA_HARDWARE_TESTS=ON \
  -DAMDVA_ORT_ROOT="$AMDVA_ORT_ROOT"
cmake --build build -j8
python scripts/prepare_npu_fixture.py artifacts/npu-fixture /path/to/provider.json
build/src/gst/amdva-validate-npu artifacts/npu-fixture /path/to/hevc.mp4
```

専用の検証モデルはConv/Relu/GlobalAveragePoolと2つの出力を持ち、MobileCLIPには依存しません。GPUで書いた入力を交互に切り替えて100回実行し、出力の期待値、入力ポインターの一致、停止後の出力保持、メタデータのコピー、不正入力、READYプロパティ、caps、flush/EOS、任意のVAデコード・前処理を確認します。`AMDVA_NPU_TEST_FIXTURE`と`AMDVA_RYZEN_AI_TEST_VENV`を設定すると、CTestに`npu-element-contract`を登録します。実機不要の`npu-meta-contract`はNPU無効時にも型と出力所有権を確認します。[検証結果](npu-element-results.json)と[MobileCLIPサンプル](../../examples/mobileclip/README.ja.md)も参照してください。

## 互換用の組: `amdpreproc`、`amdmigraphx`

当初の 2 要素構成の YOLOv8 インターフェースで、互換性のために残しています（`amdpreproc`: `device`、`tensor-width`、`tensor-height`、`model-info`、`require-zero-copy`、`output-memory`。`amdmigraphx`: `model`、`model-info`、`device`、`confidence-threshold`、`nms-threshold`、`require-zero-copy`）。新しいGPUタスクのパイプラインでは
`amddetect` を使ってください。タスク要素の前に `amdpreproc` を置かないでください。

## 同梱の DL Streamer 要素

`third_party/dlstreamer` からビルドします（`AMDVA_BUILD_GVA_COMPAT`）。プロパティは DL Streamer v2026.2.0 のものです。

| 要素 | このフレームワークでの用途 |
| --- | --- |
| `gvatrack` | `tracking-type=short-term-imageless` または `zero-term-imageless`。CPU のみで動作し、画像をマップしません |
| `gvametaconvert` | GstAnalytics を DL Streamer の JSON に変換します（`format=json`、`json-indent`、`source`、`tags`） |
| `gvametapublish` | `method=file|mqtt|kafka`、`file-path`、`address`、`topic` |
| `gvastreammux` | `output-mode=container` で解像度の異なる複数ストリームをバッチ化します。`max-queue-size`、`max-wait-time`、`sync-mode` |
| `gvastreamdemux` | コンテナーをソースごとの分岐に戻します |
| `gvametaaggregate` | 並列の分岐をまとめ、AMD の GPU ペイロードも統合します |

## 典型的なパイプライン

```text
filesrc ! h264parse ! vah264dec ! video/x-raw(memory:VAMemory),format=NV12 !
  amddetect model=m.mxr model-info=m.json !
  gvatrack tracking-type=short-term-imageless !
  amdpython module=cb.py ! amdmetapublish method=mqtt address=tcp://broker:1883 !
  amdwatermark ! amdencode ! h264parse ! mp4mux ! filesink location=out.mp4
```
