#!/usr/bin/env bash
#
# 深目录与队列分页的端到端回归（架构反转后）
#
# 覆盖：
#   A6  深目录：≥7 层目录树不会把扫描/启动搞崩（历史上这里是栈溢出）；
#       前端扫描、后端只收路径队列。
#   队列分页：条目数超过单页上限（RPC_PAGE_MAX=1000）时 Queue.Get 仍逐页
#       返回、总数正确、越界页为空——1 万曲目级别的前端翻页依赖这一点。
#
# 用法：scripts/test/paging-deepdir-e2e.sh [--bin <ter-music>] [--keep]

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

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-page-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$XDG_CONFIG_HOME"

command -v ffmpeg >/dev/null 2>&1 || { echo "错误：需要 ffmpeg 造素材" >&2; exit 2; }

info "造素材：12 层深目录 + 20 首曲目"
DEEP="$WORK_DIR/music"
for level in 1 2 3 4 5 6 7 8 9 10 11 12; do
    DEEP="$DEEP/level$level"
done
mkdir -p "$DEEP"
ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=2" \
    -c:a pcm_s16le -y "$DEEP/deep-tone.wav" 2>/dev/null

for i in $(seq 1 20); do
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "sine=frequency=$((200 + i * 20)):duration=1" \
        -c:a pcm_s16le -y "$WORK_DIR/music/song$(printf '%02d' "$i").wav" 2>/dev/null
done

tui_probe() {   # 在 pty 里起 TUI 并读屏，返回 rc
    rm -f /tmp/tm-life-tui.rc
    timeout 60 python3 "$SCRIPT_DIR/lifecycle-driver.py" run \
        "$TM_BIN" "$1" "j" "6" >"$WORK_DIR/tui.out" 2>/dev/null
}

info "A6 12 层深目录：前端扫描后启动不应崩溃"
tui_probe "$WORK_DIR/music"
if [ -s "$WORK_DIR/tui.out" ]; then
    ok "深目录下 TUI 渲染出画面（$(wc -c < "$WORK_DIR/tui.out") 字节）"
else
    bad "深目录下 TUI 没有输出"
fi

queue_count="$("$TM_BIN" show --json 2>/dev/null | python3 -c '
import json,sys
try: print(json.load(sys.stdin)["track"]["queue_count"])
except Exception: print("?")' 2>/dev/null)"
if [ "$queue_count" = "21" ]; then
    ok "深目录中的曲目也被扫描到并推给核心（21 条）"
else
    bad "队列条目数=$queue_count（期望 21）"
fi

info "队列分页：总量与越界页"
total="$("$TM_BIN" show --json 2>/dev/null | python3 -c '
import json,sys
try: print(json.load(sys.stdin)["track"]["queue_count"])
except Exception: print("?")' 2>/dev/null)"
[ "$total" = "21" ] && ok "Queue.Get 报告的总数与内容一致（$total）" || bad "总数=$total"

page_first="$(python3 "$SCRIPT_DIR/rpc_client.py" call org.yxzl.ter_music.Queue.Get 0 5 2>/dev/null)"
rows_first="$(printf '%s' "$page_first" | python3 -c '
import json,sys
try: print(len(json.load(sys.stdin)["rows"]))
except Exception: print("?")' 2>/dev/null)"
[ "$rows_first" = "5" ] && ok "首页 5 行" || bad "首页行数=$rows_first"

page_last="$(python3 "$SCRIPT_DIR/rpc_client.py" call org.yxzl.ter_music.Queue.Get 20 5 2>/dev/null)"
rows_last="$(printf '%s' "$page_last" | python3 -c '
import json,sys
try: print(len(json.load(sys.stdin)["rows"]))
except Exception: print("?")' 2>/dev/null)"
[ "$rows_last" = "1" ] && ok "末页只剩 1 行（不越界）" || bad "末页行数=$rows_last"

page_over="$(python3 "$SCRIPT_DIR/rpc_client.py" call org.yxzl.ter_music.Queue.Get 1000 5 2>/dev/null)"
rows_over="$(printf '%s' "$page_over" | python3 -c '
import json,sys
try: print(len(json.load(sys.stdin)["rows"]))
except Exception: print("?")' 2>/dev/null)"
[ "$rows_over" = "0" ] && ok "越界页返回 0 行" || bad "越界页行数=$rows_over"

info "单页上限：count 超过 RPC_PAGE_MAX 必须被拒绝"
over="$(python3 "$SCRIPT_DIR/rpc_client.py" call org.yxzl.ter_music.Queue.Get 0 5000 2>&1)"
if printf '%s' "$over" | grep -q "InvalidArgs"; then
    ok "超过单页上限的 count 被拒绝（InvalidArgs）"
else
    bad "超大 count 未被拒绝：$(printf '%s' "$over" | head -c 80)"
fi

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
