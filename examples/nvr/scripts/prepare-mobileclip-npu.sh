#!/usr/bin/env bash
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$repo/scripts/env.sh"
source "$repo/examples/nvr/scripts/mobileclip-env.sh" "$1"
# Use the user-selected Ryzen AI interpreter. The source script is locked and
# hash checked by prepare_demo.py. Native inference never imports this module.
"$1/bin/python" "$repo/scripts/npu_environment.py" check "$2"
"$1/bin/python" "$3" npu --bundle "$2"
"$1/bin/python" "$repo/scripts/npu_environment.py" seal "$2"
