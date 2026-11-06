#!/usr/bin/env bash
#
# 右键上下文菜单端到端回归
#
# 覆盖：
#   R1  主视图列表右键：菜单在指针处打开、不播放、矩形不越界；
#   R2  队列管理子菜单（第 2 层）执行"追加到队列"：核心队列 +1；
#   R3  "切换列表模式"条目：文件模式 → 队列模式（[ui] playlist tab mode）；
#   R4  播放控制 → 播放模式（第 3 层）：三层菜单都在屏内；
#   R5  贴右下角右键：上翻/左翻，且不压底部提示条行（top+rows <= 29）；
#   R6  菜单展开时右键 / 点菜单外：关闭并吞掉，不执行任何动作；
#   R7  控制区弹层展开时右键：只取消弹层，不弹菜单；
#   R8  设置页右键 + 执行"保存设置"；
#   R9  历史/歌单/收藏/语言/帮助页右键：菜单打开且标题正确；
#   R10 信息页：菜单严格 1 条"科乐美流光"，执行后流光开启/关闭；
#   R10b 主视图信息窗格：右键被吞掉，不弹菜单（流光菜单只属于 F6 信息页）；
#   R11 曲库浏览器右键：菜单目标是 BROWSER；
#   R12 布局编辑器：右键在指针处打开"窗格操作"菜单并钳制，执行"退出编辑器"；
#   R13 搜索输入框（模态）中的右键被吞掉，不弹菜单；
#   R14 竞态：菜单打开后外部 queue append 改队列版本，再点"从队列移除" →
#       提示 stale 且队列数不变；
#   R15 菜单展开 2 s（动画继续）不自行关闭；
#   R16 播放中动画窗格刷新时，菜单不被擦除（合成刷新 + 菜单快照）；
#   R16b 修饰左键备选触发（pty 语义）：Ctrl/Alt+左键打开/收起菜单、只开一次、
#        跨格按下/松开也只开一次、不播放；
#   R16c 中键备选触发：中键打开/收起菜单、不播放；带修饰的中键被忽略；
#   R16d 带修饰的右键一律不触发 TUI 菜单，裸右键不受影响。
#        注意：本脚本直接把 SGR 字节写进 pty，绕过终端模拟器；它只证明
#        ncurses 层的归一化语义，不证明 deepin-terminal 会转发修饰位
#        （该终端实测只转发按钮号，Ctrl/Alt+左键等同普通左键，请用中键）。
#   R17 全程前端存活。
#
# 判定依据：
#   · [context_menu] debug 日志（open/level/idx/push/action/callback/close/stale）；
#   · [ui] playlist tab mode / [controls] popup shown / [layout_editor] 日志；
#   · D-Bus 事实：show --json 的 track、Queue.Get 的 count。
#
# 用法：scripts/test/context-menu-e2e.sh [--bin <ter-music>] [--keep]
# 环境：会话总线（缺省自行 dbus-run-session）、ffmpeg 造素材、python3。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""
DRIVER_PID=""
CTL_FILE=""
TUI_LOG=""
LOG_FILE=""

usage() { sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

ORIG_ARGS=("$@")
while [ $# -gt 0 ]; do
    case "$1" in
        --bin)  TM_BIN="$2"; shift 2 ;;
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

if [ -z "${TM_TEST_BUS_READY:-}" ] && [ "${TM_TEST_REUSE_SESSION_BUS:-0}" != "1" ]; then
    command -v dbus-run-session >/dev/null 2>&1 || {
        echo "错误：需要 dbus-run-session，或设 TM_TEST_REUSE_SESSION_BUS=1 复用当前会话总线。" >&2
        exit 2; }
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

cleanup() {
    if [ -n "$DRIVER_PID" ]; then
        if [ -n "$CTL_FILE" ] && [ -f "$CTL_FILE" ]; then
            printf 'kill\n' >> "$CTL_FILE" 2>/dev/null || true
            sleep 0.3
        fi
        kill -9 "$DRIVER_PID" 2>/dev/null || true
        wait "$DRIVER_PID" 2>/dev/null || true
    fi
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
RPC="$SCRIPT_DIR/rpc_client.py"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-ctx-menu-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_CACHE_HOME="$HOME/.cache"
export TER_MUSIC_SANDBOX="${TER_MUSIC_SANDBOX:-0}"
unset TER_MUSIC_CLI_LANG
LOG_DIR="$WORK_DIR/logs"
export TER_MUSIC_LOG_DIR="$LOG_DIR"
MUSIC="$WORK_DIR/music"
CONFIG_DIR="$XDG_CONFIG_HOME/ter-music"
mkdir -p "$CONFIG_DIR" "$LOG_DIR" "$MUSIC"

tone() { ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "sine=frequency=$1:duration=20" -c:a pcm_s16le -y "$2" 2>/dev/null; }
tone 300 "$MUSIC/01-alpha.wav"
tone 350 "$MUSIC/02-bravo.wav"
tone 400 "$MUSIC/03-charlie.wav"
tone 450 "$MUSIC/04-delta.wav"

cat > "$CONFIG_DIR/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <auto_play_on_start>0</auto_play_on_start>
    <resume_last_playback>0</resume_last_playback>
    <ui_language>zh_CN</ui_language>
  </preferences>
</ter-music-config>
EOF

CTL_FILE="$WORK_DIR/keys.cmd"; : > "$CTL_FILE"
TUI_LOG="$WORK_DIR/tui.bin"

send() { printf 'key:%s\n' "$1" >> "$CTL_FILE"; sleep "${2:-0.6}"; }
right_at() { send "<right:$1:$2>" "${3:-0.9}"; }   # $1=列 $2=行（SGR 1 基）

mlog() {   # 前端调试日志（同目录里还有核心日志）
    local f
    for f in $(ls -t "$LOG_DIR"/*.log 2>/dev/null); do
        grep -q "\[layout\] Selected layout" "$f" 2>/dev/null && { printf '%s' "$f"; return; }
    done
    printf ''
}

show_get() {
    local i out=""
    for i in 1 2 3 4; do
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
        sleep 0.25
    done
    printf '%s' "${out:-ERR}"
}
play_name() { show_get '(d["track"].get("path") or "?").split("/")[-1]'; }

queue_count() {
    python3 "$RPC" call cn.org.huansoft.ter_music.Queue.Get 0 100 2>/dev/null |
        python3 -c 'import json,sys; print(json.load(sys.stdin).get("count"))' 2>/dev/null ||
        printf 'ERR'
}

# ── 菜单日志解析 ─────────────────────────────────────────────────
last_level_line() {   # $1 = level
    grep -h "\[context_menu\] level=$1 title=" "$LOG_FILE" 2>/dev/null | tail -1
}

# 输出 "top left rows cols"
level_rect() {
    local line; line="$(last_level_line "$1")"
    printf '%s' "$line" | sed -E 's/.*rect=([0-9]+),([0-9]+),([0-9]+),([0-9]+).*/\1 \2 \3 \4/'
}

item_idx() {   # $1 = level, $2 = grep 片段
    local line
    line=$(grep -h "\[context_menu\] level=$1 idx=.*$2" "$LOG_FILE" 2>/dev/null | tail -1)
    printf '%s' "$line" | sed -E 's/.*idx=([0-9]+).*/\1/'
}

log_count() { grep -c "$1" "$LOG_FILE" 2>/dev/null || true; }

# 只数"普通左键点击"（[mouse] ... click wheel=...），不含 right-click。
# 新备选触发用例用它证明一次修饰键/中键事件没有被降级成普通左键（播放）。
plain_click_count() {
    grep -c '\[mouse\].* click wheel' "$LOG_FILE" 2>/dev/null || true
}

# 按"日志行号标记 → 只读新增的菜单 open/close 行"判断本次按键的效果。
# 比全局计数稳：不会把上一击/下一击的 close 读进来，也不依赖 grep 的时序。
menu_mark() { wc -l < "$LOG_FILE" 2>/dev/null || printf '0'; }
menu_new() {
    tail -n +"$(( ${1:-0} + 1 ))" "$LOG_FILE" 2>/dev/null |
        grep '\[context_menu\] \(open\|close\)' || true
}

# 最新一层菜单里，目标下标之前有多少个"可选且启用"的条目（用于键盘导航）。
selectable_before() {
    local level="$1" target="$2" start
    start=$(grep -n "\[context_menu\] level=$level title=" "$LOG_FILE" 2>/dev/null |
            tail -1 | cut -d: -f1)
    [ -n "$start" ] || { printf '0'; return; }
    tail -n +"$start" "$LOG_FILE" |
        grep "\[context_menu\] level=$level idx=" |
        head -n "$target" |
        grep -v "separator" |
        grep -c "enabled=1"
}

rect_inside() {   # $1=top $2=left $3=rows $4=cols
    [ "$1" -ge 0 ] && [ "$2" -ge 0 ] && \
    [ $(( $1 + $3 )) -le 29 ] && [ $(( $2 + $4 )) -le 100 ]
}

click_level_item() {   # $1=level $2=idx；单击即执行/下钻
    local rect top left rows cols x y
    rect="$(level_rect "$1")"
    if [ -z "$rect" ] || [ "$rect" = "$(last_level_line "$1")" ]; then
        return 1
    fi
    top=$(printf '%s' "$rect" | cut -d' ' -f1)
    left=$(printf '%s' "$rect" | cut -d' ' -f2)
    rows=$(printf '%s' "$rect" | cut -d' ' -f3)
    cols=$(printf '%s' "$rect" | cut -d' ' -f4)
    [ -n "$top" ] && [ -n "$left" ] || return 1
    y=$((top + 1 + $2 + 1)); x=$((left + 2 + 1))
    send "<click:$x:$y>" 0.9
    return 0
}

# ── 起会话 ───────────────────────────────────────────────────────
info "起会话（100x30，--debug）"
"$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
python3 "$DRIVER" drive "$TM_BIN" "$MUSIC" "$TUI_LOG" \
    --ctl "$CTL_FILE" --rc-file "$WORK_DIR/tui.rc" --pid-file "$WORK_DIR/tui.pid" \
    --cols 100 --rows 30 --max-runtime 240 -- --debug >/dev/null 2>&1 &
DRIVER_PID=$!

for _ in $(seq 1 40); do LOG_FILE="$(mlog)"; [ -n "$LOG_FILE" ] && break; sleep 0.25; done
[ -n "$LOG_FILE" ] || { bad "前端没有写出调试日志"; exit 1; }
sleep 2.5
track_before="$(play_name)"
base_queue="$(queue_count)"
case "$base_queue" in ''|*[!0-9]*) base_queue=0 ;; esac

# ── R1 列表右键：打开、不播放、不越界 ────────────────────────────
info "R1 列表行右键：菜单打开、不播放、矩形在屏内"
right_at 6 3
line="$(last_level_line 0)"
if [ -n "$line" ]; then ok "列表右键打开了菜单"; else bad "列表右键没有打开菜单"; fi
rect="$(level_rect 0)"
top=$(printf '%s' "$rect" | cut -d' ' -f1); left=$(printf '%s' "$rect" | cut -d' ' -f2)
rows=$(printf '%s' "$rect" | cut -d' ' -f3); cols=$(printf '%s' "$rect" | cut -d' ' -f4)
if [ -n "$top" ] && rect_inside "$top" "$left" "$rows" "$cols"; then
    ok "菜单矩形完整落在屏内（$top,$left,${rows}x${cols}）"
else
    bad "菜单矩形越界或缺失：$rect"
fi
if [ "$(play_name)" = "$track_before" ]; then
    ok "右键没有触发播放（仍 $track_before）"
else
    bad "右键改变了播放：$track_before → $(play_name)"
fi

# ── R2 队列管理 → 追加到队列 ──────────────────────────────────────
info "R2 队列管理子菜单：追加到队列"
qidx="$(item_idx 0 'submenu=队列管理')"
if [ -n "$qidx" ] && click_level_item 0 "$qidx"; then
    if grep -q "\[context_menu\] push depth=2" "$LOG_FILE"; then
        ok "队列管理子菜单展开（第 2 层）"
    else
        bad "队列管理子菜单没有展开"
    fi
else
    bad "找不到队列管理条目"
fi
sub_idx="$(item_idx 1 'action=queue.append')"
append_before="$(log_count 'action=queue.append')"
if [ -n "$sub_idx" ] && click_level_item 1 "$sub_idx"; then
    for _ in $(seq 1 20); do
        [ "$(log_count 'action=queue.append')" -gt "$append_before" ] && break
        sleep 0.2
    done
    if [ "$(log_count 'action=queue.append')" -gt "$append_before" ]; then
        ok "执行了队列追加动作"
    else
        bad "没有执行队列追加动作"
    fi
else
    bad "找不到/点不中'追加到队列'"
fi
sleep 0.8
qcount="$(queue_count)"
if [ "$qcount" = "$((base_queue + 1))" ]; then
    ok "核心队列 +1（$base_queue → $qcount）"
else
    bad "队列数异常：$base_queue → $qcount（期望 +1）"
fi

# ── R3 切换列表模式 ─────────────────────────────────────────────
info "R3 列表右键：切换到队列模式"
right_at 6 3
midx="$(item_idx 0 'callback=mode.toggle')"
if [ -n "$midx" ] && click_level_item 0 "$midx"; then
    if grep -q "playlist tab mode -> 1" "$LOG_FILE"; then
        ok "列表切到队列模式（playlist tab mode -> 1）"
    else
        bad "没有切到队列模式"
    fi
else
    bad "找不到/点不中'切换列表模式'条目"
fi

# ── R4 三层菜单：播放控制 → 播放模式 ─────────────────────────────
info "R4 播放控制 → 播放模式（第 3 层）"
right_at 6 3
qidx="$(item_idx 0 'submenu=播放控制')"
n=$(selectable_before 0 "$qidx")
downs="$(printf '<down>%.0s' $(seq 1 "$n"))"
l1_before="$(log_count "level=1 title='播放控制'")"
send "${downs}<right>" 1.0
for _ in $(seq 1 20); do
    [ "$(log_count "level=1 title='播放控制'")" -gt "$l1_before" ] && break
    sleep 0.2
done
if [ "$(log_count "level=1 title='播放控制'")" -gt "$l1_before" ]; then
    ok "播放控制子菜单展开（第 2 层）"
else
    bad "播放控制子菜单没有展开"
fi
pidx="$(item_idx 1 'submenu=播放模式')"
m=$(selectable_before 1 "$pidx")
downs2="$(printf '<down>%.0s' $(seq 1 "$m"))"
level2_before="$(log_count 'level=2 title=')"
send "${downs2}<right>" 1.0
for _ in $(seq 1 20); do
    [ "$(log_count 'level=2 title=')" -gt "$level2_before" ] && break
    sleep 0.2
done
if [ "$(log_count 'level=2 title=')" -gt "$level2_before" ]; then
    ok "三层菜单展开成功"
else
    bad "第 3 层没有展开"
fi
rect="$(level_rect 2)"
t=$(printf '%s' "$rect" | cut -d' ' -f1); l=$(printf '%s' "$rect" | cut -d' ' -f2)
r=$(printf '%s' "$rect" | cut -d' ' -f3); c=$(printf '%s' "$rect" | cut -d' ' -f4)
if [ -n "$t" ] && rect_inside "$t" "$l" "$r" "$c"; then
    ok "第 3 层矩形在屏内"
else
    bad "第 3 层矩形越界：$rect"
fi
send "<esc>" 0.6

# ── R5 右下角钳制 ───────────────────────────────────────────────
info "R5 列表窗格右下角右键：上翻/左翻且不压提示条"
right_at 67 13
rect="$(level_rect 0)"
t=$(printf '%s' "$rect" | cut -d' ' -f1); l=$(printf '%s' "$rect" | cut -d' ' -f2)
r=$(printf '%s' "$rect" | cut -d' ' -f3); c=$(printf '%s' "$rect" | cut -d' ' -f4)
if [ -n "$t" ] && rect_inside "$t" "$l" "$r" "$c"; then
    ok "右下角菜单被钳制在屏内（$t,$l,${r}x${c}）"
else
    bad "右下角菜单越界：$rect"
fi
if [ -n "$t" ] && [ "$t" -lt 12 ]; then
    ok "菜单在锚点上方（发生上翻）"
else
    bad "菜单没有上翻（top=$t）"
fi

# ── R6 关闭语义 ─────────────────────────────────────────────────
info "R6 菜单展开时右键 / 点菜单外：关闭且不执行"
closes_before="$(log_count '\[context_menu\] close')"
right_at 6 3
if [ -n "$(last_level_line 0)" ]; then :; fi
right_at 6 3
sleep 0.5
closes_after="$(log_count '\[context_menu\] close')"
if [ "$closes_after" -gt "$closes_before" ]; then ok "菜单内右键关闭了菜单"; else bad "菜单内右键没有关闭"; fi
right_at 6 3
send "<click:90:25>" 0.6
sleep 0.4
closes_after2="$(log_count '\[context_menu\] close')"
if [ "$closes_after2" -gt "$closes_after" ]; then ok "菜单外左键关闭了菜单"; else bad "菜单外左键没有关闭"; fi
if [ "$(play_name)" = "$track_before" ]; then
    ok "关闭菜单没有触发播放"
else
    bad "关闭菜单时误播放：$(play_name)"
fi

# ── R7 控制区弹层：右键取消 ──────────────────────────────────────
info "R7 控制区弹层展开时右键：只取消弹层"
send "C" 0.6
send "<right><right><right><space>" 1.0
if grep -q "\[controls\] popup shown" "$LOG_FILE"; then
    ok "控制区弹层已展开"
else
    bad "控制区弹层没有展开"
fi
right_at 50 15
if grep -q "right-click cancels controls popup" "$LOG_FILE"; then
    ok "右键取消了控制区弹层"
else
    bad "右键没有取消控制区弹层"
fi

# ── R8 设置页：保存设置 ─────────────────────────────────────────
info "R8 设置页右键：菜单 + 保存设置"
send "<f1>" 0.5
send "<f2>" 0.9
right_at 80 15
if grep -q "\[context_menu\] open view=1" "$LOG_FILE"; then
    ok "设置页右键打开了菜单"
else
    bad "设置页右键没有打开菜单"
fi
sidx="$(item_idx 0 'action=settings.save')"
if [ -n "$sidx" ] && click_level_item 0 "$sidx"; then
    if grep -q "\[context_menu\] action=settings.save" "$LOG_FILE"; then
        ok "执行了保存设置"
    else
        bad "没有执行保存设置"
    fi
else
    bad "找不到/点不中'保存设置'"
fi

# ── R9 其余页面菜单 ─────────────────────────────────────────────
info "R9 历史/歌单/收藏/语言/帮助页右键菜单"
for pair in "f3:2" "f4:3" "f5:4" "f7:7" "f8:6"; do
    key="${pair%%:*}"; viewnum="${pair#*:}"
    send "<$key>" 0.9
    right_at 50 15
    last_open="$(grep -h "\[context_menu\] open" "$LOG_FILE" | tail -1)"
    case "$last_open" in
        *"open view=$viewnum "*) ok "$key 页右键菜单打开（view=$viewnum）" ;;
        *) bad "$key 页右键菜单没有打开（期望 view=$viewnum，实测：${last_open##*] })" ;;
    esac
    send "<esc>" 0.5
done

# ── R10 信息页：唯一一条流光 ────────────────────────────────────
info "R10 信息页：严格一条'科乐美流光'"
send "<f6>" 0.9
right_at 50 15
if [ -n "$(last_level_line 0)" ] && grep -q "items=1" <<<"$(last_level_line 0)"; then
    ok "信息页菜单只有 1 条"
else
    bad "信息页菜单条目数不是 1：$(last_level_line 0)"
fi
if grep -q "level=0 idx=0 callback=rainbow" "$LOG_FILE"; then
    ok "该条目是流光回调"
else
    bad "信息页菜单条目不是流光回调"
fi
if click_level_item 0 0; then
    if grep -q "\[context_menu\] callback=rainbow" "$LOG_FILE" && grep -q "Aurora flow" "$LOG_FILE"; then
        ok "执行后触发流光开关"
    else
        bad "没有触发流光开关"
    fi
else
    bad "点不中流光条目"
fi
right_at 50 15
click_level_item 0 0 >/dev/null 2>&1 || true
sleep 0.5

# ── R10b 主视图信息窗格：右键不弹菜单 ────────────────────────────
info "R10b 主视图信息窗格：右键被吞掉，不弹菜单"
send "<f1>" 0.7
opens_before="$(log_count '\[context_menu\] open')"
right_at 30 20
sleep 0.5
last_mouse="$(grep -h "\\[mouse\\].*right-click" "$LOG_FILE" | tail -1)"
case "$last_mouse" in
    *"target=11"*) ok "右键落在信息窗格（target=INFO）" ;;
    *) bad "右键没有落在信息窗格：${last_mouse##*] }" ;;
esac
opens_after="$(log_count '\[context_menu\] open')"
if [ "$opens_after" -eq "$opens_before" ]; then
    ok "主视图信息窗格右键没有打开菜单"
else
    bad "主视图信息窗格右键打开了菜单"
fi

# ── R11 曲库浏览器 ──────────────────────────────────────────────
info "R11 曲库浏览器右键"
send "<f1>" 0.5
send "M" 0.8
right_at 6 3
if grep -q "\[context_menu\] open view=0 target=7" "$LOG_FILE"; then
    ok "曲库浏览器右键归属 BROWSER"
else
    bad "曲库浏览器右键归属异常"
fi
send "<esc>" 0.5
send "M" 0.5

# ── R12 布局编辑器 ──────────────────────────────────────────────
info "R12 布局编辑器：右键锚定动作菜单 + 退出编辑器"
send "<f2>" 0.9
send "<right>" 0.5
downs="$(printf '<down>%.0s' $(seq 1 20))"
send "${downs}<enter>" 1.5
if grep -q "\[layout_editor\] Opened layout" "$LOG_FILE"; then
    ok "布局编辑器已打开"
else
    bad "布局编辑器没有打开"
fi
right_at 80 15
if grep -q "actions menu anchored" "$LOG_FILE"; then
    ok "右键在指针处打开窗格操作菜单"
else
    bad "右键没有打开布局编辑器菜单"
fi
eline=$(grep -h "actions menu rect=" "$LOG_FILE" | tail -1)
erect=$(printf '%s' "$eline" | sed -E 's/.*rect=([0-9]+),([0-9]+),([0-9]+),([0-9]+).*/\1 \2 \3 \4/')
et=$(printf '%s' "$erect" | cut -d' ' -f1); el=$(printf '%s' "$erect" | cut -d' ' -f2)
er=$(printf '%s' "$erect" | cut -d' ' -f3); ec=$(printf '%s' "$erect" | cut -d' ' -f4)
if [ -n "$et" ] && rect_inside "$et" "$el" "$er" "$ec"; then
    ok "编辑器菜单矩形在屏内"
else
    bad "编辑器菜单矩形越界：$erect"
fi
ey=$((et + 1 + 10 + 1)); ex=$((el + 2 + 1))   # idx 10 = 退出编辑器
send "<click:$ex:$ey>" 0.9
if grep -q "\[layout_editor\] closed" "$LOG_FILE"; then
    ok "通过菜单退出了布局编辑器"
else
    bad "菜单没有退出布局编辑器"
fi

# ── R13 模态输入吞掉右键 ────────────────────────────────────────
info "R13 搜索输入框中的右键被吞掉"
send "<f1>" 0.6
send "L" 0.4
opens_before="$(log_count '\[context_menu\] open')"
send "/" 0.6
right_at 50 15
sleep 0.8
opens_after="$(log_count '\[context_menu\] open')"
if [ "$opens_after" -eq "$opens_before" ]; then
    ok "模态搜索框中的右键没有打开菜单"
else
    bad "模态搜索框中的右键打开了菜单"
fi
send "<esc>" 0.5
# 等输入框真正退出，避免下一条右键指令仍被模态循环吃掉。
for _ in $(seq 1 20); do
    grep -q "搜索已取消" "$LOG_FILE" && break
    sleep 0.2
done

# ── R14 队列版本竞态 ────────────────────────────────────────────
info "R14 菜单打开后队列被外部改动：移除动作拒绝执行"
right_at 6 3
l1_before="$(log_count "level=1 title='队列管理'")"
send "<down><right>" 1.0
for _ in $(seq 1 20); do
    [ "$(log_count "level=1 title='队列管理'")" -gt "$l1_before" ] && break
    sleep 0.2
done
if [ "$(log_count "level=1 title='队列管理'")" -gt "$l1_before" ]; then
    ok "键盘打开队列管理子菜单"
else
    bad "键盘没有打开队列管理子菜单"
fi
# 队列子菜单：插入(0)、追加(1)、分隔(2)、移除(3)；插入在无播放曲目时禁用，
# 所以按"移除之前有多少个可选且启用的条目"动态下移，而不是写死次数。
ridx="$(item_idx 1 'action=queue.remove')"
m=$(selectable_before 1 "$ridx")
downs="$(printf '<down>%.0s' $(seq 1 "$m"))"
send "$downs" 0.6
before="$(queue_count)"
rev_before="$(log_count 'queue revision')"
"$TM_BIN" queue append "$MUSIC/03-charlie.wav" >/dev/null 2>&1 || true
for _ in $(seq 1 25); do
    [ "$(log_count 'queue revision')" -gt "$rev_before" ] && break
    sleep 0.2
done
after_append="$(queue_count)"
stale_before="$(log_count 'stale action=queue.remove')"
send "<enter>" 0.8
for _ in $(seq 1 20); do
    [ "$(log_count 'stale action=queue.remove')" -gt "$stale_before" ] && break
    sleep 0.2
done
if [ "$(log_count 'stale action=queue.remove')" -gt "$stale_before" ]; then
    ok "队列版本变化时报告 stale"
else
    bad "队列版本变化没有报告 stale"
fi
sleep 0.6
after_attempt="$(queue_count)"
if [ "$after_attempt" = "$after_append" ] && [ "$after_append" != "$before" ]; then
    ok "stale 时没有执行移除（队列数保持 $after_attempt）"
else
    bad "stale 时仍改了队列：$before → $after_append → $after_attempt"
fi

# ── R15 稳态不自行关闭 ─────────────────────────────────────────
info "R15 菜单展开 2 s（动画继续）不自行关闭"
right_at 6 3
closes_before="$(log_count '\[context_menu\] close')"
sleep 2
closes_after="$(log_count '\[context_menu\] close')"
if [ "$closes_after" -eq "$closes_before" ]; then
    ok "2 s 内菜单没有自行关闭"
else
    bad "菜单在 2 s 内自行关闭了"
fi
send "<esc>" 0.5

# ── R16 动画窗格刷新时菜单保持完整 ─────────────────────────────
info "R16 播放中动画窗格刷新：菜单矩形不被擦除"
send "<f1>" 0.5
send "<click:5:3>" 1.0
playing="$(play_name)"
if [ -n "$playing" ] && [ "$playing" != "?" ]; then
    ok "已开始播放（$playing），频谱/歌词动画在跑"
else
    bad "没有开始播放，无法验证动画刷新"
fi
right_at 60 3
rect="$(level_rect 0)"
rleft=$(printf '%s' "$rect" | cut -d' ' -f2)
rcols=$(printf '%s' "$rect" | cut -d' ' -f4)
if [ -n "$rleft" ] && [ -n "$rcols" ] && [ $((rleft + rcols)) -gt 68 ]; then
    ok "菜单覆盖右侧动画窗格（left=$rleft cols=$rcols）"
else
    bad "菜单没有覆盖右侧动画窗格：$rect"
fi
reassert_before="$(log_count 'overlay reassert after refresh')"
for _ in $(seq 1 20); do
    [ "$(log_count 'overlay reassert after refresh')" -gt "$reassert_before" ] && break
    sleep 0.2
done
if [ "$(log_count 'overlay reassert after refresh')" -gt "$reassert_before" ]; then
    ok "动画窗格刷新走了菜单合成路径"
else
    bad "动画窗格刷新没有走菜单合成路径"
fi
snap_ok=0
for i in 1 2 3 4; do
    sleep 0.18
    cp "$TUI_LOG" "$WORK_DIR/r16-snap-$i.bin"
    if ! python3 "$SCRIPT_DIR/vt_grid.py" --cols 100 --rows 30 \
            --has '列.*表.*操.*作' "$WORK_DIR/r16-snap-$i.bin" >/dev/null 2>&1; then
        sleep 0.1
        cp "$TUI_LOG" "$WORK_DIR/r16-snap-$i.bin"
        python3 "$SCRIPT_DIR/vt_grid.py" --cols 100 --rows 30 \
            --has '列.*表.*操.*作' "$WORK_DIR/r16-snap-$i.bin" >/dev/null 2>&1 && snap_ok=$((snap_ok + 1))
    else
        snap_ok=$((snap_ok + 1))
    fi
done
if [ "$snap_ok" -eq 4 ]; then
    ok "4/4 动画帧快照仍含菜单标题"
else
    bad "只有 $snap_ok/4 快照含菜单标题（菜单被擦除）"
fi
send "<esc>" 0.5
if [ "$(play_name)" = "$playing" ]; then
    ok "菜单期间没有误切曲目（仍 $playing）"
else
    bad "菜单期间曲目被切走：$playing → $(play_name)"
fi

# ── R16b 修饰左键备选触发（pty 语义） ───────────────────────────
# 本段只验证 ncurses 层的归一化：Ctrl/Alt+左键被转成右键菜单。
# 注意 deepin-terminal 6.5.40 转发时只发按钮号、不带修饰位，故它上面的
# Ctrl/Alt+左键等同普通左键（会播放）——该终端的可靠入口是 R16c 的中键。
info "R16b Ctrl/Alt+左键等同于右键（只开一次、不播放）"
sleep 0.6  # 等 R16 最后一次 Esc 的 close 落盘后再取 marker
plain_before="$(plain_click_count)"

# 1) Ctrl+左键：新增日志里只有 open，没有紧跟的 close（防双触发）。
mark="$(menu_mark)"
send "<ctrl-click:6:3>" 0.9
sleep 1.0
new="$(menu_new "$mark")"
last="$(printf '%s\n' "$new" | tail -1)"
if printf '%s' "$last" | grep -q 'open'; then
    ok "Ctrl+左键打开了右键菜单（新增日志末条为 open）"
else
    bad "Ctrl+左键没有打开右键菜单（末条：${last:-空}）"
fi
if printf '%s' "$new" | grep -q 'close'; then
    bad "一次 Ctrl+左键紧跟了 close（双触发）"
else
    ok "一次 Ctrl+左键没有紧跟一次关闭（无双触发）"
fi
sleep 0.4

# 2) 菜单展开时 Ctrl+左键只收起，不叠加新菜单。
mark="$(menu_mark)"
send "<ctrl-click:6:3>" 0.9
sleep 1.0
new="$(menu_new "$mark")"
if printf '%s' "$new" | grep -q 'close' && \
        ! printf '%s' "$new" | grep -q 'open'; then
    ok "菜单展开时 Ctrl+左键只收起菜单（与右键一致）"
else
    bad "菜单展开时 Ctrl+左键没有只收起菜单"
fi
sleep 0.4

# 3) Alt+左键同样打开。
mark="$(menu_mark)"
send "<alt-click:6:3>" 0.9
sleep 1.0
new="$(menu_new "$mark")"
last="$(printf '%s\n' "$new" | tail -1)"
if printf '%s' "$last" | grep -q 'open'; then
    ok "Alt+左键同样打开右键菜单"
else
    bad "Alt+左键没有打开右键菜单"
fi
send "<esc>" 0.9
sleep 0.6

# 4) 指针跨格：press/release 不合并，只认 RELEASED，仍只开一次。
#    原始 SGR：16 = 左键+Ctrl，M=按下，m=松开；按下@(6,3)，松开@(6,4)。
mark="$(menu_mark)"
send "\x1b[<16;6;3M\x1b[<16;6;4m" 0.9
sleep 1.0
new="$(menu_new "$mark")"
last="$(printf '%s\n' "$new" | tail -1)"
if printf '%s' "$last" | grep -q 'open' && ! printf '%s' "$new" | grep -q 'close'; then
    ok "按下/松开跨格仍只打开一次菜单"
else
    bad "按下/松开跨格触发了多次打开/关闭"
fi
send "<esc>" 0.9
sleep 0.6

if [ "$(plain_click_count)" -eq "$plain_before" ]; then
    ok "修饰左键全程没有降级成普通左键（无播放）"
else
    bad "修饰左键被当成了普通左键（$plain_before → $(plain_click_count)）"
fi

# ── R16c 中键备选触发 ──────────────────────────────────────────
# deepin-terminal 把中键原样转发，且不为中键弹终端自家菜单；程序把中键
# 归一成右键菜单（见 mouse.c）。带修饰的中键刻意不认，留给终端。
info "R16c 中键等同于右键，带修饰的中键被忽略"
plain_before="$(plain_click_count)"

mark="$(menu_mark)"
send "<middle:6:3>" 0.9
sleep 1.0
new="$(menu_new "$mark")"
last="$(printf '%s\n' "$new" | tail -1)"
if printf '%s' "$last" | grep -q 'open'; then
    ok "中键打开了右键菜单（新增日志末条为 open）"
else
    bad "中键没有打开右键菜单（末条：${last:-空}）"
fi
if printf '%s' "$new" | grep -q 'close'; then
    bad "一次中键紧跟了 close（双触发）"
else
    ok "一次中键没有紧跟一次关闭"
fi
sleep 0.4

mark="$(menu_mark)"
send "<middle:6:3>" 0.9
sleep 1.0
new="$(menu_new "$mark")"
if printf '%s' "$new" | grep -q 'close' && \
        ! printf '%s' "$new" | grep -q 'open'; then
    ok "菜单展开时中键只收起菜单"
else
    bad "菜单展开时中键没有只收起菜单"
fi
sleep 0.4

for ev in ctrl-middle alt-middle shift-middle; do
    mark="$(menu_mark)"
    send "<$ev:6:3>" 0.7
    sleep 0.6
    new="$(menu_new "$mark")"
    if [ -z "$new" ]; then
        ok "$ev 不触发 TUI 菜单（无新增 open/close）"
    else
        bad "$ev 触发了 TUI 菜单"
    fi
done
send "<esc>" 0.9
sleep 0.6
if [ "$(plain_click_count)" -eq "$plain_before" ]; then
    ok "中键用例全程没有降级成普通左键（无播放）"
else
    bad "中键用例被当成了普通左键（$plain_before → $(plain_click_count)）"
fi

# ── R16d 带修饰的右键一律不认 ───────────────────────────────────
# 终端对同一组合键也弹自家菜单；TUI 再认就会两张菜单、overlay 错位。
info "R16d Ctrl/Alt/Shift+右键不触发 TUI 菜单，裸右键不受影响"
plain_before="$(plain_click_count)"
for ev in ctrl-right alt-right shift-right; do
    # 连续两次：第一次不能开，第二次也不能在"第一次已开"的假象下关。
    for _ in 1 2; do
        mark="$(menu_mark)"
        send "<$ev:6:3>" 0.7
        sleep 0.6
        new="$(menu_new "$mark")"
        if [ -z "$new" ]; then
            ok "$ev 不触发 TUI 菜单（无新增 open/close）"
        else
            bad "$ev 触发了 TUI 菜单"
        fi
    done
done

# 裸右键仍照旧（终端把右键转发给程序且不弹自家菜单时走这条老路）
mark="$(menu_mark)"
right_at 6 3
sleep 0.6
new="$(menu_new "$mark")"
last="$(printf '%s\n' "$new" | tail -1)"
if printf '%s' "$last" | grep -q 'open'; then
    ok "裸右键菜单未受影响"
else
    bad "裸右键菜单被改坏了"
fi
send "<esc>" 0.9
sleep 0.6
if [ "$(plain_click_count)" -eq "$plain_before" ]; then
    ok "带修饰右键用例全程没有降级成普通左键（无播放）"
else
    bad "带修饰右键用例被当成了普通左键（$plain_before → $(plain_click_count)）"
fi

# ── R17 前端存活 ───────────────────────────────────────────────
if kill -0 "$(cat "$WORK_DIR/tui.pid" 2>/dev/null)" 2>/dev/null; then
    ok "跑完全部右键用例后前端仍存活"
else
    bad "前端在用例中退出（rc=$(cat "$WORK_DIR/tui.rc" 2>/dev/null)）"
fi

send "q" 1.0
wait "$DRIVER_PID" 2>/dev/null || true
DRIVER_PID=""

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
