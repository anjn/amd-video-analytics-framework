#!/usr/bin/env bash
# Usage: source mobileclip-env.sh /path/to/ryzen-ai-venv [gpu|npu|gpu+npu]
if [[ -z "${1:-}" || ! -f "$1/bin/activate" || ! -d "$1/onnxruntime/lib" ]]; then
    echo 'Pass the prepared Ryzen AI ORT SDK environment.' >&2
    return 1
fi
_amdva_venv="$(realpath -- "$1")"
_amdva_backend="${2:-npu}"
case "$_amdva_backend" in gpu|npu|gpu+npu) ;; *) echo 'Invalid MobileCLIP backend' >&2; return 1 ;; esac
export AMDVA_ORT_ROOT="$_amdva_venv/onnxruntime"
if /usr/bin/python3 -c 'import platform,sys; sys.exit(tuple(map(int, platform.libc_ver()[1].split(".")[:2])) < (2,41))'; then
    export GLIBC_TUNABLES="${GLIBC_TUNABLES:+$GLIBC_TUNABLES:}glibc.rtld.execstack=2"
fi
if [[ "$_amdva_backend" != gpu ]]; then
    source /opt/xilinx/xrt/setup.sh >/dev/null || return 1
    source "$_amdva_venv/bin/activate" || return 1
else
    unset XILINX_XRT
fi
# Reconstruct rather than inheriting activation's saved Torch/other-SDK paths.
export LD_LIBRARY_PATH="$ROCM_PATH/lib:$AMDVA_MIGRAPHX_ROOT/lib:$AMDVA_MIGRAPHX_ROOT/lib/migraphx/lib:$AMDVA_ROOT/build:$AMDVA_ROOT/.deps/prefix/lib:$AMDVA_ORT_ROOT/lib"
if [[ "$_amdva_backend" != gpu ]]; then
    _amdva_site="$_amdva_venv/lib/python3.12/site-packages"
    _amdva_python_lib="$("$_amdva_venv/bin/python" -c 'import sysconfig; print(sysconfig.get_config_var("LIBDIR"))')" || return 1
    export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/opt/xilinx/xrt/lib:$_amdva_python_lib:$_amdva_site/flexml/flexml_extras/lib:$_amdva_site/onnxruntime/capi:$_amdva_site/voe/lib"
fi
unset _amdva_venv _amdva_backend _amdva_site _amdva_python_lib
