<!-- source: docs/dev/npu-interop.md 9879845b8c94d98d9b313b545514e745c53775b8 -->
# 現在の環境でのGPUとNPUのメモリ共有

現在は[MobileCLIPサンプル](../../examples/mobileclip/README.ja.md)とNVRに、同一プロセスのORT/MIGraphX経路を実装しています。以下のXRT直接検証は先行調査の記録で、アプリケーションの実行経路ではありません。

2026年11月10日の実機検証では、同一プロセス内でMobileCLIPのGPU推論とNPU推論が、どちらの初期化順でも成功しました。別の検証では、**XRTが確保したホストバッファーをHIPに登録する方法**で、GPU側の書き込み、NPUのDMA処理、HIP経由での結果の読み出しが1 MiBで成功しました。推論へ組み込むための共有メモリ経路を確認した段階です。この直接検証だけではモデル推論全体のゼロコピーを確認したことにはなりません。アプリケーションの変更は上記のサンプルを参照してください。

実行結果は[npu-interop-results.json](npu-interop-results.json)、再現用ツールは[npu_interop_probe.cpp](../../tools/npu_interop_probe.cpp)と[npu_runtime_probe.py](../../tools/npu_runtime_probe.py)です。

## 環境と同一プロセスでの推論

実機はRyzen AI 9 HX 370、Radeon 890M（`gfx1150`）、XDNA2（`1022:17f0`）で、Ubuntuのカーネルは`6.17.0-1033-oem`です。フレームワークは`.deps`内のROCm 7.2.0から`libamdhip64.so.7.2.70200`と`libhsa-runtime64.so.1.18.70200`を読み込みます。システムのXRTは2.26.89、XDNA shimとカーネルモジュールは`2.26.260102.59.release`です。モジュールのビルドIDは`6b78ab22364c74e9bc51c667ba11ad087cde4be2`です。

MobileCLIPの検証にはRyzen AI 1.9開発版の環境とONNX Runtime 1.30.0を使いました。このプロセスでは、XRTコアライブラリをvenvの`voe/lib`から、XDNA shimを`/opt/xilinx/xrt/lib`から読み込んでいます。読み込んだ実際のパスは実行結果に記録しています。

隣接リポジトリの`embeddinggemma2-rocm/docs/mobileclip2.md`には、ROCrが`/dev/accel/accel0`を開いたあとにNPUを初期化すると`Device or resource busy`になり、NPUを先に初期化すると成功した記録があります。ROCr 7.2のソースでも、[KFDとXDNAの両方を探索](https://github.com/ROCm/ROCR-Runtime/blob/rocm-7.2.0/runtime/hsa-runtime/core/runtime/amd_topology.cpp)し、[XDNAのデバイスを開く](https://github.com/ROCm/ROCR-Runtime/blob/rocm-7.2.0/runtime/hsa-runtime/core/driver/xdna/amd_xdna_driver.cpp)実装を確認できます。現在もROCrとXRTがそれぞれNPUのFDを保持しますが、その状態で推論できました。FDを開いているだけでは競合していると判断できません。

今回は、HIP初期化→NPU作成、GPUモデルの作成・準備→NPU作成、NPUモデルの準備→GPU作成の3通りすべてが成功しました。各検証は新しいプロセスで実行し、両方のモデルコンテキストを保持したまま各デバイスで順に推論し、512次元の出力が有限値で単位長になることを確認しています。推論の同時実行やNVR全体の動作は検証していません。過去のEBUSYは再現せず、解消した変更を特定するためのバージョン比較も行っていません。

## メモリ共有の結果

| 確保元と共有方法 | メモリの確認 | NPUのDMA実行 |
| --- | --- | --- |
| XRTの`host_only` BO→DMA-BUF→HIPの外部メモリインポート | 失敗。HIPはout of memory、`AMDKFD_IOC_GET_DMABUF_INFO`は`EINVAL` | 未到達 |
| HIPの`hipMalloc`→DMA-BUF→XRTのインポート | インポート、マッピング、HIPの書き込みの参照は成功 | 失敗。`DRM_IOCTL_AMDXDNA_EXEC_CMD`が`EFAULT` / Bad address |
| XRTの`host_only` BO→マッピング→`hipHostRegister`→HIPデバイスポインター | HIPの書き込みを参照できることを確認 | 初期化順ごとに3回、計6回成功 |
| HIPの`hipHostMallocMapped`→XRTのユーザーポインターBO | BO作成に成功 | 未検証 |

成功した経路では、入出力用に別々のXRT BOを確保し、両方をマッピングしてHIPに登録します。HIPの`hipMemset`で入力を書き、出力をゼロで消去してから、HIPの完了を待ってNPUのパススルー処理を実行します。最後にHIPからCPUの検証用バッファーへ出力を読み戻し、全バイトを照合します。デバイス間の中間受け渡しにCPUへのコピーはありません。パススルー自体はNPUのshimとメモリータイルのDMAで入力BOから出力BOへコピーする処理で、演算タイルや推論モデルは使いません。GPU側もHIPのメモリ操作であり、前処理カーネルや推論カーネルではありません。

この経路でも同期は必要です。NPUへ投入する前にHIPの完了を待ち、XRTの入力同期を行い、NPUの完了後にXRTの出力同期を行ってからHIPで結果を使います。インストール済みドライバーの`/usr/src/xrt-amdxdna-2.26.260102.59.release/drivers/accel/amdxdna/amdxdna_gem.c`では、`amdxdna_drm_sync_bo_ioctl`が共有バッファーのCPUキャッシュをフラッシュします。これは中間データのコピーではありません。ただし登録やキャッシュ同期にはコストがあり、今回は遅延やスループットを測っていません。

## 初期調査での推論接続の要件

移行前のNVRでは、`examples/nvr/backend/search.cpp`がクロップ画像を`hipMemcpyDeviceToHost`で読み戻し、`mobileclip_client.hpp`がRGBバイト列をUnixソケットで送り、`mobileclip_worker.cpp`がCPU上のベクターに受け取ります。MobileCLIPのネイティブライブラリもホストデータを前処理し、`Ort::MemoryInfo::CreateCpu`のテンソルをVitis AIプロバイダーへ渡します。ワーカーを廃止するだけでは、これらのコピーは残り、NPUの入力BOにも直接アクセスできません。

次の推論実験では、コンパイル済みモデルの入出力BOとテンソル配置を取得し、そのBOをHIPへ登録して、GPU前処理の結果を入力領域へ直接書く方法を検証します。ローカル環境の`flexmlrt/include/FlexMLClient.h`には、`ErtTensorType::getBo`、`setBo`、`isBo`、`skipPrePostProcessing`、モデルの`getDevice`があります。これらは接続候補ですが、準備済みMobileCLIPの成果物やプロバイダーの実行経路との互換性は未確認です。ONNX RuntimeのI/O Bindingだけでは、プロバイダー内部のコピーや配置変換を省略できる証拠にはなりません。

推論での合格条件には、既存出力との一致、実際のHIPカーネルからのアクセス、同じバッファーを再利用しながら入力を交互に変える反復試験、寿命と同期の確認、プロバイダー内部のバッファーとコピーの追跡が必要です。メタデータや最終照合用の読み戻しは、中間テンソルの転送と区別して記録します。

別プロセスでも、DMA-BUFのFDを`SCM_RIGHTS`で渡してBOを共有できます。その場合は現在のバイト列IPCを変更する必要があります。今回成功した同一プロセスのHIP登録経路では、プロセス間の共有までは検証していません。

## このホストでの再現手順

フレームワークのチェックアウトで実行します。フレームワーク本体の再ビルドは不要です。

```bash
source scripts/env.sh
source /opt/xilinx/xrt/setup.sh
mkdir -p build/npu-interop
g++ -std=c++17 -Wall -Wextra -Werror -D__HIP_PLATFORM_AMD__ \
  -I"$ROCM_PATH/include" -I/opt/xilinx/xrt/include tools/npu_interop_probe.cpp \
  -L"$ROCM_PATH/lib" -L/opt/xilinx/xrt/lib -lamdhip64 -lxrt_coreutil \
  -o build/npu-interop/probe
build/npu-interop/probe hip-first register
build/npu-interop/probe xrt-first import
build/npu-interop/probe xrt-first hip-export
build/npu-interop/probe xrt-first userptr
```

パススルーは既存のローカルMLIR-AIE環境でコンパイルします。チェックアウトのHEADと生成物のハッシュは実行結果に記録しています。フレームワークのbootstrapで新たに固定した依存関係ではありません。

```bash
(
  source /home/amd/work/xdna2/mlir-aie/ironenv/bin/activate
  source /home/amd/work/xdna2/mlir-aie/utils/env_setup.sh
  python /home/amd/work/xdna2/mlir-aie/programming_examples/basic/passthrough_dmas/passthrough_dmas.py \
    -d npu2 -n 262144 --xclbin-path "$PWD/build/npu-interop/passthrough.xclbin" \
    --insts-path "$PWD/build/npu-interop/insts.bin"
)
timeout 25s build/npu-interop/probe hip-first register \
  build/npu-interop/passthrough.xclbin build/npu-interop/insts.bin
timeout 25s build/npu-interop/probe xrt-first hip-export \
  build/npu-interop/passthrough.xclbin build/npu-interop/insts.bin
```

`register`のコマンドは成功し、`hip-export`のコマンドはNPUへの投入失敗を再現します。指定したパススルーのABIと要素数で実行してください。同居の確認は、準備済みバンドルを使い、順序ごとに新しいプロセスで実行します。

```bash
source /home/amd/work/rai/rai19-venv/bin/activate
for order in hip-first gpu-first npu-first; do
  timeout 60s python tools/npu_runtime_probe.py --order "$order" \
    --library /home/amd/work/embeddinggemma2-rocm/build-mc2/libmobileclip2.so \
    --bundle /home/amd/work/embeddinggemma2-rocm/artifacts/mobileclip2-api
done
```

システムコールを記録するには、メモリ検証コマンドの前に`strace -f -e trace=openat,ioctl -o build/npu-interop/probe.strace`を付けます。
