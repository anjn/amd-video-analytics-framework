#!/usr/bin/env bash
# Source after scripts/env.sh and before launching the sample or NVR.
# Usage: source examples/nvr/scripts/mobileclip-env.sh /path/to/ryzen-ai-venv
if [[ -z "${1:-}" || ! -f "$1/bin/activate" || ! -d "$1/onnxruntime/lib" ]]; then
    echo 'Pass the prepared Ryzen AI venv directory.' >&2
    return 1
fi
# Do not inherit EmbeddingGemma/PyTorch paths. Besides HIP aliases, Torch's
# bundled libtinfo can break the Vitis AI provider's system libncurses dependency.
# Keep the framework's rootless dependencies, then let Ryzen AI add its own paths.
export LD_LIBRARY_PATH="$ROCM_PATH/lib:$ROCM_PATH/lib/migraphx/lib:$AMDVA_ROOT/build:$AMDVA_ROOT/.deps/prefix/lib:$AMDVA_ROOT/.deps/sysroot/usr/lib/x86_64-linux-gnu"
source /opt/xilinx/xrt/setup.sh || return 1
source "$1/bin/activate" || return 1
# Keep the framework HIP/HSA runtime ahead of provider dependencies. No PyTorch HIP aliases.
export AMDVA_ORT_ROOT="$VIRTUAL_ENV/onnxruntime"
export LD_LIBRARY_PATH="$ROCM_PATH/lib:$ROCM_PATH/lib/migraphx/lib:$AMDVA_ORT_ROOT/lib:/opt/xilinx/xrt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
