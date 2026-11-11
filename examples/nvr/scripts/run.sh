#!/usr/bin/env bash
# Checkout launcher. Configure native provider dependencies before the NVR loads HIP/ORT.
set -euo pipefail
nvr_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
mode=""
case "${1:-}" in
    --soak) mode="$1"; seconds="${2:?Missing duration}"; shift 2 ;;
    --fullscreen|--validate) mode="$1"; shift ;;
esac
if [[ $# != 1 ]]; then
    echo 'Usage: run.sh [--fullscreen|--validate] CONFIG.json' >&2
    exit 2
fi
config="$(realpath -- "$1")"
if [[ ${AMDVA_ROOT:-} != "$nvr_root" ]]; then
    source "$nvr_root/scripts/env.sh"
fi
# Read config as data; no interpolation into shell code. Relative venv paths follow config.
mapfile -t search_environment < <(python3 - "$config" <<'PY'
import json, os, sys
from pathlib import Path
config = Path(sys.argv[1])
search = json.loads(config.read_text()).get('search', {})
print(search.get('embedding_model', 'embeddinggemma2') if search.get('enabled', False) else '')
print(search.get('mobileclip_backend', 'gpu'))
venv = search.get('ryzen_ai_venv') or os.environ.get('AMDVA_RYZEN_AI_VENV') or os.environ.get('VIRTUAL_ENV', '')
if venv and not Path(venv).is_absolute():
    venv = str(config.parent / venv)
print(venv)
PY
)
if [[ ${#search_environment[@]} != 3 ]]; then
    echo "Cannot read search environment from $config" >&2
    exit 2
fi
host_args=(--runtime)
if [[ ${search_environment[0]} == mobileclip2-s2 && -n ${search_environment[2]} ]]; then
    host_args+=(--ryzen-ai-venv "${search_environment[2]}")
    if [[ ${search_environment[1]} == gpu ]]; then host_args+=(--ort-only); fi
fi
/usr/bin/python3 "$nvr_root/scripts/host.py" "${host_args[@]}"
if [[ ${search_environment[0]} == mobileclip2-s2 ]]; then
    if [[ -n ${search_environment[2]} ]]; then
        source "$nvr_root/examples/nvr/scripts/mobileclip-env.sh" "${search_environment[2]}" "${search_environment[1]}"
    elif [[ ${search_environment[1]} != gpu ]]; then
        echo 'Set search.ryzen_ai_venv or AMDVA_RYZEN_AI_VENV to the Ryzen AI venv before using the NPU.' >&2
        exit 2
    fi
fi
case "$mode" in
    --soak) exec "$nvr_root/build/examples/nvr/amdva-demo-soak" "$config" "$seconds" "$(dirname "$config")/logs/soak.jsonl" ;;
    --validate) exec "$nvr_root/build/examples/nvr/amdva-mobileclip-nvr-validate" "$config" ;;
    --fullscreen) exec "$nvr_root/build/examples/nvr/amdva-nvr" --fullscreen "$config" ;;
    *) exec "$nvr_root/build/examples/nvr/amdva-nvr" "$config" ;;
esac
