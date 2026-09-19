#!/usr/bin/env bash
#
# 多前端并存的端到端回归（架构反转后）
#
# 覆盖 A4′：
#   - 一个 TUI + 多个 CLI 命令同时存在：后端只有**一份**播放状态；
#   - 所有前端看到同一份队列与同一个游标；
#   - 两个前端（TUI 与 CLI）交替下发队列与命令时，内容库（SQLite）不损坏
#     （TUI 另一个进程里在扫描/读库，CLI 也在读，共写要活得下来）。
#
# 用法：scripts/test/multi-frontend-e2e.sh [--bin <ter-music>] [--keep]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""

usage() { sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# 原始参数：解析循环会 shift 掉 "$@"，重入私有总线时必须带着它
ORIG_ARGS=("$@")

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

# ── 独占的总线：默认起私有会话总线 ───────────────────────────────
# 只允许本脚本拉起的核心在总线上（否则前端会接到桌面残留的实例上）；需要复用
# 当前总线时显式传 TM_TEST_REUSE_SESSION_BUS=1。TM_TEST_BUS_READY 是再入标记。
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ "${TM_TEST_REUSE_SESSION_BUS:-0}" != "1" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：需要 dbus-run-session，或设 TM_TEST_REUSE_SESSION_BUS=1 复用当前会话总线。" >&2
        exit 2
    fi
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
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

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-multi-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$XDG_CONFIG_HOME" "$WORK_DIR/music"

if command -v ffmpeg >/dev/null 2>&1; then
    for i in 1 2 3 4; do
        ffmpeg -hide_banner -loglevel error -f lavfi \
            -i "sine=frequency=$((300 + i * 110)):duration=6" \
            -c:a pcm_s16le -y "$WORK_DIR/music/tone$i.wav" 2>/dev/null
    done
fi

json_field() {   # $1 = json, $2 = 取值表达式
    printf '%s' "$1" | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
    print(eval(sys.argv[1], {"doc": doc}))
except Exception:
    print("?")' "$2" 2>/dev/null
}

info "TUI（前端 A）拉起核心并推入 4 条内容"
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

snap="$(json_field "$("$TM_BIN" show --json 2>/dev/null)" 'doc["track"]["queue_count"]')"
[ "$snap" = "4" ] && ok "核心收到 4 条队列条目" || bad "队列条目数=$snap"

info "CLI（前端 B/C）并行下发命令，后端只应有一份状态"
"$TM_BIN" play "$WORK_DIR/music" >/dev/null 2>&1 || true
sleep 1.5
first="$(json_field "$("$TM_BIN" show --json 2>/dev/null)" 'doc["track"]["path"]')"

"$TM_BIN" next >/dev/null 2>&1
sleep 1.2
second="$(json_field "$("$TM_BIN" show --json 2>/dev/null)" 'doc["track"]["path"]')"
if [ -n "$first" ] && [ -n "$second" ] && [ "$first" != "$second" ]; then
    ok "CLI 与 TUI 看到同一份状态在推进（$first → ${second##*/}）"
else
    bad "状态没有推进（first=$first second=$second）"
fi

# 三个前端同时读，应当一致
a="$(json_field "$("$TM_BIN" show --json 2>/dev/null)" 'doc["track"]["queue_position"]')"
b="$(json_field "$("$TM_BIN" show --json 2>/dev/null)" 'doc["track"]["queue_position"]')"
"$TM_BIN" volume 30 >/dev/null 2>&1
c="$(json_field "$("$TM_BIN" show --json 2>/dev/null)" 'doc["playback"]["volume_percent"]')"
[ "$a" = "$b" ] && ok "同一时刻多次读取游标一致（$a）" || bad "游标读数不一致（$a vs $b）"
[ "$c" = "30" ] && ok "CLI 写入的音量对下一次读取立即可见（$c）" || bad "音量未生效（$c）"

info "TUI 退出后 CLI 仍然可用（核心独立于前端）"
kill -TERM "$TUI_PID" 2>/dev/null
sleep 1.5
TUI_PID=""
"$TM_BIN" next >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "TUI 退出后 CLI 仍能控制核心"
else
    bad "TUI 退出后 CLI 控制失败（rc=$rc）"
fi

info "内容库（SQLite）在多前端读写后仍然可用"
DB="$XDG_CONFIG_HOME/ter-music/library.db"
if [ ! -f "$DB" ]; then
    # 内容库属于前端：两种前端模式都必须打开并写入它（曾因"远端模式不初始化内容"
    # 导致收藏/历史/歌单/会话在退出时全部丢失，故这里缺失即失败）
    bad "没有生成内容库 $DB（前端未做内容持久化）"
elif python3 - "$DB" <<'PY'
import sqlite3, sys
db = sys.argv[1]
try:
    con = sqlite3.connect(db)
    rows = con.execute("PRAGMA integrity_check").fetchone()
    if not rows or rows[0] != "ok":
        print("integrity_check: %r" % (rows,), file=sys.stderr)
        sys.exit(1)
    tables = {name for (name,) in con.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    # 两个前端都留下痕迹：TUI 打开目录写目录历史，退出时会话落盘
    dir_rows = con.execute("SELECT count(*) FROM dir_history").fetchone()[0] if "dir_history" in tables else 0
    temp_rows = con.execute("SELECT count(*) FROM temp_playlist").fetchone()[0] if "temp_playlist" in tables else 0
    con.close()
    print("dir_history=%d temp_playlist=%d" % (dir_rows, temp_rows))
    sys.exit(0 if dir_rows >= 1 and temp_rows >= 1 else 2)
except SystemExit:
    raise
except Exception as exc:                                   # noqa: BLE001
    print("内容库检查失败：%s" % exc, file=sys.stderr)
    sys.exit(1)
PY
then
    ok "内容库在多个前端读写后仍可用，且两个前端都写入了内容（目录历史 + 会话）"
else
    bad "内容库缺失内容或完整性检查失败（期望 dir_history>=1 且 temp_playlist>=1）"
fi

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
