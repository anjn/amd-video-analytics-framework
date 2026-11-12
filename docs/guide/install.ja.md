# インストール

ブートストラップスクリプトは、Ubuntu 26.04 用に固定した GStreamer と開発パッケージをリポジトリ内の `.deps/` にインストールし、フレームワークをビルドしてテストします。sudo は使わず、システムのライブラリも置き換えません。ROCm 10.1 と対応する MIGraphX は事前に用意してください。

## 動作環境

| 項目 | 検証済みの環境 |
| --- | --- |
| ハードウェア | Ryzen AI 9 HX 370 / Radeon 890M（gfx1150、VPE 6.1.0） |
| OS | Ubuntu 24.04、カーネル 6.17.0-1032-oem 以降、Mesa 25.2.8 |
| デバイス | `/dev/dri/renderD128` と `/dev/kfd` にアクセスできるユーザー |
| ツール | Python 3.12 以降、`uv`、C/C++ コンパイラー、CMake 3.22 以降、pkg-config、git、dpkg-deb、apt-get |
| ディスク | 依存関係のダウンロードとビルドに十分な空き容量 |

上の表は従来の検証環境です。現在のブートストラップは Ubuntu 26.04 を対象としており、ROCm の既定パスは `/opt/rocm/core-10.1`、MIGraphX の既定パスは `.deps/migraphx` です。それぞれ `AMDVA_ROCM_ROOT`、`AMDVA_MIGRAPHX_ROOT` で変更できます。Ubuntu 26.04 と ROCm 10.1 で映像解析パイプライン全体の検証は完了していません。

## 1. 依存関係とフレームワークのビルド

リポジトリのルートで実行します。所要時間の大半は依存関係のダウンロードとビルドです。

```bash
python3 scripts/bootstrap.py --jobs 8
```

最後に ctest が実行され、次のように表示されれば成功です。

```text
100% tests passed, 0 tests failed out of ...
```

テストが失敗した場合は、まず動作環境の表にある前提ツールと、デバイスファイルへのアクセス権を確認してください。

## 2. 環境変数の設定

ビルドしたフレームワークと依存関係を使うシェルでは、毎回最初に次を実行します。以降のページのコマンドは、すべてこの設定を前提とします。

```bash
source scripts/env.sh
```

## 3. GPU の動作確認

VA、VPE、HIP が同じ GPU メモリを共有して動くかを確認します。

```bash
build/amdva-probe
```

最後の行に JSON が表示され、その中に `"gate_passed":true` があれば成功です。

```text
{"schema":1,"gpu":"gfx1150",...,"vpe_observed":true,...,"gate_passed":true}
```

失敗する場合は、`source scripts/env.sh` を実行したシェルで動かしているかを確認してください。ログイン環境が `HSA_OVERRIDE_GFX_VERSION` を設定していると HIP が別の GPU として動作し、`scripts/env.sh` はこの設定を解除します。

フレームワークの要素が GStreamer から見えることも確認できます。

```bash
gst-inspect-1.0 amdanalytics
```

`amddetect`、`amdclassify`、`amdinference`、`amdwatermark`、`amdencode`、`amdpython`、`amdmetapublish` などが一覧に出れば成功です。

次は [クイックスタート](quickstart.ja.md) で、動画に検出結果を描画します。
