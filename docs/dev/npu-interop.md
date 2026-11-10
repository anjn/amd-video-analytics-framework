# GPU and NPU memory sharing on the current host

The in-process ORT/MIGraphX application path is now implemented in the [MobileCLIP sample](../../examples/mobileclip/README.md) and NVR. The raw XRT experiments below are earlier diagnostic evidence, not the application runtime.

On 2026-11-10, the current host successfully ran native MobileCLIP GPU and NPU
inference in one process, with either initialization order. A separate probe
also passed a 1 MiB buffer through GPU writes, NPU DMA and a final HIP readback
using **XRT host buffers registered with HIP**. This establishes a working
memory-sharing path for further inference integration. The raw probe alone does not establish zero-copy model inference; the application migration is described in the sample linked above.

The machine-readable evidence is [npu-interop-results.json](npu-interop-results.json).
The probes are [npu_interop_probe.cpp](../../tools/npu_interop_probe.cpp) and
[npu_runtime_probe.py](../../tools/npu_runtime_probe.py).

## Environment and coexistence

The host is Ryzen AI 9 HX 370, Radeon 890M (`gfx1150`), XDNA2 (`1022:17f0`),
Ubuntu with kernel `6.17.0-1033-oem`. The framework loads ROCm 7.2.0:
`libamdhip64.so.7.2.70200` and `libhsa-runtime64.so.1.18.70200` from `.deps`.
System XRT is 2.26.89; the loaded XDNA shim and kernel module are
`2.26.260102.59.release` (module build `6b78ab22364c74e9bc51c667ba11ad087cde4be2`).
The native MobileCLIP probe uses the Ryzen AI 1.9 development environment and
ONNX Runtime 1.30.0. Its XRT core libraries come from the venv's `voe/lib`,
while its XDNA shim comes from `/opt/xilinx/xrt/lib`. Exact loaded paths are
recorded in the results; do not assume all runtime libraries come from `/opt`.

The adjacent `embeddinggemma2-rocm/docs/mobileclip2.md` records an earlier failure:
ROCr opened `/dev/accel/accel0`, and subsequent NPU initialization failed with
`Device or resource busy`; NPU-first initialization worked. ROCr 7.2 source
indeed [discovers both KFD and XDNA drivers](https://github.com/ROCm/ROCR-Runtime/blob/rocm-7.2.0/runtime/hsa-runtime/core/runtime/amd_topology.cpp),
and its [XDNA driver opens the accelerator node](https://github.com/ROCm/ROCR-Runtime/blob/rocm-7.2.0/runtime/hsa-runtime/core/driver/xdna/amd_xdna_driver.cpp).
The current probe observes separate ROCr and XRT descriptors for that node.
Opening the node alone therefore does not demonstrate a conflict.

All three fresh-process cases now pass: HIP initialization before NPU creation,
GPU model creation and preparation before NPU creation, and NPU model preparation
before GPU creation. Each case holds both model contexts alive, runs inference
on each device sequentially, and checks finite unit-length 512D embeddings.
These checks do not measure simultaneous inference execution or full NVR behavior.
The earlier EBUSY failure is not reproduced; identifying which runtime or driver
change removed it requires a version comparison that was not performed here.

## Memory-sharing results

| Allocation and sharing direction | Allocation check | NPU DMA execution |
| --- | --- | --- |
| XRT `host_only` BO → DMA-BUF → HIP external-memory import | Fails: HIP reports out of memory; `AMDKFD_IOC_GET_DMABUF_INFO` returns `EINVAL` | Not reached |
| HIP `hipMalloc` → DMA-BUF → XRT import | Passes import, mapping and visibility of HIP writes | Fails: `DRM_IOCTL_AMDXDNA_EXEC_CMD`, `EFAULT` / Bad address |
| XRT `host_only` BO → map → `hipHostRegister` → HIP device pointer | Passes visibility of HIP writes | Passes 6/6 fresh-process runs, three per initialization order |
| HIP `hipHostMallocMapped` → XRT user-pointer BO | BO creation passes | Not tested |

The successful path allocates separate input and output XRT BOs, maps and
registers both with HIP, writes the input and clears the output with `hipMemset`,
waits for HIP completion, then executes the NPU passthrough command. HIP reads
the final output into a diagnostic CPU buffer and checks every byte. No payload
copy stages data through CPU memory between devices. The passthrough itself
copies between the two BOs through NPU shim/memory-tile DMA; it uses no compute
tile and is not a model-inference test. GPU access here uses HIP memory operations,
not a preprocessing shader or inference kernel.

Explicit synchronization remains necessary: HIP completion before NPU submission,
XRT input synchronization, NPU completion, and XRT output synchronization before
HIP consumes the result. In the installed driver source,
`/usr/src/xrt-amdxdna-2.26.260102.59.release/drivers/accel/amdxdna/amdxdna_gem.c`,
`amdxdna_drm_sync_bo_ioctl` flushes CPU caches for these shared buffers.
It is not an intermediate payload copy. Registration and cache synchronization
still have costs; this probe does not measure latency or throughput.

## Initial requirements for inference integration

Before the migration, the NVR read crops back using `hipMemcpyDeviceToHost` in
`examples/nvr/backend/search.cpp`, sends RGB bytes over a Unix socket in
`mobileclip_client.hpp`, and receives them in CPU vectors in
`mobileclip_worker.cpp`. The native MobileCLIP library preprocesses host data
and supplies an `Ort::MemoryInfo::CreateCpu` tensor to the Vitis AI provider.
Removing the worker alone leaves these copies and does not expose the provider's
actual NPU input BO.

The next inference experiment should obtain the compiled model's input/output
BOs and tensor layout, register those BOs with HIP, and write GPU preprocessing
results directly into the input allocation. The locally installed
`flexmlrt/include/FlexMLClient.h` exposes `ErtTensorType::getBo`, `setBo`,
`isBo`, `skipPrePostProcessing`, and model `getDevice`. These are candidate
integration APIs; compatibility with the prepared MobileCLIP artifacts and the
provider path still needs validation. Generic ONNX Runtime I/O binding alone
does not prove that the provider avoids an internal copy or layout conversion.

Acceptance should require model output parity, actual HIP-kernel access,
repeated alternating inputs on reused buffers, correct lifetime/synchronization,
and tracing of the provider's buffer handling and payload copies. Keep metadata
and final diagnostic readbacks distinct from intermediate tensor transfers.
Separate processes can also share BOs using DMA-BUF FDs transferred with
`SCM_RIGHTS`; the present byte-stream IPC would need to change, and the successful
in-process registration test does not validate that cross-process path.

## Reproduction on this host

Run from the framework checkout. No framework rebuild is required.

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

Compile the passthrough using the existing local MLIR-AIE environment. Its
checkout HEAD and artifact hashes are in the results; this is a local toolchain,
not a new dependency pinned by the framework bootstrap.

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

The registered-BO command passes; the HIP-export command demonstrates the NPU
submission failure. Use only the specified passthrough ABI and length.
For coexistence, run each order in a fresh process with the prepared bundle:

```bash
source /home/amd/work/rai/rai19-venv/bin/activate
for order in hip-first gpu-first npu-first; do
  timeout 60s python tools/npu_runtime_probe.py --order "$order" \
    --library /home/amd/work/embeddinggemma2-rocm/build-mc2/libmobileclip2.so \
    --bundle /home/amd/work/embeddinggemma2-rocm/artifacts/mobileclip2-api
done
```

For syscall evidence, prefix an allocation probe with
`strace -f -e trace=openat,ioctl -o build/npu-interop/probe.strace`.
