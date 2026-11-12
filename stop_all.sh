#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
RUN="$ROOT/artifacts/video-demo/run"
if [[ ! -d "$RUN" ]]; then echo '管理対象のプロセスはありません。'; exit 0; fi
exec 9>"$RUN/control.lock"
flock -n 9 || { echo '起動・停止処理が実行中です。' >&2; exit 1; }
alive() {
    local pid start expected current exe
    [[ -f "$1" ]] || return 1
    read -r pid start expected < "$1" || return 1
    [[ "$pid" =~ ^[0-9]+$ && -r "/proc/$pid/stat" ]] || return 1
    current=$(awk '{print $22}' "/proc/$pid/stat")
    exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null) || return 1
    [[ "$start" == "$current" && "$exe" == "$expected" ]]
}
for name in nvr server; do
    file="$RUN/$name.pid"
    if alive "$file"; then
        read -r pid _ < "$file"
        kill -TERM "$pid"
        for ((i=0; i<30; i++)); do if ! alive "$file"; then break; fi; sleep 1; done
        if alive "$file"; then echo "$name の停止待ちがタイムアウトしました (PID $pid)。" >&2; exit 1; fi
        echo "$name を停止しました。"
    else
        echo "$name は停止しています。"
    fi
    rm -f -- "$file"
done
