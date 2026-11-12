#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
DEMO="$ROOT/artifacts/video-demo"
RUN="$DEMO/run"
SERVER="$ROOT/build/examples/rtsp-files/amdva-rtsp-files"
NVR="$ROOT/build/examples/nvr/amdva-nvr"
BASE="$DEMO/nvr-rtsp-videos.json"
LIST="$DEMO/rtsp-files.txt"
mkdir -p "$RUN"
exec 9>"$RUN/control.lock"
flock -n 9 || { echo '起動・停止処理が実行中です。' >&2; exit 1; }
alive() {
    local pid start expected current exe
    [[ -f "$1" ]] || return 1
    read -r pid start expected < "$1" || return 1
    [[ "$pid" =~ ^[0-9]+$ && -r "/proc/$pid/stat" ]] || return 1
    current=$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null) || return 1
    exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null) || return 1
    [[ "$start" == "$current" && "$exe" == "$expected" ]]
}
for item in server nvr; do
    if alive "$RUN/$item.pid"; then echo "$item は既に起動中です。./stop_all.sh で停止してください。" >&2; exit 1; fi
done
for f in "$SERVER" "$NVR" "$BASE" "$LIST"; do
    [[ -f "$f" ]] || { echo "必要なファイルがありません: $f" >&2; exit 1; }
done
python3 - "$BASE" "$LIST" "$RUN/config.json" "$DEMO" <<'PY'
import json, pathlib, sys
base, listing, output, demo = map(pathlib.Path, sys.argv[1:])
c = json.loads(base.read_text())
files = [line.strip() for line in listing.read_text().splitlines() if line.strip() and not line.lstrip().startswith('#')]
if len(files) != 16 or len(c.get('sources', [])) != 16:
    raise SystemExit('動画一覧と NVR 設定には16件必要です。')
for i, (f, src) in enumerate(zip(files, c['sources'])):
    p = pathlib.Path(f)
    if not p.is_absolute(): p = listing.parent / p
    if not p.is_file(): raise SystemExit(f'動画がありません: {p}')
    if src.get('uri') != f'rtsp://127.0.0.1:18558/cam{i}':
        raise SystemExit(f'RTSP URL が一覧と一致しません: {src.get("uri")}')
for field in ('model', 'single_model', 'model_info'):
    if c.get(field):
        p = pathlib.Path(c[field])
        if not p.is_absolute(): p = base.parent / p
        if not p.is_file(): raise SystemExit(f'モデル関連ファイルがありません: {p}')
        c[field] = str(p.resolve())
# The GUI clears its recording root at startup; a fresh directory preserves prior runs.
import datetime
c['recording_root'] = str(demo / 'recordings' / (datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')))
output.write_text(json.dumps(c, ensure_ascii=False, indent=2) + '\n')
PY
source "$ROOT/scripts/env.sh"
start_process() {
    local name=$1 bin=$2; shift 2
    nohup "$bin" "$@" >"$RUN/$name.log" 2>&1 9>&- < /dev/null &
    local pid=$! start
    start=$(awk '{print $22}' "/proc/$pid/stat")
    printf '%s %s %s\n' "$pid" "$start" "$(readlink -f "$bin")" > "$RUN/$name.pid"
}
start_process server "$SERVER" --port 18558 --list "$LIST"
sleep 0.2
ready=false
for ((i=0; i<60; i++)); do
    if ! alive "$RUN/server.pid"; then echo "RTSP サーバーが起動できません。ログ: $RUN/server.log" >&2; exit 1; fi
    if [[ $(grep -c '^rtsp://127.0.0.1:18558/cam[0-9]*$' "$RUN/server.log" || true) -eq 16 ]]; then ready=true; break; fi
    sleep 1
done
if [[ "$ready" != true ]]; then
    echo "RTSP サーバーの準備がタイムアウトしました。ログ: $RUN/server.log" >&2
    if alive "$RUN/server.pid"; then read -r pid _ < "$RUN/server.pid"; kill -TERM "$pid"; fi
    exit 1
fi
mapfile -t npu_env < <(python3 -c 'import json,sys; s=json.load(open(sys.argv[1])).get("search", {}); print("yes" if s.get("enabled") and s.get("embedding_model")=="mobileclip2-s2" and s.get("mobileclip_backend") in ("npu", "gpu+npu") else "no"); print(s.get("ryzen_ai_venv", ""))' "$RUN/config.json")
if [[ "${npu_env[0]}" == yes ]]; then
    ryzen_venv="${npu_env[1]}"
    if [[ -z "$ryzen_venv" ]] || ! source "$ROOT/examples/nvr/scripts/mobileclip-env.sh" "$ryzen_venv" >/dev/null; then
        echo 'Ryzen AI の実行環境を準備できません。' >&2
        if alive "$RUN/server.pid"; then read -r pid _ < "$RUN/server.pid"; kill -TERM "$pid"; fi
        exit 1
    fi
    python_library=$(python -c 'import os,sysconfig; print(os.path.join(sysconfig.get_config_var("LIBDIR"), sysconfig.get_config_var("LDLIBRARY")))')
    python_libdir=$(dirname -- "$python_library")
    if [[ ! -f "$python_library" ]]; then
        echo "Python 共有ライブラリがありません: $python_library" >&2
        if alive "$RUN/server.pid"; then read -r pid _ < "$RUN/server.pid"; kill -TERM "$pid"; fi
        exit 1
    fi
    export LD_LIBRARY_PATH="$python_libdir:$LD_LIBRARY_PATH"
    export LD_PRELOAD="$python_library${LD_PRELOAD:+:$LD_PRELOAD}"
fi
start_process nvr "$NVR" "$RUN/config.json"
sleep 3
if ! alive "$RUN/nvr.pid"; then
    echo "NVR が起動できません。ログ: $RUN/nvr.log" >&2
    if alive "$RUN/server.pid"; then read -r pid _ < "$RUN/server.pid"; kill -TERM "$pid"; fi
    exit 1
fi
printf '16動画の RTSP 配信と NVR を起動しました。\nログ: %s\n停止: %s/stop_all.sh\n' "$RUN" "$ROOT"
