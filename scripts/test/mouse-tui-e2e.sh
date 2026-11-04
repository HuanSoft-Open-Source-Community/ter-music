#!/usr/bin/env bash
#
# 全局鼠标事件（点击 / 滚轮）真机端到端回归
#
# 覆盖（本次「TUI 鼠标点击与滚轮」变更的核心行为，含菜单展开的竞争态）：
#   S1 播放列表点击 = 播放指针所在那一行（文件树模式：根节点占第 1 行）。
#   S2 列表滚轮只移动选中，绝不改变正在播放的曲目（滚轮≠回车）。
#   S3 控制区进度行滚轮 = ±5 秒（UI_SEEK_STEP_SECONDS）。
#   S4 控制区音量按钮滚轮 = ±5%（UI_VOLUME_STEP_PERCENT）。
#   S9 控制区倍速按钮滚轮 = 换一档，且到边界回绕（放在最后，见该段注释）。
#   S6 歌词窗格滚轮 = 快进到下一句 / 回退到上一句（跳到相邻时间戳）。
#   S7 弹层展开的竞争态：弹层内滚轮只滚弹层；弹层外的点击与滚轮被吞掉
#      （既不动列表也不动控制区），并且外点收起后一切恢复正常。
#   S8 底部功能键栏点击 = 切页（与按 F1..F8 同一条 execute_global_action 链路）。
#   S9 子视图（设置）侧栏与内容区各自认领滚轮，归属不串台。
#
# 判定依据有两路，互相印证：
#   · D-Bus 事实：`ter-music show --json` 的 playback.{position_ms,volume_percent,
#     speed,play_mode} 与 track.path —— 行为有没有真的发生。
#   · 前端调试日志里 [mouse] 的路由行（handle_global_mouse_event 打的）：
#     事件归哪个窗格（target=8 列表 / 9 控制区 / 10 歌词 / 2 弹层 / 6 功能栏 /
#     13 侧栏 / 14 内容区）—— 归属对不对，是否被吞掉。
#   所以会话必须带 --debug 启动，且用 TER_MUSIC_LOG_DIR 指定日志目录。
#
# 坐标以 100x30 窗口 + classic 布局实测为准（lifecycle-driver.py 的缺省尺寸）：
#   播放列表窗格 行 1..13/列 0..67，树首行=根，第 n 个文件在窗口行 n+1，
#     即 SGR 行 n+3（点击 SGR 行 3/5/7 → 第 1/3/5 首）；
#   控制区窗格 行 22..28：进度条在 SGR 行 25，按钮两排在 SGR 行 26/27，
#     其中倍速 27 行列 21..34、音量 27 行列 35..47、模式 26 行列 47..63
#     （按钮格随标签宽度浮动，故先让前端把命中表打进日志，脚本按日志取格）；
#   歌词窗格 列 68..99 / 行 5..28；底部功能键栏在最底行（SGR 行 30）。
#   布局若改动，这些断言会失败并打印实测路由行，提示重新测量——不会静默放过。
#
# 用法：scripts/test/mouse-tui-e2e.sh [--bin <ter-music>] [--keep]
# 环境：会话总线（缺省自行 dbus-run-session）、ffmpeg 造素材、python3。
#       宿主被判定为如意玲珑容器时 CLI 拒绝拉起后台核心，故缺省置 TER_MUSIC_SANDBOX=0
#       （已设置则沿用调用者的值）。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""
DRIVER_PID=""
CTL_FILE=""
LOG_DIR=""

usage() { sed -n '2,42p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

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

# ── 独占总线：只允许本脚本拉起的核心在总线上 ──────────────────────
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ "${TM_TEST_REUSE_SESSION_BUS:-0}" != "1" ]; then
    command -v dbus-run-session >/dev/null 2>&1 || {
        echo "错误：需要 dbus-run-session，或设 TM_TEST_REUSE_SESSION_BUS=1 复用当前会话总线。" >&2
        exit 2; }
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

cleanup() {
    [ -n "$DRIVER_PID" ] && kill -9 "$DRIVER_PID" 2>/dev/null || true
    wait "$DRIVER_PID" 2>/dev/null || true   # 收掉后台作业的状态回显，别在 CI 日志里留噪声
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（用 --bin 指定）" >&2; exit 2; }
command -v ffmpeg  >/dev/null 2>&1 || { echo "错误：需要 ffmpeg 造素材" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "错误：需要 python3" >&2; exit 2; }

DRIVER="$SCRIPT_DIR/lifecycle-driver.py"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-mouse-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_CACHE_HOME="$HOME/.cache"
# 宿主若被判定为如意玲珑容器，CLI 会拒绝 fork 后台核心；这里默认关掉该判定，
# 真的容器里测试可以用 TER_MUSIC_SANDBOX=1 交回原行为。
export TER_MUSIC_SANDBOX="${TER_MUSIC_SANDBOX:-0}"
LOG_DIR="$WORK_DIR/logs"
export TER_MUSIC_LOG_DIR="$LOG_DIR"  # 前端 --debug 的路由日志要落在可寻址的目录
MUSIC="$WORK_DIR/music"
mkdir -p "$XDG_CONFIG_HOME" "$LOG_DIR" "$MUSIC"

# 30 秒的音：断言窗口里不会自己切歌；03 配一份歌词供 S6 用
tone() { ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "sine=frequency=$1:duration=30" -c:a pcm_s16le -y "$2" 2>/dev/null; }
tone 300 "$MUSIC/01-alpha.wav"
tone 350 "$MUSIC/02-bravo.wav"
tone 400 "$MUSIC/03-charlie.wav"
tone 450 "$MUSIC/04-delta.wav"
tone 500 "$MUSIC/05-echo.wav"
printf '[00:02.00]第一句\n[00:05.00]第二句\n[00:08.00]第三句\n[00:12.00]第四句\n[00:20.00]第五句\n' \
    > "$MUSIC/03-charlie.lrc"

# ── 会话与观测 ────────────────────────────────────────────────────
CTL_FILE="$WORK_DIR/keys.cmd"; : > "$CTL_FILE"
send() { printf 'key:%s\n' "$1" >> "$CTL_FILE"; sleep "${2:-0.9}"; }

python3 "$DRIVER" drive "$TM_BIN" "$MUSIC" "$WORK_DIR/tui.log" \
    --ctl "$CTL_FILE" --rc-file "$WORK_DIR/tui.rc" --pid-file "$WORK_DIR/tui.pid" \
    --max-runtime 240 -- --debug >/dev/null 2>&1 &
DRIVER_PID=$!

mlog() {   # 前端自己的调试日志：同目录里还有核心的日志，必须挑"有 [mouse] 行"的那份
    local f
    for f in $(ls -t "$LOG_DIR"/*.log 2>/dev/null); do
        grep -q "\[mouse\]" "$f" 2>/dev/null && { printf '%s' "$f"; return; }
    done
    printf ''; }

show_get() {   # $1 = python 表达式，作用在 show --json 的 dict 上
    local i out=""
    for i in 1 2 3 4 5 6; do
        out="$("$TM_BIN" show --json 2>/dev/null | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print("ERR"); raise SystemExit
try:
    print(eval(sys.argv[1], {"d": d}))
except Exception:
    print("ERR")' "$1" 2>/dev/null)"
        [ -n "$out" ] && [ "$out" != "ERR" ] && break
        sleep 0.4
    done
    printf '%s' "${out:-ERR}"; }

play_pos()  { show_get 'd["playback"]["position_ms"]'; }
play_vol()  { show_get 'd["playback"]["volume_percent"]'; }
play_spd()  { show_get 'd["playback"]["speed"]'; }
play_mode() { show_get 'd["playback"]["play_mode"]'; }
play_name() { show_get '(d["track"].get("path") or "?").split("/")[-1]'; }
play_state() { show_get 'd["playback"]["state"]'; }
ensure_track() {   # $1 = 期望文件名, $2 = 该文件的 SGR 行
    send "<click:6:3>" 1.0 || true          # 先点首行，确保索引发生变化
    wait_name 01-alpha.wav 8 || true
    send "<click:6:$2>" 1.0
    if ! wait_name "$1" 8; then
        printf '         前置失败现场：state=%s name=%s 路由=%s\n' \
            "$(play_state)" "$(play_name)" "$(last_route)"
        grep -h "\[mouse\]" "$(mlog)" 2>/dev/null | tail -3 | sed 's/^/         /'
        return 1
    fi
    local i
    for i in 1 2 3 4 5 6; do
        [ "$(play_state)" = "playing" ] && return 0
        sleep 0.4
    done
    printf '         前置失败现场：state=%s name=%s 路由=%s\n' \
        "$(play_state)" "$(play_name)" "$(last_route)"
    grep -h "\[mouse\]" "$(mlog)" 2>/dev/null | tail -4 | sed 's/^/         /'
    return 1; }

wait_name() {   # $1 = 期望文件名, $2 = 超时秒
    local i
    for i in $(seq 1 "$(( ${2:-12} * 2 ))"); do
        [ "$(play_name)" = "$1" ] && return 0
        sleep 0.5
    done
    return 1; }

last_route() {   # 最近一条 [mouse] 路由行（$1 = 可选 grep 过滤）
    local L; L="$(mlog)"; [ -n "$L" ] || { echo ""; return; }
    grep -h "\[mouse\].*-> target=" "$L" | tail -1 | sed -E 's/.*-> (target=[-0-9]+).*/\1/'; }

route_count() {   # 日志里匹配 $1 的路由行条数
    local L; L="$(mlog)"; [ -n "$L" ] || { echo 0; return; }
    grep -hc "$1" "$L" 2>/dev/null || echo 0; }

info "起会话（-o music，--debug 以便读路由日志）"
send "<click:6:3>" 1.6                        # 点列表第 1 行，把播放推起来
if ! wait_name 01-alpha.wav 15; then
    bad "起点：点击播放列表第 1 行没有播 01-alpha.wav（实测 $(play_name)）"
    info "结果"; printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"; exit 1
fi
ok "会话就绪，点击列表首行播到 01-alpha.wav"

# ── S1 播放列表点击 ───────────────────────────────────────────────
info "S1 播放列表：点击哪一行就播哪一首"
for pair in "5 03-charlie.wav" "7 05-echo.wav" "3 01-alpha.wav"; do
    set -- $pair
    send "<click:6:$1>" 1.2
    if wait_name "$2" 10; then
        ok "点击 SGR 行 $1 → 播 $2"
    else
        bad "点击 SGR 行 $1 没播 $2（实测 $(play_name)）"
    fi
    t="$(last_route)"
    [ "$t" = "target=8" ] && ok "该点击归属播放列表（$t）" \
        || bad "该点击归属异常：期望 target=8，实测 $t"
done

# ── S2 列表滚轮只动选中 ───────────────────────────────────────────
info "S2 播放列表：滚轮移动选中，不改变正在播放的曲目"
before="$(play_name)"
send "<wheel-down:6:5>" 0.6; send "<wheel-down:6:5>" 0.6; send "<wheel-up:6:5>" 0.6
if [ "$(play_name)" = "$before" ]; then
    ok "滚轮 3 格后仍在播 $before（滚轮不等于回车）"
else
    bad "滚轮把播放改成了 $(play_name)（应为 $before）"
fi
[ "$(last_route)" = "target=8" ] && ok "滚轮归属播放列表" \
    || bad "滚轮归属异常：$(last_route)"

# ── 从日志取控制区命中表（按钮格随标签文字宽度浮动，故每次现取现用）──
send "<wheel-down:30:28>" 0.6          # 状态行滚轮：只为把命中表引进日志，不动播放
cell() {   # $1 = 控件序号（4=模式 5=倍速 6=音量），输出 "SGR行 SGR列"（窗格中心）
    local L t r c w
    L="$(mlog)"; t=""
    for i in 1 2 3 4 5; do
        t="$(grep -h "控制区命中表" "$L" 2>/dev/null | tail -1)"; [ -n "$t" ] && break
        sleep 0.4
    done
    r="$(printf '%s' "$t" | sed -E "s/.*格$1=([-0-9]+):([-0-9]+):([0-9]+).*/\1/")"
    c="$(printf '%s' "$t" | sed -E "s/.*格$1=([-0-9]+):([-0-9]+):([0-9]+).*/\2/")"
    w="$(printf '%s' "$t" | sed -E "s/.*格$1=([-0-9]+):([-0-9]+):([0-9]+).*/\3/")"
    case "$r:$c:$w" in *:*:*) ;; *) return 1 ;; esac
    [ "${r:0:1}" = "-" ] && return 1
    # 控制区窗格左上角在屏幕 (22,0)，SGR 坐标为 1 基
    printf '%s %s' "$((r + 22 + 1))" "$((c + w / 2 + 1))"; }
VOL_CELL="$(cell 6)"; SPD_CELL="$(cell 5)"; MODE_CELL="$(cell 4)"
if [ -z "$VOL_CELL" ] || [ -z "$MODE_CELL" ]; then
    bad "没能从日志里读到控制区命中表，后面的滚轮用例无从定位按钮"
    info "结果"; printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"; exit 1
fi
ok "控制区命中表：音量行/列 $VOL_CELL，倍速 $SPD_CELL，模式 $MODE_CELL（SGR 坐标）"

# 命中表随标签文字变宽变窄，每个用例前都重新取一次，不用旧值。
# 探测点选 SGR 行 28（控制区的状态行）：归属仍是控制区，会把命中表打进日志，
# 但 controls_row_index 返回 -1 —— 不会顺带 seek +5s 把曲子滚到结尾。
resync_cell() { send "<wheel-down:30:28>" 0.5; cell "$1"; }

# ── S3 进度滚轮 ±5 秒 ─────────────────────────────────────────────
info "S3 控制区进度行：滚轮 ±5 秒"
send "<click:6:5>" 1.2                       # 回到 03-charlie（进度从 0 起）
wait_name 03-charlie.wav 10 || true
p0="$(play_pos)"
send "<wheel-down:30:25>" 0.7; p1="$(play_pos)"
send "<wheel-up:30:25>"   0.7; p2="$(play_pos)"
UP=$((p1 - p0)); DN=$((p2 - p1))
if [ $((UP >= 3500 && UP <= 6500 ? 1 : 0)) -eq 1 ]; then
    ok "下滚一格 +5s（实测 ${UP}ms）"
else
    bad "下滚一格不是 +5s（${p0}→${p1} = ${UP}ms）"
fi
if [ $((DN <= -3500 && DN >= -6500 ? 1 : 0)) -eq 1 ]; then
    ok "上滚一格 -5s（实测 ${DN}ms）"
else
    bad "上滚一格不是 -5s（${p1}→${p2} = ${DN}ms）"
fi
if [ "$(last_route)" = "target=9" ]; then
    ok "进度行滚轮归属控制区（target=9）"
else
    bad "进度行滚轮归属异常：$(last_route)"
fi

# ── S4 音量滚轮 ±5% ───────────────────────────────────────────────
info "S4 控制区音量按钮：滚轮 ±5%"
"$TM_BIN" volume 80 >/dev/null 2>&1 || true
sleep 0.5
VOL_CELL="$(resync_cell 6)"
v0="$(play_vol)"
send "<wheel-down:${VOL_CELL##* }:${VOL_CELL%% *}>" 0.7; v1="$(play_vol)"
VOL_CELL="$(resync_cell 6)"
send "<wheel-up:${VOL_CELL##* }:${VOL_CELL%% *}>"   0.7; v2="$(play_vol)"
if [ "$v1" = "$((v0 + 5))" ]; then
    ok "音量 ${v0}% → ${v1}%（+5%）"
else
    bad "音量滚轮不是 +5%（${v0}→${v1}）"
fi
if [ "$v2" = "$v0" ]; then
    ok "反向滚轮回原（${v1}% → ${v2}%）"
else
    bad "反向滚轮没回到 ${v0}%（实测 ${v2}%）"
fi

# ── S6 歌词滚轮 = 快进/回退到下一句/上一句 ────────────────────────
info "S6 歌词窗格：滚轮跳到相邻一句（03-charlie 的 lrc 有 2/5/8/12/20 秒）"
ensure_track 03-charlie.wav 5 || bad "S6 前置：03-charlie 没能播起来"
"$TM_BIN" seek 3 >/dev/null 2>&1 || true     # 游标落在「第一句」(2s) 之后
sleep 0.6
l0="$(play_pos)"
send "<wheel-down:85:12>" 0.8; l1="$(play_pos)"
send "<wheel-up:85:12>"   0.8; l2="$(play_pos)"
if [ "$l1" = "5000" ] || [ "$l1" = "8000" ]; then
    ok "下滚一句跳到 ${l1}ms（下一句时间戳）"
else
    bad "下滚一句没跳到下一句（${l0}→${l1}）"
fi
if [ "$l2" = "2000" ] || [ "$l2" = "5000" ]; then
    ok "上滚一句回到 ${l2}ms（上一句时间戳）"
else
    bad "上滚一句没回到上一句（${l1}→${l2}）"
fi
[ "$(last_route)" = "target=10" ] && ok "歌词滚轮归属歌词窗格" \
    || bad "歌词滚轮归属异常：$(last_route)"

# ── S7 弹层展开的竞争态 ───────────────────────────────────────────
info "S7 弹层展开：滚轮只滚弹层，外部点击吞掉不穿透，收起后一切正常"
ensure_track 03-charlie.wav 5 || bad "S7 前置：03-charlie 没能播起来"
MODE_CELL="$(resync_cell 4)"
MR_ROW="${MODE_CELL%% *}"; MR_COL="${MODE_CELL##* }"
mode_before="$(play_mode)"
send "<click:$MR_COL:$MR_ROW>" 1.0
name_before="$(play_name)"
send "<wheel-down:6:5>" 0.8            # 列表区滚轮：展开期间必须被弹层接管
if [ "$(last_route)" = "target=2" ]; then
    ok "点击模式按钮打开了弹层，随后事件归弹层（target=2）"
else
    bad "点击模式按钮没打开弹层（下一个事件 $(last_route)，期望 target=2）"
fi
send "<wheel-down:30:25>" 0.7          # 进度行滚轮：同样不得作用到控制区
if [ "$(last_route)" = "target=2" ]; then
    ok "弹层展开时，控制区的滚轮仍归弹层所有（row=局部行）"
else
    bad "弹层展开时滚轮被内容区抢走（$(last_route)）"
fi
if [ "$(play_name)" = "$name_before" ]; then
    ok "弹层展开时的滚轮没改变播放（仍 $(play_name)）"
else
    bad "弹层展开时滚轮动了播放（$(play_name)）"
fi
# 外点故意落在"另一首"的行上：若穿透就会切歌，吞掉则仍在播原来那首
send "<click:6:7>" 1.0
if [ "$(play_name)" = "$name_before" ]; then
    ok "弹层外点击被吞掉，没有穿透到列表（仍 $(play_name)）"
else
    bad "弹层外点击穿透到列表，播成了 $(play_name)（应仍是 $name_before）"
fi
send "<wheel-down:6:5>" 0.7
if [ "$(last_route)" = "target=8" ]; then
    ok "弹层收起后，列表滚轮重新归播放列表（target=8）"
else
    bad "弹层收起后归属没恢复（$(last_route)）"
fi
if [ "$(play_mode)" = "$mode_before" ]; then
    ok "这一整段没误改播放模式（仍是 $(play_mode)）"
else
    bad "鼠标弹层流程误改播放模式：$mode_before → $(play_mode)"
fi
send "<click:6:7>" 1.2                 # 外点后列表点击仍可用
if wait_name 05-echo.wav 12; then
    ok "外点收起后点击列表照常播放（05-echo.wav）"
else
    bad "外点收起后列表点击失效（实测 $(play_name)）"
fi

# ── S8 功能键栏点击切页 ───────────────────────────────────────────
info "S8 底部功能键栏：点击 = 切页（与 F1..F8 同一链路）"
send "<click:13:30>" 1.2               # f2:设置
if [ "$(last_route)" = "target=6" ]; then
    ok "最底行点击归功能键栏（target=6）"
else
    bad "最底行点击没归功能键栏（$(last_route)）"
fi
send "<wheel-down:10:8>" 0.8           # 左侧栏滚轮：子视图侧栏必须认领
t="$(last_route)"
if [ "$t" = "target=13" ]; then
    ok "设置视图侧栏滚轮归侧栏（target=13）"
else
    bad "设置视图侧栏滚轮归属异常：期望 target=13（SIDEBAR），实测 $t"
fi
send "<wheel-down:60:8>" 0.8           # 右侧内容区滚轮
t="$(last_route)"
if [ "$t" = "target=14" ]; then
    ok "设置视图内容区滚轮归内容区（target=14）"
else
    bad "设置视图内容区滚轮归属异常：期望 target=14（CONTENT），实测 $t"
fi
send "<click:60:20>" 0.8               # 内容区点击：认领但不越权改配置
t="$(last_route)"
[ "$t" = "target=14" ] && ok "设置视图内容区点击归内容区" \
    || bad "设置视图内容区点击归属异常：$t"
send "<F1>" 1.0
send "<wheel-down:6:5>" 0.7
[ "$(last_route)" = "target=8" ] && ok "F1 回主界面后列表滚轮恢复归属播放列表" \
    || bad "回主界面后归属没恢复（$(last_route)）"

# ── S9 倍速滚轮换档 + 回绕（**放在最后**：改倍速会让核心停播，
#       这是既有缺陷，纯 CLI `ter-music speed` 同样复现，与鼠标无关；
#       放在末尾可避免它把 S6/S7 的播放前置一起拖掉。）
info "S9 控制区倍速按钮：滚轮换档，边界回绕"
s0="$(play_spd)"
SPD_CELL="$(resync_cell 5)"
send "<wheel-down:${SPD_CELL##* }:${SPD_CELL%% *}>" 0.7; s1="$(play_spd)"
if [ "$s0" != "$s1" ]; then
    ok "倍速 ${s0}x → ${s1}x（滚轮换档）"
else
    bad "倍速滚轮没换档（仍是 ${s0}x）"
fi
wrap=0; prev="$s1"
for _ in 1 2 3 4 5 6 7 8; do
    SPD_CELL="$(resync_cell 5)"
    send "<wheel-down:${SPD_CELL##* }:${SPD_CELL%% *}>" 0.5
    cur="$(play_spd)"
    if [ "$cur" = "$s0" ]; then wrap=1; break; fi
    prev="$cur"
done
if [ "$wrap" = "1" ]; then
    ok "倍速连续下滚回到起点 ${s0}x（回绕生效）"
else
    bad "倍速连续下滚 8 次没回到 ${s0}x（末值 $prev）"
fi

# ── 收尾 ──────────────────────────────────────────────────────────
if kill -0 "$(cat "$WORK_DIR/tui.pid" 2>/dev/null)" 2>/dev/null; then
    ok "跑完全部鼠标用例后前端仍存活（没有崩溃）"
else
    bad "前端在鼠标用例中退出（rc=$(cat "$WORK_DIR/tui.rc" 2>/dev/null)）"
fi
send "q" 1.5
info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
