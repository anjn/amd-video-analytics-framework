# MobileCLIPのGPU推論とNPU推論を同一プロセスで実行する

このサンプルとNVRは共通ライブラリの`amdva_mobileclip`を使います。GPU前処理は推論入力のfloat32 NCHWテンソルへ直接書き込みます。GPU推論はMIGraphXのデバイス入出力、NPU推論はHIPで共有したホストメモリを参照するONNX RuntimeのCPUテンソルを使います。NPUはVitis AI EP経由で実行します。EP内部のバッファー操作は今回のコピー禁止の対象外で、サンプルからXRTを直接呼び出す必要はありません。

## ビルドとモデルの準備

`image.onnx`、`text.onnx`、`tokenizer.json`とチェックサムを含む既存のMobileCLIP2-S2ネイティブバンドルを使います。NPUには準備済みのBF16用`npu-cache`と`npu.json`も必要です。元のバンドルを保つため、このチェックアウトの`artifacts/`へコピーしてからGPUプロファイルを追加します。モデルは事前に準備し、アプリケーション起動時にはコンパイルしません。

```bash
source scripts/env.sh
source examples/nvr/scripts/mobileclip-env.sh /path/to/ryzen-ai-venv
cmake -S . -B build -DAMDVA_BUILD_MOBILECLIP=ON -DAMDVA_ORT_ROOT="$AMDVA_ORT_ROOT"
cmake --build build -j8
cp -a /path/to/mobileclip2-api artifacts/mobileclip-device
python scripts/prepare_mobileclip_device.py artifacts/mobileclip-device
```

この機能のビルドにはRyzen AIのONNX Runtime C++ SDKとCargoが必要です。MITライセンスのトークナイザーとCargo依存は`third_party/mobileclip-tokenizer`と`dependencies.lock.json`で固定しています。通常のフレームワークは`AMDVA_BUILD_MOBILECLIP=OFF`でもビルドできます。そのビルドでMobileCLIPを選ぶとエラーになります。

GPU用の`gpu-device-b1.mxr`と`gpu-device-b4.mxr`はFP16演算と`offload_copy=False`でコンパイルします。旧ホスト入力用の`gpu-b*.mxr`は比較用に残しますが、このランタイムでは読み込みません。

## コピーなしの入力を検証する

```bash
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/examples/mobileclip/amdva-mobileclip-interop \
  artifacts/mobileclip-device gpu+npu 100 /path/to/reference/libmobileclip2.so
```

引数はバンドル、モード、反復回数、比較用の旧ネイティブライブラリです。モードは`gpu`、`npu`、`gpu+npu`、反復回数は100以上を指定します。比較用ライブラリは省略できます。

入力画像はGPUで生成します。反復ごとに寸法を変え、画像内容も交互に切り替えます。GPUは1〜4枚で検証し、2〜3枚は4枚へパディングします。NPUは1枚です。併用モードでは両方の推論を並行して実行します。GPU前処理は従来のPillow互換CPU実装と照合し、旧ライブラリを指定した場合はバックエンドごとのEmbeddingのコサイン類似度が0.9999以上になることも確認します。日英のテキスト検索には従来と同じCPUトークナイザーとモデルを使い、旧ライブラリを指定した場合は結果も比較します。不正な寸法、デバイス番号、空の入力や文章、別のランタイムで準備した入力を拒否することも確認します。

この検証にはコピー監査ライブラリが必要です。JSONにはPID、GPUが書いた入力のポインター、ORTに渡したポインター、反復回数、バッファー再利用、前処理誤差、入力コピー違反、Embeddingの比較結果、呼び出し時間を記録します。`application_input_copy_verified=true`には、入力受け渡し中のコピーが0回で、前処理が完全一致し、バッファーが再利用されたことが必要です。検証用のテンソル読み戻しと旧ライブラリの比較推論は明示的に監査から除外します。呼び出し時間は前処理と推論の合計で、NVRの処理能力を保証する値ではありません。

`ep_internal_copy_verified`はfalseのままです。監査では、アプリケーションの入力準備中のHIP転送と、元の領域を変更せず推論へ渡したことを確認します。ONNX Runtime、Vitis AI、MIGraphX内部の任意のCPU memcpyは追跡しません。キャッシュ同期とNPU内部のDMAはEPが処理します。

## NVRの設定

```json
{
  "embedding_model": "mobileclip2-s2",
  "mobileclip_backend": "gpu+npu",
  "mobileclip_bundle": "/absolute/path/to/mobileclip-device",
  "embedding_batch": 4,
  "ryzen_ai_venv": "/path/to/ryzen-ai-venv"
}
```

これらを`search`内へ設定し、起動ヘルパーからNVRを実行します。

```bash
examples/nvr/scripts/run.sh --fullscreen /path/to/nvr-config.json
```

起動ヘルパーは`search.ryzen_ai_venv`、環境変数の`AMDVA_RYZEN_AI_VENV`、有効な`VIRTUAL_ENV`の順でRyzen AI環境を選びます。MobileCLIPでは、継承したEmbeddingGemmaやPyTorchのライブラリパスを除いてから依存環境を設定します。旧HIPエイリアスや同梱libtinfoが残るとVitis AIを読み込めず、ReIDの初期化にも進めません。準備済みデモの`artifacts/1440p-demo/run-demo.sh`もこのヘルパーを使います。

MobileCLIPはすべて同一プロセスで実行します。旧設定の`mobileclip_library`と`mobileclip_worker`は読み込めますが、実行には使いません。`ryzen_ai_venv`は起動時の環境設定に使い、ワーカーは起動しません。推論にワーカー実行ファイルやPythonは使いません。環境ヘルパーは起動前に読み込みます。HIPやORTの初期化後にライブラリパスを切り替える方法には対応しません。

NVRはフレームを更新した後も、キューや非同期ジョブが参照するGPUクロップ画像を保持します。サムネイルとReID用のCPU画像は残しますが、MobileCLIPへ戻しません。検索APIへ直接渡したCPU画像だけは、入口で一度アップロードします。NPUの前処理は短時間だけGPUの利用権を取得し、EP呼び出し前に解放します。バッチ設定、デバイスごとの予算、テキスト検索、バックエンドごとの検索空間は維持します。新しいフィンガープリントで旧ワーカー方式のインデックスと区別し、DBの移行や既存録画の再Embeddingは行いません。

[検証結果](../../docs/dev/mobileclip-native-results.json)と[先行するXRT調査](../../docs/dev/npu-interop.ja.md)も参照してください。

RTSPのテストサーバーを起動している場合は、画面なしでNVRを検証できます。

```bash
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/examples/nvr/amdva-mobileclip-nvr-validate /path/to/nvr-config.json
```

このツールは`AMDVA_HARDWARE_TESTS=ON`でビルドします。収集、日英の検索、CPUから渡した画像での検索、予算の変更、子プロセスがないこと、コピー監査、終了処理を確認します。GUIの検索検証では、ライブ・録画からの領域検索、再生、録画削除も確認します。
