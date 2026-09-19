#!/usr/bin/env bash
#
# 前/后端生命周期端到端回归（架构反转后）
#
# 覆盖：
#   A1  TUI 默认（--frontend=remote）自动确保核心在跑并把内容推过去；
#       --attach-only 在没有核心时以退出码 3 明确失败。
#   A5  前端 q 退出后核心继续播放；core_exit_when_no_frontend=true 时，
#       最后一个前端离开并过宽限期后核心自行退出（无人接入过则不退）。
#
# 用法：scripts/test/lifecycle-e2e.sh [--bin <ter-music>] [--keep]
# 环境：需要会话总线（未设置时用 dbus-run-session）；需要 ffmpeg 造素材。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""

usage() {
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

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


daemon_up() { "$TM_BIN" daemon status 2>/dev/null | grep -q "pid"; }

cleanup() {
    "$TM_BIN" daemon stop >/dev/null 2>&1 || true
    for _ in $(seq 1 20); do
        daemon_up || break
        sleep 0.1
    done
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先用 --bin 指定）" >&2; exit 2; }

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-life-XXXXXX")"
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

DRIVER="$SCRIPT_DIR/lifecycle-driver.py"

tui_session() {   # $1 = keys, $2 = 秒
    timeout 60 python3 "$DRIVER" run "$TM_BIN" "$WORK_DIR/music" "${1:-q}" "${2:-6}" 2>/dev/null
}

# ── A1：默认远端前端自动确保核心 ─────────────────────────────────
info "A1 默认前端（remote）自动确保核心在跑并接入"
tui_session "j" 6 >/dev/null

show_json="$("$TM_BIN" show --json 2>/dev/null)"
queue_count="$(printf '%s' "$show_json" | python3 -c '
import json,sys
try:
    print(json.load(sys.stdin)["track"]["queue_count"])
except Exception:
    print("?")' 2>/dev/null)"
if [ "$queue_count" = "3" ]; then
    ok "前端把 3 条内容推给了核心（queue_count=3）"
else
    bad "前端的队列没有到达核心（queue_count=$queue_count）"
fi

mode="$(printf '%s' "$show_json" | python3 -c '
import json,sys
try:
    print(json.load(sys.stdin)["instance"]["mode"])
except Exception:
    print("?")' 2>/dev/null)"
if [ "$mode" = "daemon" ]; then
    ok "核心以 daemon 形态常驻（instance.mode=daemon）"
else
    bad "期望核心为 daemon，实际 '$mode'"
fi

# ── A5：前端退出，核心继续 ───────────────────────────────────────
info "A5 前端退出后核心继续播放"
if daemon_up; then
    ok "TUI 退出后核心仍在运行"
else
    bad "TUI 退出后核心也没了"
fi

# ── A1：--attach-only ────────────────────────────────────────────
info "A1 --attach-only 在没有核心时以退出码 3 失败"
"$TM_BIN" daemon stop >/dev/null 2>&1
for _ in $(seq 1 20); do daemon_up || break; sleep 0.1; done

rm -f /tmp/tm-life-tui.rc
attach_out="$(timeout 30 python3 "$DRIVER" tui "$TM_BIN" "$WORK_DIR/music" --attach-only 2>&1)"
attach_rc="$(cat /tmp/tm-life-tui.rc 2>/dev/null)"
if [ "$attach_rc" = "3" ]; then
    ok "--attach-only 无核心时退出码 3"
else
    bad "--attach-only 期望退出码 3，实际 '$attach_rc'"
fi
if printf '%s' "$attach_out" | grep -q "attach-only"; then
    ok "--attach-only 给出明确提示"
else
    bad "--attach-only 未给出明确提示"
fi

# ── core_exit_when_no_frontend ──────────────────────────────────
info "A5 core_exit_when_no_frontend=true 时核心随最后前端退出"
mkdir -p "$XDG_CONFIG_HOME/ter-music"
cat > "$XDG_CONFIG_HOME/ter-music/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="7">
  <preferences>
    <core_exit_when_no_frontend>1</core_exit_when_no_frontend>
    <volume_percent>80</volume_percent>
  </preferences>
</ter-music-config>
EOF

"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1.5
if daemon_up; then
    ok "启用看门狗后核心照常启动"
else
    bad "启用看门狗后核心启动失败"
fi

info "  没有前端接入过时，看门狗不得误杀核心"
sleep 11
if daemon_up; then
    ok "无人接入时核心保持存活（宽限条件：曾 Attach 过）"
else
    bad "核心在无人接入时被误杀"
fi

tui_session "q" 5 >/dev/null
sleep 1
if daemon_up; then
    ok "前端刚离开时核心仍在宽限期内"
else
    bad "宽限期内核心就退出了"
fi

sleep 12
if daemon_up; then
    bad "宽限期后核心应随最后前端退出"
else
    ok "最后一个前端离开并过宽限期后，核心自行退出"
fi

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
