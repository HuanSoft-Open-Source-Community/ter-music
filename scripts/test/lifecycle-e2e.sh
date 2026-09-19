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

# ── A7：核心随总线消失而退出 ──────────────────────────────────────
# 总线没了 = 再也没有前端能联系到这个核心；若它继续播放，就变成一个谁也
# 停不掉、`daemon stop` 也看不见的孤儿播放进程（本轮实测踩到过）。这条
# 断言需要一条**会死掉**的总线，所以在一层嵌套的私有会话里起核心。
info "A7 核心所属的会话总线消失后必须自行退出"

core_alive() {   # $1 = pid：既要存在，也要确实是 ter-music 核心（防 pid 复用误判）
    kill -0 "$1" 2>/dev/null || return 1
    ps -p "$1" -o cmd= 2>/dev/null | grep -q "ter-music"
}

nested_home="$WORK_DIR/nested-home"
mkdir -p "$nested_home/.config"
rm -f "$WORK_DIR/nested.pid"
timeout 30 dbus-run-session -- bash -c '
    export HOME="$1" XDG_CONFIG_HOME="$1/.config"
    out="$("$2" daemon start 2>&1)"
    printf "%s\n" "$out" | sed -n "s/.*pid \([0-9]\{1,\}\).*/\1/p" | head -1 > "$3"
    sleep 2
' _ "$nested_home" "$TM_BIN" "$WORK_DIR/nested.pid" >/dev/null 2>&1 || true

nested_pid="$(cat "$WORK_DIR/nested.pid" 2>/dev/null)"
if [ -z "$nested_pid" ]; then
    bad "未能记录嵌套会话里的核心 pid（bus-loss 场景无法判定）"
else
    nested_exited=0
    for _ in $(seq 1 40); do          # 最多等 10 秒
        core_alive "$nested_pid" || { nested_exited=1; break; }
        sleep 0.25
    done
    if [ "$nested_exited" -eq 1 ]; then
        ok "总线消失后核心自行退出（pid $nested_pid）"
    else
        bad "总线消失后核心仍在运行（pid $nested_pid），会成为无人可控的孤儿"
        kill -9 "$nested_pid" 2>/dev/null
    fi
fi

# ── A4″：次要实例可见、可一次收尾 ────────────────────────────────
# `daemon start --force` 起的第二个核心是**有文档的功能**，但它不接收普通
# CLI 命令：必须能从 `daemon status` 看见、能一次收干净，否则就成了“后台
# 莫名其妙有两个 ter-music 在跑”。
info "A4″ --force 起的次要实例可见、可一次收尾"

"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1.5
"$TM_BIN" daemon start --force >"$WORK_DIR/force.out" 2>"$WORK_DIR/force.err" || true

if grep -q "instance" "$WORK_DIR/force.err" 2>/dev/null; then
    ok "--force 启动时报出次要实例的总线名（可据此 --bus 控制）"
else
    bad "--force 启动时未报出次要实例的总线名：$(tail -2 "$WORK_DIR/force.err" | tr '\n' ' ')"
fi

if "$TM_BIN" daemon status 2>&1 >/dev/null | grep -q "次要实例"; then
    ok "daemon status 会提示次要实例（含 pid 与总线名）"
else
    bad "daemon status 没有提示次要实例"
fi

status_pids="$("$TM_BIN" daemon status 2>/dev/null | grep -c "pid")"
if [ "$status_pids" = "1" ]; then
    ok "daemon status 的 stdout 未被次要实例污染（仍是 1 行 pid，脚本解析不受影响）"
else
    bad "daemon status 的 stdout 出现 $status_pids 行 pid（应为 1）"
fi

# 提示里给出的控制方式必须真的可用：`--bus` 写在**子命令之后**
secondary_bus="$(sed -n 's/^次要实例总线名：\(org[^ ]*\)$/\1/p' "$WORK_DIR/force.err" 2>/dev/null | head -1)"
if [ -z "$secondary_bus" ]; then
    secondary_bus="$("$TM_BIN" daemon status 2>&1 >/dev/null |
        sed -n 's/.*pid [0-9]\{1,\}  [^ ]*  \(org[^ ]*\)$/\1/p' | head -1)"
fi
if [ -z "$secondary_bus" ]; then
    bad "未能取到次要实例的总线名（无法验证 --bus 控制路径）"
else
    if "$TM_BIN" daemon stop --bus "$secondary_bus" >/dev/null 2>&1; then
        ok "ter-music daemon stop --bus <次要实例> 能单独停掉它"
    else
        bad "ter-music daemon stop --bus $secondary_bus 失败"
    fi
    if "$TM_BIN" daemon status >/dev/null 2>&1; then
        ok "--bus 只停次要实例，主实例不受影响"
    else
        bad "次要实例被 --bus 停掉时主实例也一起没了"
    fi
    "$TM_BIN" daemon start --force >/dev/null 2>&1 || true   # 后面还要一个次要实例
    sleep 1.5
fi

"$TM_BIN" daemon stop >/dev/null 2>&1
sleep 2
status_err="$("$TM_BIN" daemon status 2>&1 >/dev/null || true)"
if printf '%s' "$status_err" | grep -q "主实例未在运行，但仍有"; then
    ok "主实例退出后，次要实例仍被明确指出（这正是最容易变成孤儿的形态）"
else
    bad "主实例退出后没有提示残留的次要实例：$(printf '%s' "$status_err" | head -1)"
fi

if "$TM_BIN" daemon stop --all >/dev/null 2>&1; then
    ok "daemon stop --all 收掉主实例与次要实例"
else
    bad "daemon stop --all 失败（rc=$?）"
fi

"$TM_BIN" daemon status >/dev/null 2>&1
no_instance_rc=$?
if [ "$no_instance_rc" -eq 3 ]; then
    ok "--all 之后确实没有实例在运行（退出码 3）"
else
    bad "--all 之后仍有实例在运行（退出码 $no_instance_rc）"
fi

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
