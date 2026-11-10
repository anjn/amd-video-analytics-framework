#!/usr/bin/env bash
# CTest wrapper: provider dependencies must be configured before process startup.
set -euo pipefail
amdva_npu_test_venv="$1"
shift
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
source "$AMDVA_ROOT/examples/nvr/scripts/mobileclip-env.sh" "$amdva_npu_test_venv"
exec "$@"
