#!/usr/bin/env bash
# Start only the fixture servers owned by this launcher; stop them with the NVR.
set -euo pipefail
profile="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$profile/../../../.." && pwd)"
mode=fullscreen
case "${1:-}" in
    --check) mode=check; shift ;;
    --windowed) mode=windowed; shift ;;
esac
if [[ $# != 1 ]]; then
    echo 'Usage: run.sh [--check|--windowed] RUNTIME_DIRECTORY' >&2
    exit 2
fi
runtime="$(realpath -- "$1")"
source "$repo/scripts/env.sh"
python3 "$profile/manage.py" check --runtime "$runtime"
for binary in "$repo/build/examples/rtsp-files/amdva-rtsp-files" "$repo/build/examples/resource-dashboard/amdva-dashboard" "$repo/build/examples/nvr/amdva-nvr"; do
    [[ -x "$binary" ]] || { echo "Build required executable: $binary" >&2; exit 1; }
done
[[ "$mode" != check ]] || exit 0
if [[ -z "${QT_QPA_PLATFORM:-}" ]]; then
    if [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
        export QT_QPA_PLATFORM=wayland-egl
    elif [[ -z "${DISPLAY:-}" ]]; then
        echo 'No desktop display configured. Set WAYLAND_DISPLAY/XDG_RUNTIME_DIR or DISPLAY (see README).' >&2
        exit 1
    fi
fi
mapfile -t options < <(python3 - "$runtime/demo.json" <<'PY'
import json, sys
settings = json.load(open(sys.argv[1]))
print(settings['rtsp_port'])
print(settings['dashboard_port'])
print(settings['cpu_name'])
PY
)
[[ ${#options[@]} == 3 ]] || { echo 'Invalid demo.json' >&2; exit 1; }
# Fail before starting any child if either fixed demo port is already occupied.
python3 - "${options[0]}" "${options[1]}" <<'PY'
import socket, sys
sockets = []
for port in map(int, sys.argv[1:]):
    sock = socket.socket()
    try:
        sock.bind(('127.0.0.1', port))
    except OSError as error:
        sys.exit(f"Demo port {port} is unavailable: {error}")
    sockets.append(sock)
PY
mkdir -p "$runtime/logs"
pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$repo/build/examples/rtsp-files/amdva-rtsp-files" --list "$runtime/videos.txt" --port "${options[0]}" > "$runtime/logs/rtsp.log" 2>&1 &
pids+=("$!")
dashboard_args=()
if [[ -n "${options[2]}" ]]; then dashboard_args+=(--cpu-name "${options[2]}"); fi
"$repo/build/examples/resource-dashboard/amdva-dashboard" --bind 127.0.0.1 --port "${options[1]}" "${dashboard_args[@]}" > "$runtime/logs/dashboard.log" 2>&1 &
pids+=("$!")
sleep 2
for pid in "${pids[@]}"; do kill -0 "$pid" || { echo "Server startup failed; see $runtime/logs" >&2; exit 1; }; done
nvr_args=()
if [[ "$mode" == fullscreen ]]; then nvr_args+=(--fullscreen); fi
# run.sh loads the configured Ryzen AI environment before HIP/ORT initialization.
"$repo/examples/nvr/scripts/run.sh" "${nvr_args[@]}" "$runtime/config.json" > "$runtime/logs/nvr.log" 2>&1 &
nvr_pid=$!
pids+=("$nvr_pid")
echo "NVR started; logs: $runtime/logs"
if wait "$nvr_pid"; then exit 0; else
    status=$?
    tail -n 20 "$runtime/logs/nvr.log" >&2
    exit "$status"
fi
