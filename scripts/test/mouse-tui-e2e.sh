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
#   S8b 子视图单击 = 选中并执行；设置选择弹层在选项冒号后展开、贴边翻转且不越界。
#   S8c 均衡器页鼠标：点击频带列只选中（不改增益、不执行）；滚轮改当前选中
#       频带/预增益的值（上滚 +1 dB、下滚 -1 dB，钳制在 ±12 dB），点另一个
#       频带后滚轮的作用目标随之切换；开关行上的滚轮只被消费，不切换开关。
#   S9 子视图（设置）侧栏与内容区各自认领滚轮，归属不串台。
#   S10a 按钮常显（config v12）：没有功能开关，默认 Off 下控制区第 8 个按钮
#       也在——命中表里格7 有行列、屏幕有按钮文案、点击能开消人声弹层。
#   S10 控制区第 8 个按钮（卡拉OK/消人声）：点击开弹层、弹层滚轮移选、
#       Enter 落定写进核心，Esc 取消则不改档。
#   S11 设置页「播放设置」子页的消人声三档行（键盘路径：F2 → 侧栏 →
#       内容区 → 弹层选定）：v12 删除了功能行，本组末行就是档位行。
#   S11b 设置页不再有「启用卡拉OK（选择模型）」功能行（v12 删除）。
#
# 排障开关：TM_DUMP=1 时，弹层检测失败会把当时的屏幕网格片段打到结果里。
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

usage() { sed -n '2,47p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

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
    [ -n "${CORE_FG_PID:-}" ] && kill "$CORE_FG_PID" 2>/dev/null || true
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
CORE_FG_PID=""
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

# 先在私有总线上起一个前台核心，TUI 起来后直接接入该实例。
# 常规宿主的 TUI 会自己 fork 核心；受限宿主/容器里 CLI 可能拒绝 detach fork，
# 预起核心让"鼠标/布局/控制面"这些真正的断言不受进程归属影响。
daemon_up() { "$TM_BIN" daemon status 2>/dev/null | grep -q "pid"; }
"$TM_BIN" daemon start >/dev/null 2>&1
for _ in $(seq 1 40); do daemon_up && break; sleep 0.25; done
if ! daemon_up; then
    "$TM_BIN" daemon foreground >"$WORK_DIR/core-foreground.log" 2>&1 &
    CORE_FG_PID=$!
    for _ in $(seq 1 80); do daemon_up && break; sleep 0.25; done
fi
daemon_up && ok "预起播放核心上线（TUI 将 attach 到该实例）" \
    || { bad "播放核心未上线"; exit 1; }

python3 "$DRIVER" drive "$TM_BIN" "$MUSIC" "$WORK_DIR/tui.log" \
    --ctl "$CTL_FILE" --rc-file "$WORK_DIR/tui.rc" --pid-file "$WORK_DIR/tui.pid" \
    --max-runtime 240 -- --debug >/dev/null 2>&1 &
DRIVER_PID=$!

mlog() {   # 前端自己的调试日志：同目录里还有核心的日志，必须挑"有 [mouse] 行"的那份
    local f files
    mapfile -t files < <(ls -t "$LOG_DIR"/*.log 2>/dev/null)
    for f in "${files[@]}"; do
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
# 消人声档位只走 Control.GetKaraoke（不是 info 字段），故用 CLI 取编号：
# 输出形如 "soft (1) 人声较弱"，只取括号里的数字，与界面语言无关。
kar_level() { "$TM_BIN" karaoke 2>/dev/null | sed -n 's/^[a-z]* (\([0-9]\)).*/\1/p'; }
kar_wait() {   # $1 = 期望档位；CLI 走一次 D-Bus 往返，给最多 5 秒收敛
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ "$(kar_level)" = "$1" ] && return 0
        sleep 0.5
    done
    return 1; }
screen_has() {  # 在当前屏幕网格上做正则搜索（宽字符被 vt_grid 按列展开，故模式里允许空格）
    python3 "$SCRIPT_DIR/vt_grid.py" --cols "${TERM_COLS:-100}" --rows "${TERM_ROWS:-30}" \
        --has "$1" "$WORK_DIR/tui.log" >/dev/null 2>&1; }
karaoke_popup_on_screen() {   # 认弹层标题那一行：╭─ 消人声 ─╮（英文 Vocal Removal，先秦汰聲之等）
    local _i                  # pty 收屏比界面慢一拍，单次抓取会假失败，故重试约 3 秒
    for _i in 1 2 3 4 5 6; do
        screen_has "─ ?(消 ?人 ?声|Vocal ?Removal|汰 ?聲 ?之 ?等)" && return 0
        sleep 0.5
    done
    return 1; }
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
    # 仅看到 state=playing 还不够：切曲时新曲目路径会先出现、上一首的
    # playing 状态还会短暂残留，此时滚轮 seek 会被随后真正开始的播放重置。
    # 以“播放位置在两次读数之间确实前进”作为新曲目时钟已启动的判据。
    local i a b
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if [ "$(play_state)" = "playing" ]; then
            a="$(play_pos)"; sleep 0.4; b="$(play_pos)"
            case "$a" in ''|*[!0-9]*) a=-1 ;; esac
            case "$b" in ''|*[!0-9]*) b=-1 ;; esac
            [ "$a" -ge 0 ] && [ "$b" -gt "$a" ] && return 0
        fi
        sleep 0.3
    done
    printf '         前置失败现场：state=%s name=%s 位置=%s 路由=%s\n' \
        "$(play_state)" "$(play_name)" "$(play_pos)" "$(last_route)"
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

sel_menu_line() {   # 最近一条 [settings] sel menu 日志行
    local L; L="$(mlog)"; [ -n "$L" ] || { echo ""; return; }
    grep -h "\[settings\] sel menu" "$L" | tail -1; }

route_count() {   # 日志里匹配 $1 的路由行条数
    local L; L="$(mlog)"; [ -n "$L" ] || { echo 0; return; }
    grep -hc "$1" "$L" 2>/dev/null || echo 0; }

# ── 均衡器鼠标用例的观测面：核心落盘的 config.xml ──────────────────
# 配置树形如 <equalizer><enabled>..</enabled><preamp>..</preamp>
#            <band frequency="125">0</band>…</equalizer>。
# 增益由 player_eq_* 经 player_config_persist 交给核心异步落盘，故读取失败
# 或尚未收敛时都要短重试；返回 "ERR" 表示始终读不到/解析不了。
eq_cfg() {   # $1 = enabled | preamp | <频带频率，如 125>
    local key="$1" i out
    for i in 1 2 3 4 5 6 7 8; do
        out="$(python3 - "$XDG_CONFIG_HOME/ter-music/config.xml" "$key" 2>/dev/null <<'PY'
import sys
import xml.etree.ElementTree as ET

try:
    eq = ET.parse(sys.argv[1]).getroot().find("equalizer")
except Exception:
    raise SystemExit(1)
if eq is None:
    raise SystemExit(1)
if sys.argv[2] in ("enabled", "preamp"):
    node = eq.find(sys.argv[2])
else:
    node = next((b for b in eq.findall("band")
                 if b.get("frequency") == sys.argv[2]), None)
try:
    value = (node.text or "").strip()
except AttributeError:
    raise SystemExit(1)
if not value:
    raise SystemExit(1)
print(value)
PY
)"
        case "$out" in ''|ERR) sleep 0.25 ;; *) printf '%s' "$out"; return 0 ;; esac
    done
    printf 'ERR'; return 1; }

eq_expect() {   # $1=字段 $2=期望值 [$3=超时秒]；成功打印实测值，失败打印最后实测
    local key="$1" want="$2" secs="${3:-8}" i out=""
    for i in $(seq 1 "$(( secs * 4 ))"); do
        out="$(eq_cfg "$key")"
        [ "$out" = "$want" ] && { printf '%s' "$out"; return 0; }
        sleep 0.25
    done
    printf '%s' "$out"; return 1; }

eq_snapshot() {   # 均衡器落盘面全量：preamp/enabled + 10 段增益
    local b out=""
    for b in 31 62 125 250 500 1000 2000 4000 8000 16000; do
        out="$out $b=$(eq_cfg "$b")"
    done
    printf 'preamp=%s enabled=%s%s' "$(eq_cfg preamp)" "$(eq_cfg enabled)" "$out"; }

eq_log_seen() {   # $1 = 模式；在 [settings] eq mouse 日志里等它出现（块缓冲有延迟）
    local pat="$1" secs="${2:-5}" i L
    for i in $(seq 1 "$(( secs * 4 ))"); do
        L="$(mlog)"
        [ -n "$L" ] && grep -h "\[settings\] eq mouse" "$L" | grep -q -- "$pat" && return 0
        sleep 0.25
    done
    return 1; }

eq_log_last() {   # 最近一条均衡器鼠标日志（排障用）
    local L; L="$(mlog)"; [ -n "$L" ] || { echo ""; return; }
    grep -h "\[settings\] eq mouse" "$L" | tail -1; }

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
# 必须先等到 state=playing（v12 核心/前端分离后，队列里的 track 路径会先出现、
# 播放状态稍后才变 playing）；seek_relative_seconds() 在 STOPPED 或时长未知时
# 是有意 no-op，太早发滚轮会被吞掉，看起来像“只前进了自然播放的 1 秒”。
ensure_track 03-charlie.wav 5 || bad "前置：03-charlie 未进入 playing（滚轮 seek 无法验证）"
sleep 0.3
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
send "<click:60:20>" 0.8               # “返回”分区无选项：点击只认领、不动作
t="$(last_route)"
[ "$t" = "target=14" ] && ok "设置视图内容区点击归内容区" \
    || bad "设置视图内容区点击归属异常：$t"
send "<F1>" 1.0
send "<wheel-down:6:5>" 0.7
[ "$(last_route)" = "target=8" ] && ok "F1 回主界面后列表滚轮恢复归属播放列表" \
    || bad "回主界面后归属没恢复（$(last_route)）"

# ── S8b 单击语义 + 设置选择弹层就地展开 ──────────────────────────
info "S8b 设置页：单击侧栏即执行；选择弹层在选项冒号后展开且不越界"
# 侧栏第 9 项（general.back）在 SGR 行 11；单击一次就应退出设置视图。
send "<F2>" 1.2
send "<click:10:11>" 1.0
t="$(last_route)"
[ "$t" = "target=13" ] && ok "单击侧栏“返回”归侧栏（target=13）" \
    || bad "单击侧栏“返回”归属异常：$t"
send "<wheel-down:6:5>" 0.6
if [ "$(last_route)" = "target=8" ]; then
    ok "单击侧栏“返回”一次即回到主界面"
else
    bad "单击侧栏“返回”没有立即执行（列表滚轮归属 $(last_route)）"
fi

# 回设置页，单击第一行颜色选项（screen row 4，SGR 行 5）即应打开调色板。
send "<F2>" 1.2
send "<click:30:5>" 1.0
sel=""
for _ in 1 2 3 4 5 6; do
    sel="$(sel_menu_line)"
    case "$sel" in *"sel menu open"*) break ;; esac
    sleep 0.5
done
sel_parsed="$(printf '%s' "$sel" | sed -nE \
    's/.*anchor=([0-9]+),([0-9]+) rect=([0-9]+),([0-9]+),([0-9]+),([0-9]+).*/\1 \2 \3 \4 \5 \6/p')"
if [ -n "$sel_parsed" ]; then
    read -r ay ax mt ml mh mw <<<"$sel_parsed"
    if [ "$mt" = "$ay" ] && [ "$ml" = "$ax" ]; then
        ok "选择弹层在选项所在位置展开（anchor=$ay,$ax rect=$mt,$ml,$mh,$mw）"
    else
        bad "选择弹层没有锚定选项（anchor=$ay,$ax rect=$mt,$ml）"
    fi
    # 冒号后锚定：100 列窗口下内容区左缘为 100/4+2=27，弹层左缘必须严格
    # 落在其右（即"选项名+冒号"之后的取值列），不得从行首盖住选项名。
    if [ "$ax" -gt $((100 / 4 + 2)) ]; then
        ok "弹层左缘锚定在选项冒号之后的取值列（ax=$ax > 27），不盖选项名"
    else
        bad "弹层左缘仍贴着选项行首（ax=$ax），会盖住选项名"
    fi
    if [ $((mt + mh)) -le 27 ] && [ $((ml + mw)) -le 98 ]; then
        ok "选择弹层完整落在终端内（不压状态/提示行与右边框）"
    else
        bad "选择弹层越界：rect=$mt,$ml,$mh,$mw"
    fi
    # 弹层内也单击即执行：首项是“跟随主题”，点一次应 apply=1 并关闭。
    send "<click:$((ml + 3)):$((mt + 2))>" 1.0
    closed=""
    for _ in 1 2 3 4 5 6; do
        closed="$(sel_menu_line)"
        case "$closed" in *"close apply=1"*) break ;; esac
        sleep 0.5
    done
    case "$closed" in
        *"close apply=1"*) ok "单击弹层条目即落定（apply=1）" ;;
        *) bad "单击弹层条目没有落定：${closed:-无日志}" ;;
    esac
else
    bad "解析不到选择弹层打开日志：${sel:-无日志}"
fi

# 靠底部的颜色选项（screen row 21，SGR 行 22）：弹层必须上翻且仍在边界内。
send "<click:30:22>" 1.0
sel=""
for _ in 1 2 3 4 5 6; do
    sel="$(sel_menu_line)"
    case "$sel" in *"sel menu open"*) break ;; esac
    sleep 0.5
done
sel_parsed="$(printf '%s' "$sel" | sed -nE \
    's/.*anchor=([0-9]+),([0-9]+) rect=([0-9]+),([0-9]+),([0-9]+),([0-9]+).*/\1 \2 \3 \4 \5 \6/p')"
if [ -n "$sel_parsed" ]; then
    read -r ay ax mt ml mh mw <<<"$sel_parsed"
    if [ "$mt" -lt "$ay" ] && [ $((mt + mh)) -le 27 ] &&
       [ $((ml + mw)) -le 98 ]; then
        ok "靠底部的选项触发上翻且不越界（anchor=$ay rect=$mt,$mh）"
    else
        bad "靠底部的选项没有正确上翻/钳制（anchor=$ay rect=$mt,$ml,$mh,$mw）"
    fi
else
    bad "解析不到底部选项的弹层日志：${sel:-无日志}"
fi
send "<Esc>" 0.8
send "<F1>" 1.0

# ── S8c 均衡器鼠标：点击只选中频带，滚轮增减选中项的值 ────────────
#       契约（与 eq.hint / 帮助文档一致）：
#         · 左键点频带列（柱体/0dB 线/频率标签/增益值这一整列）只选中该频带，
#           不改增益、也不执行任何动作；
#         · 滚轮不再轮询选中，而是改**当前选中项**的值：上滚 +1 dB、下滚
#           -1 dB；频带与预增益都钳制在 ±12 dB；开关行只消费、不切换；
#         · 点击另一个频带后，滚轮的作用目标随之切换（指针停在哪一列无关）。
#       100x30 classic 布局：设置内容区 content_x=27（0 基），每频带 5 列，
#       频带 b 的中心列（SGR 1 基）= 30+5b，命中行 7..18；125 Hz 是 b=2。
#       观测面：前端 [settings] eq mouse 日志 + 核心落盘的 config.xml。
info "S8c 均衡器：点击只选中频带，滚轮增减选中频带 ±1 dB"
send "<F2>" 1.2
send "<click:10:9>" 1.2                     # 侧栏「均衡器」在 SGR 行 9
t="$(last_route)"
[ "$t" = "target=13" ] && ok "点击设置侧栏“均衡器”归侧栏（target=13）" \
    || bad "点击设置侧栏“均衡器”归属异常：期望 target=13，实测 $t"
eq_page=0
for _ in 1 2 3 4 5 6; do
    screen_has '±1dB' && { eq_page=1; break; }
    sleep 0.5
done
[ "$eq_page" = "1" ] && ok "均衡器页已打开（提示行含新版鼠标说明 ±1dB）" \
    || bad "没进入均衡器页（屏幕上找不到新版 ±1dB 提示）"

snap0="$(eq_snapshot)"
send "<click:40:12>" 1.0                    # 125 Hz（b=2）柱体所在列
t="$(last_route)"
[ "$t" = "target=14" ] && ok "点击频带列归设置内容区（target=14）" \
    || bad "点击频带列归属异常：期望 target=14，实测 $t"
sleep 0.8
snap1="$(eq_snapshot)"
[ "$snap1" = "$snap0" ] && ok "点击 125 Hz 只选中、不改增益（前后一致：$snap1）" \
    || bad "点击频带改了增益：$snap0 → $snap1"
eq_log_seen "eq mouse select band=2" \
    && ok "点击命中 125 Hz（band=2），没有落到执行分支" \
    || bad "点击频带没有 select 日志（最后一条：$(eq_log_last)）"

send "<wheel-up:40:12>" 1.0
t="$(last_route)"
[ "$t" = "target=14" ] && ok "频带滚轮归设置内容区（target=14）" \
    || bad "频带滚轮归属异常：期望 target=14，实测 $t"
if got="$(eq_expect 125 1)"; then
    ok "125 Hz 上滚一格 +1 dB（config.xml：0 → +1）"
else
    bad "125 Hz 上滚一格不是 +1 dB（config.xml 实测 $got）"
fi
eq_log_seen "option=31 gain=1$" \
    && ok "滚轮日志确认选中项仍是 band=2（option=31）、值 +1" \
    || bad "滚轮日志不符（最后一条：$(eq_log_last)）"
send "<wheel-down:40:12>" 1.0
if got="$(eq_expect 125 0)"; then
    ok "125 Hz 下滚一格 -1 dB（config.xml：+1 → 0）"
else
    bad "125 Hz 下滚一格没回到 0（config.xml 实测 $got）"
fi

# 点击另一个频带：既验证“点击只选中”，也验证滚轮作用目标会跟着切。
send "<click:50:12>" 1.0                    # 500 Hz（b=4）
t="$(last_route)"
[ "$t" = "target=14" ] && ok "点击 500 Hz 频带列归内容区（target=14）" \
    || bad "点击 500 Hz 频带列归属异常（$t）"
sleep 0.8
snap2="$(eq_snapshot)"
[ "$snap2" = "$snap0" ] && ok "点击 500 Hz 同样只选中、不改增益（$snap2）" \
    || bad "点击 500 Hz 改了增益：$snap0 → $snap2"
eq_log_seen "eq mouse select band=4" && ok "点击命中 500 Hz（band=4）" \
    || bad "点击 500 Hz 没有 select 日志（最后一条：$(eq_log_last)）"
# 指针故意停在 125 Hz 那一列：滚轮必须只作用于“选中的 500 Hz”。
send "<wheel-up:40:12>" 1.0
if got="$(eq_expect 500 1)"; then
    ok "滚轮只改当前选中频带：500 Hz → +1 dB（指针停在 125 Hz 列亦然）"
else
    bad "滚轮没作用于选中的 500 Hz（config.xml 实测 $got）"
fi
[ "$(eq_cfg 125)" = "0" ] && ok "同一次滚轮没有波及 125 Hz（仍 0）" \
    || bad "滚轮串台：125 Hz 变成了 $(eq_cfg 125)"
eq_log_seen "option=33 gain=1$" \
    && ok "滚轮日志确认作用目标已切到 500 Hz（option=33）" \
    || bad "滚轮日志不符（最后一条：$(eq_log_last)）"
send "<wheel-down:50:12>" 1.0               # 收回 0，下一段做钳制
if ! eq_expect 500 0 >/dev/null; then
    bad "500 Hz 下滚没回到 0（config.xml 实测 $(eq_cfg 500)）"
fi

# ±12 dB 钳制：选中 125 Hz 连滚 13 格到 +12，再多滚不退格；500 Hz 连滚到 -12。
send "<click:40:12>" 1.0
for _ in $(seq 1 13); do send "<wheel-up:40:12>" 0.2; done
if got="$(eq_expect 125 12 12)"; then
    ok "125 Hz 连续上滚 13 格钳制在 +12 dB"
else
    bad "125 Hz 上滚没有钳制在 +12 dB（config.xml 实测 $got）"
fi
for _ in 1 2 3; do send "<wheel-up:40:12>" 0.2; done
[ "$(eq_cfg 125)" = "12" ] && ok "越过 +12 dB 后再滚仍停在 +12 dB" \
    || bad "+12 dB 后越界（实测 $(eq_cfg 125)）"
eq_log_seen "option=31 gain=12$" \
    && ok "钳制日志确认 band=2 的值停在 max（12）" \
    || bad "钳制日志不符（最后一条：$(eq_log_last)）"
send "<click:50:12>" 0.9
for _ in $(seq 1 12); do send "<wheel-down:50:12>" 0.2; done
if got="$(eq_expect 500 -12 12)"; then
    ok "500 Hz 连续下滚 12 格钳制在 -12 dB"
else
    bad "500 Hz 下滚没有钳制在 -12 dB（config.xml 实测 $got）"
fi
for _ in 1 2 3; do send "<wheel-down:50:12>" 0.2; done
[ "$(eq_cfg 500)" = "-12" ] && ok "越过 -12 dB 后再滚仍停在 -12 dB" \
    || bad "-12 dB 后越界（实测 $(eq_cfg 500)）"

# 预增益：点 31 Hz 后按 Left 退到预增益行，滚轮同样 ±1 dB，且不动开关。
send "<click:30:12>" 0.9                    # 31 Hz（b=0）
send "<Left>" 0.8
send "<wheel-up:50:15>" 1.0
if got="$(eq_expect preamp 1)"; then
    ok "选中预增益后上滚 +1 dB（config.xml preamp：0 → +1，enabled 仍 0）"
else
    bad "预增益上滚不是 +1 dB（config.xml 实测 $got）"
fi
eq_log_seen "option=28 gain=1$" \
    && ok "滚轮日志确认作用目标是预增益（option=28）" \
    || bad "预增益滚轮日志不符（最后一条：$(eq_log_last)）"
[ "$(eq_cfg enabled)" = "0" ] && ok "改预增益没有顺带切换开关（enabled 仍 0）" \
    || bad "改预增益把开关切成了 $(eq_cfg enabled)"

# 再按 Left 到开关行：滚轮只被消费，不翻转。
send "<Left>" 0.8
send "<wheel-up:50:15>" 1.0
sleep 0.6
[ "$(eq_cfg enabled)" = "0" ] && ok "开关行上滚只消费不切换（enabled 仍 0）" \
    || bad "开关行上的滚轮把开关切成了 $(eq_cfg enabled)"
eq_log_seen "option=27 ignored" && ok "开关行滚轮日志为 ignored（无值变更）" \
    || bad "开关行滚轮日志不符（最后一条：$(eq_log_last)）"

# 全量盘点：只有 125=+12、500=-12、preamp=+1 三处被鼠标改动过。
want="preamp=1 enabled=0 31=0 62=0 125=12 250=0 500=-12 1000=0 2000=0 4000=0 8000=0 16000=0"
snapf="$(eq_snapshot)"
[ "$snapf" = "$want" ] && ok "全量盘点只剩预期改动（$snapf）" \
    || bad "均衡器状态与预期不符：期望 $want，实测 $snapf"
send "<F1>" 1.0

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

# ── S10a 按钮常显（config v12）：默认 Off 也显示第 8 个卡拉OK按钮 ──
#       模型随包内置、没有"功能开关/模型选择"，控制栏按钮不再被门禁隐藏。
#       控制按钮可见性仍收在唯一几何来源 controls_compute_layout()，因此三处
#       一起验：命中表格7 有行列（鼠标命中/滚轮几何）、屏幕有按钮文案、
#       点击按钮能打开消人声弹层。
info "S10a 按钮常显：默认配置下控制区就有卡拉OK按钮"
send "<Esc>" 0.5                        # 收掉前面用例可能残留的弹层
"$TM_BIN" karaoke off >/dev/null 2>&1
kar_wait 0 >/dev/null 2>&1
KARA_CELL="$(cell 7)"
if [ -n "$KARA_CELL" ]; then
    ok "默认配置下命中表有格7（按钮参与布局/渲染/命中，格7=$KARA_CELL）"
else
    bad "默认配置下命中表没有格7（按钮被隐藏，违反 v12 常显契约）"
fi
HT="$(grep -h "控制区命中表" "$(mlog)" 2>/dev/null | tail -1)"
case "$HT" in
    *"格7=-1"*) bad "命中表原始行仍把格7 标成未分配（$HT）" ;;
    *)          ok "命中表原始行确认格7 已分配行列（默认 Off 也常显）" ;;
esac
if screen_has '卡拉OK:(不启用|人声较弱|消除人声)|Karaoke:(Off|Faint vocals|No vocals)'; then
    ok "主界面显示卡拉OK按钮文案（默认 Off 也常显）"
else
    bad "主界面没有卡拉OK按钮文案（按钮应常显）"
fi
send "<click:10:23>" 0.8                # 控制区第一行空白（不压任何按钮）
if karaoke_popup_on_screen; then
    bad "点击控制区空白处竟打开了消人声弹层"
else
    ok "点击控制区空白处不命中卡拉OK入口（无弹层）"
fi

# ── S10 卡拉OK按钮：点击开弹层 → 滚轮移选 → Enter 生效 ──────────
#       第 8 个按钮（CONTROL_IDX_KARAOKE=7）走的正是模式/倍速/音量
#       那一套弹层机制。这里既验它没被 100 列布局挤掉（命中表里得有格7），
#       也验"鼠标开层 + 弹层滚轮选定 + Enter 落定"真的把档位写进了核心。
info "S10 控制区卡拉OK按钮：弹层选档写进核心"
if [ -z "$KARA_CELL" ]; then
    bad "命中表里没有格7（卡拉OK按钮在当前宽度下被丢弃，按钮排布需重新测量）"
else
    "$TM_BIN" karaoke off >/dev/null 2>&1
    if kar_wait 0; then
        ok "S10 前置：档位为 off（0）"
    else
        bad "S10 前置档位不是 0（实测 $(kar_level)）"
    fi
    KR_ROW="${KARA_CELL%% *}"; KR_COL="${KARA_CELL##* }"
    send "<click:$KR_COL:$KR_ROW>" 1.0
    # 点击归属看日志（块缓冲可能滞后，故最多重试 3 秒）；弹层是否真开看屏幕：
    # 锚点取标题行的制表符 "─ 消人声"，这样按钮自身的文案"卡拉OK（消人声）"
    # 不会被误判成弹层已开（vt_grid 重建网格，不受日志块缓冲影响）。
    route=""
    for _ in 1 2 3 4 5 6; do
        route="$(last_route)"
        [ "$route" = "target=9" ] && break
        sleep 0.5
    done
    if [ "$route" = "target=9" ]; then
        ok "卡拉OK按钮点击归控制区（target=9）"
    else
        bad "卡拉OK按钮点击归属异常（$route）"
    fi
    if karaoke_popup_on_screen; then
        ok "点击卡拉OK按钮打开了消人声弹层（屏上见弹层标题）"
    else
        bad "点击卡拉OK按钮没打开消人声弹层"
        [ -n "${TM_DUMP:-}" ] && python3 "$SCRIPT_DIR/vt_grid.py" --cols 100 --rows 30 \
            "$WORK_DIR/tui.log" | sed -n 20,30p
    fi
    send "<Esc>" 0.8
    if kar_wait 0; then
        ok "Esc 取消弹层不改档位（仍 0）"
    else
        bad "Esc 取消把档位改成了 $(kar_level)"
    fi
    # 重新开层：selected_index 复位为当前档位 0，两格下滚到第三档，Enter 落定
    send "<click:$KR_COL:$KR_ROW>" 1.0
    send "<wheel-down:50:20>" 0.5
    send "<wheel-down:50:20>" 0.5
    send "<Enter>" 1.0
    if kar_wait 2; then
        ok "弹层选定第三档后核心档位为 full（2）"
    else
        bad "弹层选定没写进核心（档位 $(kar_level)，期望 2）"
    fi
    # 末档上再开一层：滚轮被钳制在最后一项，Esc 取消仍不该改档
    send "<click:$KR_COL:$KR_ROW>" 1.0
    send "<wheel-down:50:20>" 0.5
    send "<Esc>" 0.8
    if kar_wait 2; then
        ok "末档钳制 + Esc 取消后档位仍为 2"
    else
        bad "末档取消流程把档位改成了 $(kar_level)"
    fi
    "$TM_BIN" karaoke off >/dev/null 2>&1
fi

# ── S11 设置页「播放设置」里的消人声三档行 ──────────────────────
#       设置页是纯键盘路径（弹层只有 Enter，没有 ←/→ 微调），所以这里
#       走完"进视图 → 侧栏切子页 → 移到末行 → Enter 开层 → 选定"整条链。
#       config v12 删除了 v11 的"启用卡拉OK（选择模型）"功能行：本组末行
#       就是消人声档位行（不再需要"再上一行"）。行号对不对，用弹层标题的
#       制表符锚点来验：开错行就没有 "─ 消人声" 这行。
info "S11 设置页：播放设置子页的消人声三档行（v12 唯一一行）"
"$TM_BIN" karaoke off >/dev/null 2>&1
kar_wait 0 >/dev/null 2>&1
send "<F2>" 1.4                      # 进设置视图（侧栏复位到第 0 项）
send "<Down>" 0.6; send "<Down>" 0.6 # 侧栏 0→2 = 播放设置
send "<Right>" 0.8                   # 焦点进内容区，选中项回到本组第 0 行
send "<Up>" 0.8                      # 回绕到本组末行 = 消人声档位行
if screen_has '启用卡拉OK|Enable Karaoke|启 ?用 ?卡 ?拉 ?OK'; then
    bad "设置页仍有「启用卡拉OK（选择模型）」功能行（v12 已删除）"
else
    ok "设置页没有「启用卡拉OK（选择模型）」功能行（v12 删除）"
fi
send "<Enter>" 1.0                   # 弹出三档选择层
if karaoke_popup_on_screen; then
    ok "设置页末行的弹层标题是消人声（末行=三档行，行序正确）"
else
    bad "设置页末行开出来的弹层不是消人声（子页项序变了，需重新数行）"
    send "<Esc>" 0.5
fi
send "<Down>" 0.6; send "<Down>" 0.6 # 0→2 = 消除人声
send "<Enter>" 1.2                   # 落定：走 player_set_karaoke_mode 门面
if kar_wait 2; then
    ok "设置页选定「消除人声」后核心档位为 2"
else
    bad "设置页的选定没写进核心（档位 $(kar_level)，期望 2）"
fi
send "<Esc>" 0.8                     # 退出设置视图，回主界面

# ── S11b 设置页不再有功能行（config v12 删除）─────────────────────
#       v11 的「启用卡拉OK（选择模型）」功能行（更换模型/关闭功能）已删除，
#       配置里也不再有 karaoke_feature/karaoke_model 两个键。
info "S11b 设置页与配置面都不再有功能开关/模型选择"
"$TM_BIN" config get karaoke_feature >/dev/null 2>&1 \
    && bad "config get karaoke_feature 仍可用（键应已删除）" \
    || ok "config get karaoke_feature 被拒绝（v12 已删除该键）"
"$TM_BIN" config get karaoke_model >/dev/null 2>&1 \
    && bad "config get karaoke_model 仍可用（键应已删除）" \
    || ok "config get karaoke_model 被拒绝（v12 已删除该键）"
send "<F2>" 1.4                      # 重新进设置视图，扫一遍播放设置页
send "<Down>" 0.6; send "<Down>" 0.6
send "<Right>" 0.8
if screen_has '更换模型文件|Change the model file|关闭卡拉OK|Disable karaoke'; then
    bad "设置页/弹层仍出现「更换模型文件/关闭卡拉OK」（模型选择 UI 应已删除）"
else
    ok "设置页不再出现「更换模型文件/关闭卡拉OK」模型选择 UI"
fi
send "<Esc>" 0.8
"$TM_BIN" karaoke off >/dev/null 2>&1
kar_wait 0 >/dev/null 2>&1

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
