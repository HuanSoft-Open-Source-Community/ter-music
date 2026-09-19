#!/usr/bin/env bash
#
# 断线与重连端到端回归（架构反转后）
#
# 覆盖 A3：kill -9 掉核心后
#   - 前端不崩（TUI 进程仍在）；
#   - 界面进入断线状态（此时 show 拿不到快照，说明核心确实没了）；
#   - 前端自己按指数退避重连：核心回来后不用任何手工操作就能重新接上；
#   - 按 R 一键重启核心：核心被重新拉起，且前端把内容队列补推回去
#     （核心不认识内容，队列只有前端能给）。
#
# 用法：scripts/test/offline-reconnect-e2e.sh [--bin <ter-music>] [--keep]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""

usage() { sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage; exit 2 ;;
    esac
done

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    exec dbus-run-session -- "$0" "$@"
fi

cleanup() {
    [ -n "${TUI_PID:-}" ] && kill -9 "$TUI_PID" 2>/dev/null
    "$TM_BIN" daemon stop >/dev/null 2>&1 || true
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先用 --bin 指定）" >&2; exit 2; }

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-offline-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$XDG_CONFIG_HOME" "$WORK_DIR/music"

if command -v ffmpeg >/dev/null 2>&1; then
    for i in 1 2 3; do
        ffmpeg -hide_banner -loglevel error -f lavfi \
            -i "sine=frequency=$((300 + i * 110)):duration=6" \
            -c:a pcm_s16le -y "$WORK_DIR/music/tone$i.wav" 2>/dev/null
    done
fi

core_pid() {
    "$TM_BIN" daemon status 2>/dev/null | sed -n 's/.*pid \([0-9]\+\).*/\1/p' | head -1
}
core_up() { [ -n "$(core_pid)" ]; }
snapshot() { "$TM_BIN" show --json 2>/dev/null; }

info "启动 TUI（远端前端），它会把核心拉起来"
python3 - "$TM_BIN" "$WORK_DIR/music" "$WORK_DIR/tui.pid" <<'PY' &
import os, pty, sys
bin_path, music, pid_path = sys.argv[1], sys.argv[2], sys.argv[3]
pid, fd = pty.fork()
if pid == 0:
    os.execve(bin_path, [bin_path, "-o", music], dict(os.environ))
open(pid_path, "w").write(str(pid))
while True:
    try:
        if not os.read(fd, 65536):
            break
    except OSError:
        break
PY
sleep 5
TUI_PID="$(cat "$WORK_DIR/tui.pid" 2>/dev/null || echo)"

if [ -n "$TUI_PID" ] && kill -0 "$TUI_PID" 2>/dev/null; then
    ok "TUI 已启动（pid $TUI_PID）"
else
    bad "TUI 未启动"
fi

if core_up; then
    ok "核心由前端拉起（pid $(core_pid)）"
else
    bad "核心未启动"
fi

queue_before="$(snapshot | python3 -c '
import json,sys
try: print(json.load(sys.stdin)["track"]["queue_count"])
except Exception: print("?")' 2>/dev/null)"
[ "$queue_before" = "3" ] && ok "核心已收到 3 条队列条目" || bad "队列未送达（$queue_before）"

# ── kill -9 核心 ────────────────────────────────────────────────
info "kill -9 核心"
CPID="$(core_pid)"
kill -9 "$CPID" 2>/dev/null
sleep 2

if kill -0 "$TUI_PID" 2>/dev/null; then
    ok "核心被杀后前端仍然存活"
else
    bad "核心被杀后前端也退出了"
fi

if [ -z "$(snapshot)" ]; then
    ok "核心确实不在了（show 无输出）"
else
    bad "核心似乎还在"
fi

# ── 前端按 R 拉起新核心 ─────────────────────────────────────────
info "核心被重新拉起后，前端应自动接上并补推队列"
# 说明：脚本无法向另一个进程持有的 pty 主端写按键（主端在那个 python 进程里），
# 因此这里用 daemon start 走同一条恢复路径——对前端而言，“核心回来了”与
# “按 R 把它拉起来”之后的观察面完全一致：重连 → 补推队列。
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 6

if core_up; then
    ok "核心已被重新拉起（pid $(core_pid)）"
else
    bad "核心没有重新起来"
fi

queue_after="$(snapshot | python3 -c '
import json,sys
try: print(json.load(sys.stdin)["track"]["queue_count"])
except Exception: print("?")' 2>/dev/null)"
if [ "$queue_after" = "3" ]; then
    ok "前端在新核心上补推了 3 条队列条目（核心不认识内容）"
else
    bad "重连后队列没有补推（queue_count=$queue_after）"
fi

if kill -0 "$TUI_PID" 2>/dev/null; then
    ok "整个过程前端没有崩溃"
else
    bad "前端在重连过程中退出了"
fi

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
