#!/usr/bin/env bash
#
# 前/后端生命周期端到端回归（架构反转后）
#
# 覆盖：
#   A1  TUI 默认（--frontend=remote）自动确保核心在跑并把内容推过去；
#       --attach-only 在没有核心时以退出码 3 明确失败。
#   A5  前端 q 退出后核心继续播放（前提：stop_core_on_exit 显式置 0；config v8
#       起缺省为 1，正常退出会发 Control.Quit 终止核心）；core_exit_when_no_frontend
#       =true 时，最后一个前端离开并过宽限期后核心自行退出（无人接入过则不退）。
#   A5′ 播放中按 F9 退出与 'q' 同路：前端没了，核心仍在总线、仍在播同一首
#       （同样以 stop_core_on_exit=0 为前提，见 A1 前的夹具注释）。
#   S8  stop_core_on_exit 缺省态（=1）：q 与 F9 正常退出后核心必须离线。
#   S9  没能接入核心的前端（--bus 指向一个不在线的名字）退出：既不改动总线上既有核心
#       的存活与曲目状态，也不留下新实例（详见 S9 段注释：本段不是接入门控的判别性测试）。
#   S10 收尾顺序不变量：核心释放 D-Bus 总线名之前，播放必须已经彻底停止
#       （顺序颠倒就会重现“名字没了、声音还在”的窗口 → 前端在该窗口里拉起第二个核心 → 叠播）。
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
    sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
SKIP=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
skip() { SKIP=$((SKIP + 1)); printf '  SKIP %s\n' "$1"; }
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
    [ -n "${F9_DRIVER_PID:-}" ] && kill -9 "$F9_DRIVER_PID" 2>/dev/null
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

# ── stop_core_on_exit=0 夹具：A5/A5′ 续播断言的前提 ────────────────────
# config v8 起缺省 stop_core_on_exit=1：remote 前端正常退出（q/F9/Ctrl+C/SIGTERM
# 收敛到 player_remote_shutdown）会发 Control.Quit 终止核心。A5/A5′ 要保留
# “前端没了、核心继续”的原语义，就必须在核心启动前把该开关显式写成 0
# （前端退出时经 Config.GetAll 实时读的就是它）。缺省态（=1）的对照断言
# 在脚本末尾 S8 段。
mkdir -p "$XDG_CONFIG_HOME/ter-music"
cat > "$XDG_CONFIG_HOME/ter-music/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <stop_core_on_exit>0</stop_core_on_exit>
  </preferences>
</ter-music-config>
EOF

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

# ── A5：前端退出，核心继续（夹具已置 stop_core_on_exit=0）────────
info "A5 前端退出后核心继续播放"
if daemon_up; then
    ok "TUI 退出后核心仍在运行"
else
    bad "TUI 退出后核心也没了"
fi

# ── A5′：F9 退出（与 q 对称，Task 9.5）───────────────────────────
# F9 在 menus.c 的 handle_function_keys() 里只置 g_should_exit=1，与 'q' 走同一条
# 正常退出路径；这里要在**正在播**的状态下验证：前端没了，核心必须还在总线上、
# 还在播同一首（不能像早期版本那样把后台音乐一起停掉）。
# 按键用 lifecycle-driver.py 的 drive 会话：F9 是 `\e[20~` 这样的多字节序列，
# 必须一次写进 pty，ncurses 才会解成 KEY_F(9)（`run` 按字符发键做不到）。
info "A5′ 播放中按 F9 退出：核心仍在总线且继续播放"

snap_field() {   # $1 = 取值表达式（doc 已绑定）
    "$TM_BIN" show --json 2>/dev/null | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
    print(eval(sys.argv[1], {"doc": doc}))
except Exception:
    print("?")' "$1" 2>/dev/null
}

F9_CTL="$WORK_DIR/f9.cmd"
: > "$F9_CTL"
python3 "$DRIVER" drive "$TM_BIN" "$WORK_DIR/music" "$WORK_DIR/f9-tui.log" \
    --ctl "$F9_CTL" --rc-file "$WORK_DIR/f9.rc" --pid-file "$WORK_DIR/f9.pid" \
    --max-runtime 60 >/dev/null 2>&1 &
F9_DRIVER_PID=$!

for _ in $(seq 1 40); do
    [ "$(snap_field 'doc["track"]["queue_count"]')" = "3" ] && break
    sleep 0.5
done

printf 'key:j\n' >> "$F9_CTL"          # 浏览器树首行是根目录节点：一次 j 落到第一个文件
sleep 1
printf 'key:<Enter>\n' >> "$F9_CTL"    # Enter 播放选中的文件（ui.c case 10）
sleep 2

f9_before="$(snap_field 'doc["playback"]["state"]')"
f9_track_before="$(snap_field 'doc["track"]["path"]')"
if [ "$f9_before" = "playing" ]; then
    ok "F9 之前确实在播（state=$f9_before，track=${f9_track_before##*/}）"
else
    bad "F9 之前没进入播放（state=$f9_before，track=${f9_track_before##*/}）"
fi

printf 'key:<F9>\n' >> "$F9_CTL"
for _ in $(seq 1 20); do
    kill -0 "$F9_DRIVER_PID" 2>/dev/null || break
    sleep 0.5
done
f9_pid="$(cat "$WORK_DIR/f9.pid" 2>/dev/null)"
if [ -n "$f9_pid" ] && kill -0 "$f9_pid" 2>/dev/null; then
    bad "按 F9 之后 TUI 仍在运行（退出键没生效）"
else
    f9_rc="$(cat "$WORK_DIR/f9.rc" 2>/dev/null)"
    if [ "$f9_rc" = "0" ]; then
        ok "按 F9 的 TUI 已正常退出（退出码 0，与 'q' 同一条路径）"
    else
        bad "按 F9 的 TUI 退出码不是 0（实测 '${f9_rc:-空}'）"
    fi
fi

if daemon_up; then
    ok "F9 退出后核心仍在总线上（daemon status 可见）"
else
    bad "F9 退出把核心也带走了（daemon status 查不到实例）"
fi
f9_after="$(snap_field 'doc["playback"]["state"]')"
f9_track_after="$(snap_field 'doc["track"]["path"]')"
if [ "$f9_after" = "playing" ] && [ "$f9_track_after" = "$f9_track_before" ]; then
    ok "F9 退出后核心仍在播同一首（state=$f9_after，track=${f9_track_after##*/}）"
else
    bad "F9 退出后播放被打断或换了曲目（state=$f9_after，track ${f9_track_before##*/} → ${f9_track_after##*/}）"
fi
kill -9 "$F9_DRIVER_PID" 2>/dev/null
F9_DRIVER_PID=""

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
# 属性 9 文件（低版本会让核心首启的迁移块把 stop_core_on_exit 强制补成 1）；
# stop_core_on_exit 显式写 0 是这一段成立的前提：若按缺省 1，前端 q 退出会直接
# Control.Quit 带走核心，“宽限期内核心仍在 / 过宽限期自行退出”就无从验证。
cat > "$XDG_CONFIG_HOME/ter-music/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <core_exit_when_no_frontend>1</core_exit_when_no_frontend>
    <stop_core_on_exit>0</stop_core_on_exit>
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

# ── S8：stop_core_on_exit 缺省态（=1），正常退出即终止核心 ──────────
# 回到“没有 config.xml”的出厂缺省（缺省 1）。与 A5/A5′ 同构的操作序列，
# 结论必须相反：前端没了，核心也随之离线（Control.Quit）。
# F9 不是注入障碍——drive 会话把 `<F9>` 作为一个转义序列一次写进 pty（A5′ 已证）；
# Ctrl+C / SIGTERM / TTY 断开与 q/F9 收敛到同一个 player_remote_shutdown()，
# 通道等效性在实现里只分流一次，这里不再重复起进程验证。
info "S8 缺省态（stop_core_on_exit=1）：前端正常退出后核心离线"

exit_key_offlines_core() {   # $1 = 退出键（q 或 <F9>）
    local key="$1"
    local ctl="$WORK_DIR/s8.ctl" rc="$WORK_DIR/s8.rc" pid="$WORK_DIR/s8.pid"
    rm -f "$XDG_CONFIG_HOME/ter-music/config.xml"   # 无文件 = 缺省 1
    : > "$ctl"
    python3 "$DRIVER" drive "$TM_BIN" "$WORK_DIR/music" "$WORK_DIR/s8.log" \
        --ctl "$ctl" --rc-file "$rc" --pid-file "$pid" --max-runtime 60 >/dev/null 2>&1 &
    local dpid=$!
    for _ in $(seq 1 30); do   # 等前端把核心确保起来并推完内容
        [ "$(snap_field 'doc["track"]["queue_count"]')" = "3" ] && break
        sleep 0.5
    done
    printf 'key:j\n' >> "$ctl"
    sleep 1
    printf 'key:<Enter>\n' >> "$ctl"
    sleep 2
    printf 'key:%s\n' "$key" >> "$ctl"
    for _ in $(seq 1 40); do
        kill -0 "$dpid" 2>/dev/null || break
        sleep 0.5
    done
    kill -9 "$dpid" 2>/dev/null
    wait 2>/dev/null || true
    local code
    code="$(cat "$rc" 2>/dev/null)"
    if [ "$code" != "0" ]; then
        bad "S8：退出键 '$key' 的前端没有正常退出（rc='$code'），核心离线断言无从判起"
        return
    fi
    for _ in $(seq 1 40); do
        daemon_up || break
        sleep 0.25
    done
    if daemon_up; then
        bad "S8：缺省态 '$key' 退出后核心仍在总线（Control.Quit 没发出或没生效）"
    else
        ok "S8：缺省态 '$key' 退出后核心随即离线（daemon status 查不到）"
    fi
}
exit_key_offlines_core q
exit_key_offlines_core '<F9>'

# ── S9：没能接入核心的前端，不动总线上的既有核心 ────────────────────
# 用户可见契约：一个没能接入核心的前端进程，无论怎么退出，都不改变总线上既有核心的存活
# 与曲目状态，也不留下新的实例。构造办法是把前端指向一个必然不在线的总线名（核心总线主名
# 是 org.mpris.MediaPlayer2.ter_music，次要实例是 `<主名>.instance<pid>`；这里用的名字带
# 非数字后缀，既不会被认成次要实例，也不可能有人持有）。
#
# 诚实交代覆盖范围：**本段不是 player_remote_shutdown() 接入门控的判别性测试**——拆掉门控
# 本段照样是绿的。实测这条路径停在 cli_ensure_core_for_frontend()：主名已被本段的夹具占用
# → daemon_start_background() 拒绝再起核心（退出码 5）→ main() 在 player_init() **之前**就
# 返回，player_shutdown() 根本没执行。另一条"接上后掉线"的路径里 remote_close_connection()
# 早把 connected/token/connection 一并清零，remote_quit_core() 开头的
# `if (!g_remote.connection) return;` 与新的门控同时成立；而 init 失败那条路径同样不进
# run_event_loop()。所以"未接入的前端把总线上的核心杀掉"这个竞态在当前代码里构造不出来，
# 要构造只能造假夹具或赌 sleep 竞态——本段不那么写。门控的价值是把契约写成显式代码、
# 防御后续改动；本段守的是这条契约**用户可见的那一面**。
# 夹具把 stop_core_on_exit 显式写成 1（config v8 起的缺省态）：这样"S8 转红 / S9 不转红"
# 的差别只可能来自"前端没接入成功"这一件事——哪天前端真的接上了主核心（--bus 被忽略、或
# ensure 改成按请求的名字起核心），它退出必发 Control.Quit，本段立刻转红。
info "S9 未接入核心的前端：不动总线上的既有核心、不留新实例"

S9_BUS="org.mpris.MediaPlayer2.ter_music.instance-s9-none"

"$TM_BIN" daemon stop --all >/dev/null 2>&1 || true      # 从干净总线起，避免前面段落的残留
for _ in $(seq 1 20); do daemon_up || break; sleep 0.1; done

cat > "$XDG_CONFIG_HOME/ter-music/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <stop_core_on_exit>1</stop_core_on_exit>
  </preferences>
</ter-music-config>
EOF

"$TM_BIN" daemon start >/dev/null 2>&1
for _ in $(seq 1 25); do daemon_up && break; sleep 0.2; done

if ! daemon_up; then
    bad "S9：夹具未能把核心起在总线上（本段其余判定无从判起）"
else
    # 让核心处于**可辨识且不会自己变**的状态：整份内容交给核心（queue_count=3）并把播放
    # 模式设成 single_repeat——6 秒的测试音播完也回到同一首，"曲目没变"才不是和播完赛跑。
    "$TM_BIN" play "$WORK_DIR/music" --mode single_repeat >/dev/null 2>&1 || true
    for _ in $(seq 1 20); do
        [ "$(snap_field 'doc["playback"]["state"]')" = "playing" ] && break
        sleep 0.25
    done
    sleep 1

    s9_pid="$(snap_field 'doc["instance"]["pid"]')"
    s9_state="$(snap_field 'doc["playback"]["state"]')"
    s9_track="$(snap_field 'doc["track"]["path"]')"
    s9_queue="$(snap_field 'doc["track"]["queue_count"]')"

    # 前端：pty 里起（main() 要求 stdout 是终端），--bus 指向那个不在线的名字，不发按键
    : > "$WORK_DIR/s9.ctl"
    TM_TUI_STDERR="$WORK_DIR/s9.stderr" timeout 60 python3 "$DRIVER" drive "$TM_BIN" \
        "$WORK_DIR/music" "$WORK_DIR/s9.log" --ctl "$WORK_DIR/s9.ctl" \
        --rc-file "$WORK_DIR/s9.rc" --pid-file "$WORK_DIR/s9.pid" --max-runtime 25 \
        -- --bus "$S9_BUS" >/dev/null 2>&1
    s9_rc="$(cat "$WORK_DIR/s9.rc" 2>/dev/null)"

    case "$s9_rc" in
        3|4|5) ok "S9：没接入核心的前端以语义化退出码结束（实测 rc=$s9_rc：3=无实例 / 4=总线不可用 / 5=实例拒绝）" ;;
        *) bad "S9：没接入核心的前端本该以非 0 的语义化退出码结束，实测 rc='${s9_rc:-空}'（0 意味着它其实接上了主核心）" ;;
    esac

    if grep -qE "实例在运行|无法连接|自定义 --bus" "$WORK_DIR/s9.stderr" 2>/dev/null; then
        ok "S9：前端把没接入的原因说清楚了（$(head -1 "$WORK_DIR/s9.stderr")）"
    else
        bad "S9：前端既没接入核心也没给出原因：stderr=$(head -1 "$WORK_DIR/s9.stderr" 2>/dev/null || echo 空)"
    fi

    s9_pid_after="$(snap_field 'doc["instance"]["pid"]')"
    if daemon_up && [ "$s9_pid_after" = "$s9_pid" ]; then
        ok "S9：主核心仍在线且是同一个实例（pid $s9_pid_after，跑前端前 $s9_pid）"
    else
        bad "S9：那个没接入核心的前端把主核心带走或换掉了（跑前 pid=$s9_pid，跑后 daemon_up=$(daemon_up && echo 1 || echo 0)，pid=${s9_pid_after:-?}）"
    fi

    s9_state_after="$(snap_field 'doc["playback"]["state"]')"
    s9_track_after="$(snap_field 'doc["track"]["path"]')"
    s9_queue_after="$(snap_field 'doc["track"]["queue_count"]')"
    if [ "$s9_state_after" = "$s9_state" ] && [ "$s9_track_after" = "$s9_track" ] &&
       [ "$s9_queue_after" = "$s9_queue" ]; then
        ok "S9：主核心的曲目状态原样未动（state=$s9_state_after，track=${s9_track_after##*/}，queue_count=$s9_queue_after）"
    else
        bad "S9：主核心状态被那个没接入的前端改了（state $s9_state→$s9_state_after，track ${s9_track##*/}→${s9_track_after##*/}，queue_count $s9_queue→$s9_queue_after）"
    fi

    s9_status_err="$("$TM_BIN" daemon status 2>&1 >/dev/null || true)"
    s9_status_pids="$("$TM_BIN" daemon status 2>/dev/null | grep -c "pid")"
    if [ "$s9_status_pids" = "1" ] && ! printf '%s' "$s9_status_err" | grep -q "次要实例"; then
        ok "S9：总线上没有多出实例（daemon status 仍 1 行 pid，也没有次要实例提示）"
    else
        bad "S9：那个没接入的前端留下了多余实例（pid 行数 $s9_status_pids，stderr：$(printf '%s' "$s9_status_err" | head -1)）"
    fi

    "$TM_BIN" daemon status --bus "$S9_BUS" >/dev/null 2>&1
    s9_phantom_rc=$?
    if [ "$s9_phantom_rc" = "3" ]; then
        ok "S9：前端要的总线名 '$S9_BUS' 自始至终没有核心（daemon status --bus 退出码 3=无实例）"
    else
        bad "S9：'$S9_BUS' 上出现了本该不存在的东西（daemon status --bus 退出码 $s9_phantom_rc）"
    fi

    # 收尾：本段夹具起的核心不能留给 trap 去猜（--all 连可能出现的次要实例一起收）
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    for _ in $(seq 1 20); do daemon_up || break; sleep 0.1; done
fi

# ── S10：收尾顺序不变量——总线名释放前，播放必须已经彻底停止 ─────────────
# 叠播（反复启停 TUI 后多个播放线程并行）的根因不在音频后端，而在 daemon_shutdown()
# 的收尾顺序：一旦先 media_session_shutdown() 释放主总线名、后收尾播放，就留下一个
# “名字已消失、进程仍在出声”的窗口；用户在该窗口里 q 退出再立刻启动 TUI，
# cli_ensure_core_for_frontend() 判定“没有核心”，daemon_start_background() 便拉起
# 第二个核心 → 两路播放流并行，而 `daemon stop` 只停得掉前端新接入的那一路。
# 本段守这条不变量：stop_audio() 早于播放线程退出，线程退出早于总线名释放。
#
# 判定只看核心**调试日志的行序**，不看是否真的出声，所以无声卡/无 PipeWire 的 CI
# 同样能跑（后端是 ALSA 还是 Pulse 都无所谓）。唯一前提是核心能进 playing 态，
# 进不去就 skip 并打印原因，绝不让断言空洞地通过。
# 三行都取 `Shutting down daemon` 之后首次出现的行号：该行是 daemon_shutdown() 打印
# 的第一条，锚在它之后即可排除“曲目自然播完”同样会打的 Playback thread exiting
# （素材也因此用 60 秒长音，收尾时绝未播完）。
# `Releasing D-Bus name` 由 media_session_shutdown() 在 dbus_bus_release_name() 前打印
# （log_debug；实测 --debug 下确实落盘）。万一某环境看不到它，退回同一函数更早打印的
# `Shutting down media session`：释放动作在其后，判定只会更宽松而不会更松到失真。
info "S10 收尾顺序不变量：总线名释放之前，播放必须已经彻底停止"

core_pids() {   # 只数核心进程（`daemon foreground`）：[ ] 让模式串本身匹配不到脚本与
                # python 包装进程的命令行（与 pipewire-playback-census-e2e.sh 同口径）
    pgrep -f 'daemon[ ]foreground' 2>/dev/null | tr '\n' ' '
}

core_count() {
    local n=0 pid
    for pid in $(core_pids); do
        n=$((n + 1))
    done
    printf '%s' "$n"
}

s10_log_dir="$XDG_CONFIG_HOME/ter-music"
s10_log_before="$(ls -1t "$s10_log_dir"/ter-music-debug-*.log 2>/dev/null | head -1)"

"$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
for _ in $(seq 1 20); do daemon_up || break; sleep 0.1; done

cat > "$XDG_CONFIG_HOME/ter-music/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <stop_core_on_exit>1</stop_core_on_exit>
    <core_exit_when_no_frontend>0</core_exit_when_no_frontend>
    <volume_percent>80</volume_percent>
  </preferences>
</ter-music-config>
EOF

# 本段自己造一条 60 秒长音（开头夹具那批素材只有 6 秒：播完会自然打线程退出行）
s10_music="$WORK_DIR/s10-music"
mkdir -p "$s10_music"
s10_material=""
if command -v ffmpeg >/dev/null 2>&1; then
    if ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=520:duration=60" \
            -c:a pcm_s16le -y "$s10_music/long-60s.wav" >/dev/null 2>&1; then
        s10_material="$s10_music/long-60s.wav"
    fi
fi

# 起一个 --debug 核心（daemon_prepare_logging 把调试日志落到配置目录）
"$TM_BIN" daemon start --debug >/dev/null 2>&1 || true
for _ in $(seq 1 25); do daemon_up && break; sleep 0.2; done
s10_log="$(ls -1t "$s10_log_dir"/ter-music-debug-*.log 2>/dev/null | head -1)"

if ! daemon_up; then
    bad "S10：夹具没能用 --debug 起一个核心（收尾顺序无从判定）"
elif [ -z "$s10_material" ]; then
    skip "S10：没有 ffmpeg（或素材生成失败），造不出 60 秒长音来保证收尾时仍在播"
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
elif [ -z "$s10_log" ] || [ "$s10_log" = "$s10_log_before" ]; then
    bad "S10：本次 --debug 核心没有写出自己的调试日志（$s10_log_dir/ter-music-debug-*.log）"
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
else
    "$TM_BIN" play "$s10_music" >/dev/null 2>&1 || true
    s10_state=""
    for _ in $(seq 1 40); do
        s10_state="$(snap_field 'doc["playback"]["state"]')"
        [ "$s10_state" = "playing" ] && break
        sleep 0.25
    done

    if [ "$s10_state" != "playing" ]; then
        skip "S10：该环境里核心始终进不了 playing 态（实测 state=${s10_state:-空}），播放收尾窗口无从观测"
        "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    else
        # 触发退出：`daemon stop` 发的就是 Control.Quit（与前端 q 退出同一条收尾路径）。
        # 放后台，好让采样循环盯着整个收尾过程；它自己要等名字释放（上限 5s）。
        "$TM_BIN" daemon stop >/dev/null 2>&1 &
        s10_stop_job=$!
        s10_max_cores=0
        s10_max_pids=""
        s10_zero_streak=0
        for _ in $(seq 1 80); do          # 每 50ms 采一次，上限 4s
            s10_cores_now="$(core_count)"
            if [ "${s10_cores_now:-0}" -gt "$s10_max_cores" ]; then
                s10_max_cores="$s10_cores_now"
                s10_max_pids="$(core_pids)"
            fi
            if [ "${s10_cores_now:-0}" -eq 0 ]; then
                s10_zero_streak=$((s10_zero_streak + 1))
                [ "$s10_zero_streak" -ge 3 ] && break
            else
                s10_zero_streak=0
            fi
            sleep 0.05
        done
        wait "$s10_stop_job" 2>/dev/null
        s10_stop_rc=$?

        s10_anchor="$(awk '/\[daemon\] Shutting down daemon/{print NR; exit}' "$s10_log")"
        s10_line_stop="$(awk -v a="${s10_anchor:-0}" 'NR>a && /stop_audio\(\) called/{print NR; exit}' "$s10_log")"
        s10_line_exit="$(awk -v a="${s10_anchor:-0}" 'NR>a && /Playback thread exiting/{print NR; exit}' "$s10_log")"
        s10_release_mark="Releasing D-Bus name"
        s10_line_release="$(awk -v a="${s10_anchor:-0}" 'NR>a && /Releasing D-Bus name/{print NR; exit}' "$s10_log")"
        if [ -z "$s10_line_release" ]; then
            s10_release_mark="Shutting down media session"
            s10_line_release="$(awk -v a="${s10_anchor:-0}" 'NR>a && /Shutting down media session/{print NR; exit}' "$s10_log")"
        fi

        if [ -z "$s10_anchor" ]; then
            bad "S10：核心日志里没有 'Shutting down daemon'（$s10_log），收尾记录缺失，顺序无从判定"
        elif [ -z "$s10_line_stop" ] || [ -z "$s10_line_exit" ] || [ -z "$s10_line_release" ]; then
            bad "S10：收尾日志缺行，无法判定顺序（stop_audio=${s10_line_stop:-无} 线程退出=${s10_line_exit:-无} ${s10_release_mark}=${s10_line_release:-无}）"
        elif [ "$s10_line_stop" -lt "$s10_line_exit" ] && [ "$s10_line_exit" -lt "$s10_line_release" ]; then
            ok "S10：总线名在播放彻底停止之后才释放（stop_audio@行${s10_line_stop} < 线程退出@行${s10_line_exit} < ${s10_release_mark}@行${s10_line_release}）"
        else
            bad "总线名在播放停止之前就被释放（叠播窗口重现）：stop_audio@行${s10_line_stop:-无} / 线程退出@行${s10_line_exit:-无} / ${s10_release_mark}@行${s10_line_release:-无}"
            for s10_ln in "$s10_line_stop" "$s10_line_exit" "$s10_line_release"; do
                [ -n "$s10_ln" ] || continue
                printf '       %s\n' "$(awk -v n="$s10_ln" 'NR==n' "$s10_log")"
            done
        fi

        if [ "${s10_max_cores:-0}" -gt 1 ]; then
            bad "S10：退出过程中同时存在 ${s10_max_cores} 个 daemon foreground 核心（叠播的直接形态），pid=${s10_max_pids}"
        else
            ok "S10：退出过程中核心进程数从未超过 1（峰值 ${s10_max_cores}，每 50ms 采样、上限 4s）"
        fi

        # 新顺序的代价实测条：把“先停播再放名字”多花的时间摊在 CLI 的 5s 离线等待里
        if [ "${s10_stop_rc:-0}" -ne 0 ]; then
            bad "S10：daemon stop 没能在 5s 上限内等到实例离线（退出码 $s10_stop_rc）"
        else
            ok "S10：daemon stop 在 CLI 的 5s 离线上限内等到名字释放（收尾顺序调整没顶穿它）"
        fi
    fi
fi

info "结果"
printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
