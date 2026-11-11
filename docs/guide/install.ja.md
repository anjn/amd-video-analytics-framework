# インストール

Git checkoutから依存構築とモデル準備を行います。他のPCの `.deps`、ビルド結果、Python venv、生成済みモデルをコピーする必要はありません。初回はインターネット接続が必要です。動画はユーザーが用意します。

## ホストの前提条件

| 項目 | 条件 |
|---|---|
| OS | Ubuntu 24.04 LTS／26.04 LTS、x86_64 |
| GPU | Strix Point gfx1150／Strix Halo gfx1151 |
| GPU SDK | ROCm **10.1.0**、MIGraphX **2.18.0**。別版への自動切り替えなし |
| グラフィックス | ホストOSのカーネル、Mesa、libva、libdrm。GPU機能はprobeで検査 |
| 権限 | `/dev/kfd`、`/dev/dri/renderD128` の読み書き。必要なら管理者がrender／videoグループを設定し、再ログイン |
| その他 | Python 3.12以降、OS標準の開発パッケージ。モデル変換用Python 3.12はuvが解決 |

管理者は[AMDの導入手順](https://rocm.docs.amd.com/projects/install-on-linux/en/latest/install/rocm.html)に従い、OSとGPUに合うROCm 10.1.0のランタイム・開発ヘッダー・コンパイラー、MIGraphX 2.18.0を事前導入します。frameworkのbootstrapはこれらを導入・更新しません。

対象機で確認したパッケージは `amdrocm-core10.1-gfx1151=10.1.0-3` と `amdrocm10-migraphx=2.18.0-1`。gfx1150では対応するGPU向けパッケージを選びます。取得元は `https://stable.repo.amd.com/rocm/core/packages/ubuntu2604/` と `https://stable.repo.amd.com/rocm/migraphx/packages/ubuntu2604/`、24.04ではOS別の `ubuntu2404` を使います。配布版とパッケージ版を管理者が確認し、将来stableの既定値が変わっても指定版を維持します。

標準配置はROCmが `/opt/rocm/core-10.1`、MIGraphXが `/opt/rocm/extras-10`。別配置では `AMDVA_ROCM_ROOT`／`AMDVA_MIGRAPHX_ROOT` を明示して `host.py --write` を実行します。検出が曖昧、版違い、開発ファイルや動的依存が不足している場合は停止します。

## 構築と検査

```bash
# Run after the administrator has installed ROCm 10.1.0 and MIGraphX 2.18.0.
sudo apt-get update
sudo apt-get install $(python3 scripts/host.py --packages)
python3 scripts/host.py --write
python3 scripts/bootstrap.py --jobs 8
source scripts/env.sh
build/amdva-probe
```

`host.py` はOS、SDKの版、必要パッケージ、共有ライブラリ依存、GPU権限を検査し、`.deps/host.json` に選択したSDKとホストパッケージ版を記録します。実際のGPUは `amdva-probe` の `gate_passed:true` で確認します。probeだけでパイプライン全体のゼロコピーを保証するものではありません。

`bootstrap.py` は固定したGStreamerソースとパッチを取得・検証して `.deps/prefix` に構築します。ROCm、Qt、libvaなどを別OSのdebから展開しません。再実行可能で、取得済みソースはSHA-256を確認します。`--check` はダウンロード済みソースの検査です。各シェルでは `source scripts/env.sh` を行ってください。外部のTorch／別版ROCm／旧sysrootの検索パスは継承しません。

## NPUを有効にする場合

Ryzen AI SW **1.9**、XRT **2.26.89**、配布版NPUドライバーを管理者が[AMD手順](https://ryzenai.docs.amd.com/en/latest/linux.html)で用意します。認証情報やライセンスをGitに格納しません。Secure Bootでドライバーが拒否された場合は管理者が署名登録または無効化を判断し、標準ドライバーに自動代替しません。

**AMD公式のLinux対象OSは24.04です。26.04のframework検証はベンダーの公式サポートを意味しません。** 26.04では実行可能スタックを要求するORT拡張のため、明示的にNPU環境を有効にするシェルだけにglibc互換設定を適用します。memlock不足はPAM設定・再ログインで解消してください。

```bash
python3 scripts/host.py --write --ryzen-ai-venv /path/to/rai19-venv
python3 scripts/bootstrap.py --jobs 8 --ryzen-ai-venv /path/to/rai19-venv
source scripts/env.sh
```

## 動画とモデルの準備

[1440pデモ](../../examples/nvr/demo/1440p/README.ja.md)の新規準備手順に従います。モデルは公開元の固定チェックポイントから生成し、GPU・SDKに合わない古いモデルは拒否します。NPUキャッシュも最終配置で生成します。

## 検証状態

対応対象は両OS×両GPUの4構成です。対応対象と実機検証済みを区別し、[環境別結果](../dev/support-matrix.md)を確認してください。検証用の別OS導入は自動実行しません。

固定したtokenizer依存にはRust／Cargo 1.85以降が必要です。ディストリビューションのcargoパッケージに代えてrustupのツールチェーンも使用でき、host.pyは実際の版を記録します。

## 管理者向けapt導入例

新しいホストでは、以下のGPUを実機に合わせて選びます。検証時に確認したパッケージ版を明示し、OS別の署名付きリポジトリを登録する例です。管理者が明示実行する手順であり、frameworkのbootstrapは実行しません。既存のリポジトリ設定を確認し、既存SDKの置き換えは承認を得てから行ってください。

```bash
set -euo pipefail
. /etc/os-release
case "$VERSION_ID" in
  24.04) distro=ubuntu2404 ;;
  26.04) distro=ubuntu2604 ;;
  *) echo "Ubuntu 24.04 or 26.04 required" >&2; exit 1 ;;
esac
gpu=gfx1151 # Strix Halo; use gfx1150 for Strix Point
case "$gpu" in gfx1150|gfx1151) ;; *) exit 1 ;; esac
key=$(mktemp)
curl -fsSL https://stable.repo.amd.com/rocm/gpg/packages.gpg -o "$key"
fingerprint=$(gpg --show-keys --with-colons "$key" | awk -F: '$1=="fpr" {print $10; exit}')
test "$fingerprint" = D0F004A0025A1145C7807FCD0701EAC4D5E02107
sudo install -d -m 0755 /etc/apt/keyrings
gpg --dearmor < "$key" | sudo tee /etc/apt/keyrings/amdrocm.gpg >/dev/null
rm "$key"
sudo tee /etc/apt/sources.list.d/amdrocm-stable.sources >/dev/null <<EOF
Types: deb
URIs: https://stable.repo.amd.com/rocm/core/packages/$distro/
Suites: stable
Components: main
Architectures: amd64
Signed-By: /etc/apt/keyrings/amdrocm.gpg
EOF
sudo tee /etc/apt/sources.list.d/amdrocm-migraphx.sources >/dev/null <<EOF
Types: deb
URIs: https://stable.repo.amd.com/rocm/migraphx/packages/$distro/
Suites: stable
Components: main
Architectures: amd64
Signed-By: /etc/apt/keyrings/amdrocm.gpg
EOF
sudo apt-get update
sudo apt-get install "amdrocm-core10.1-$gpu=10.1.0-3" amdrocm10-migraphx=2.18.0-1
```

aptの前提パッケージ一覧にはcargoを含めません。動作中のrustupを置き換えないためです。Rustが未導入または古い場合は、管理者がOSパッケージまたは[rustup](https://rustup.rs/)でRust／Cargo 1.85以降を用意し、host.pyを再実行してください。

NPUを使わずMobileCLIPのGPU画像推論とCPUテキスト推論を使う場合、bootstrapに
`--ryzen-ai-venv /path/to/rai19-venv --mobileclip-backend gpu` を指定します。
ORT C++ SDKとCPU providerは必要ですが、XRT・Vitis provider・NPUのmemlockやアクセス権は要求しません。
デモ準備にも `--backend gpu` を指定します。SDK指定を省略した場合はMobileCLIP／NPUを明示的に無効化し、
古いCMake設定を引き継ぎません。
