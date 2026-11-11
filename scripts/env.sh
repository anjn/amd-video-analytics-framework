#!/usr/bin/env bash
# Source in Bash. Only the selected host SDK and framework prefix are exposed.
AMDVA_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export AMDVA_ROOT
if [[ ! -f "$AMDVA_ROOT/.deps/host.json" ]]; then
    echo 'Run python3 scripts/host.py --write before sourcing scripts/env.sh' >&2
    return 1
fi
mapfile -t _amdva_roots < <(/usr/bin/python3 - "$AMDVA_ROOT/.deps/host.json" <<'PY'
import json, sys
x=json.load(open(sys.argv[1]))
print(x['rocm_root']); print(x['migraphx_root'])
PY
)
if [[ ${#_amdva_roots[@]} != 2 ]]; then return 1; fi
export AMDVA_ROCM_ROOT="${_amdva_roots[0]}"
export AMDVA_MIGRAPHX_ROOT="${_amdva_roots[1]}"
export ROCM_PATH="$AMDVA_ROCM_ROOT" HIP_PATH="$AMDVA_ROCM_ROOT"
export HIP_CLANG_PATH="$AMDVA_ROCM_ROOT/lib/llvm/bin"
# Do not inherit another project's SDK, Torch, pkg-config, or plugin search paths.
export PATH="$AMDVA_ROOT/.deps/prefix/bin:$AMDVA_ROOT/.deps/venv/bin:$AMDVA_ROCM_ROOT/bin:$HOME/.cargo/bin:/usr/local/bin:/usr/bin:/bin"
export PKG_CONFIG_PATH="$AMDVA_ROOT/.deps/prefix/lib/pkgconfig"
export LD_LIBRARY_PATH="$AMDVA_ROOT/build:$AMDVA_ROOT/.deps/prefix/lib:$AMDVA_ROCM_ROOT/lib:$AMDVA_MIGRAPHX_ROOT/lib:$AMDVA_MIGRAPHX_ROOT/lib/migraphx/lib"
export GST_PLUGIN_PATH="$AMDVA_ROOT/build/plugins:$AMDVA_ROOT/.deps/prefix/lib/gstreamer-1.0"
export GST_PLUGIN_SYSTEM_PATH="$AMDVA_ROOT/.deps/prefix/lib/gstreamer-1.0"
export GST_REGISTRY="$AMDVA_ROOT/.deps/gst-registry.bin"
export CMAKE_PREFIX_PATH="$AMDVA_ROOT/.deps/prefix:$AMDVA_ROCM_ROOT:$AMDVA_MIGRAPHX_ROOT"
unset HSA_OVERRIDE_GFX_VERSION HSA_ENABLE_SDMA LD_PRELOAD PYTHONPATH PYTHONHOME QT_PLUGIN_PATH BISON_PKGDATADIR
unset PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR CPATH CPLUS_INCLUDE_PATH LIBRARY_PATH
unset QT_QPA_PLATFORM_PLUGIN_PATH GST_PLUGIN_SYSTEM_PATH_1_0 GST_PLUGIN_PATH_1_0
export GPU_MAX_HW_QUEUES="${GPU_MAX_HW_QUEUES:-32}"
export QT_XCB_GL_INTEGRATION=xcb_egl
unset _amdva_roots
