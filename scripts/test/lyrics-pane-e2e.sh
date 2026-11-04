#!/usr/bin/env bash
#
# 歌词窗格"让位 / 自动恢复"端到端回归
#
# 回归对象（2026-11-04 定性、已批准的修复计划 plan-lyrics-pane-fix.md）：
#   双格布局（lyrics + vinyl）里，当前曲目没有歌词时歌词格临时让位、余格补位；
#   换到有歌词的曲目时歌词格必须**自动回来**，不需要切视图（F2→F1）、不需要
#   改终端尺寸、也不需要任何按键。
#
# 旧实现的缺陷：ui.c 的让位块用"边沿检测 + 首帧只记种子"驱动重建。启动自动
# 播放时歌词先到、首次 sync 后到，唯一的翻转被种子分支吃掉，期望值与实际
# 生效几何永久不一致 ⇒ 歌词格再也不出现，只有 F2→F1 的无条件 create_layout
# 才能撞回来。
#
# 判定依据：
#   · 画面事实：lifecycle-driver.py 把 pty 原始字节写进 tui.bin，本脚本用
#     scripts/test/vt_grid.py 还原字符网格，断言窗格标题在不在（标题同时接受
#     中英文，避免 i18n 回退导致误判）。vt_grid 只用于"有无"，不用于量宽度。
#   · 行为事实：ter-music show --json 的 track.path 判当前曲目；play <文件>
#     切曲（与真实用户操作同一条 D-Bus 链路）。
#   · 抖动事实：前端 --debug 日志里的 `[layout] lyrics pane: want=.. applied=..`
#     只在生效让位值变化时打一行；稳态窗口必须零新增。
#
# 布局：使用一次性 HOME 里的用户 layouts.json **覆盖内置 vinly 预设**，树为
#   row 680 [playlist, col 500 [lyrics, vinyl]]
# 复现用户"两格同在"的真实现场（内置 vinyl 只有 vinyl，不覆盖就测不到让位）。
#
# 覆盖用例：
#   T1 启动即播内嵌 lyrics-eng 歌词曲目：≤5 s 内歌词标题出现，黑胶/播放列表标题都在。
#   T2 切到无歌词曲目：歌词标题消失、黑胶标题仍在。
#   T3 切回有歌词曲目：≤2 s 内歌词标题自动回来，期间不发任何按键。
#   T4 稳态 15 s：让位日志零新增、全程 ≤6 行、最后一行 want=0 applied=0、
#      前端仍存活。
#
# 用法：
#   scripts/test/lyrics-pane-e2e.sh [--bin <ter-music>] [--frontend remote|local] [--keep]
#
# 环境：会话总线（缺省自行 dbus-run-session）、ffmpeg 造素材、python3。
#       宿主被判定为如意玲珑容器时 CLI 拒绝拉起后台核心，故缺省置
#       TER_MUSIC_SANDBOX=0（已设置则沿用调用者的值）。
#       慢机器排障可用 LYRICS_E2E_T3_TIMEOUT 临时放宽 T3 的 2 s，不默认放宽。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
FRONTEND="remote"
KEEP=0
WORK_DIR=""
DRIVER_PID=""
CTL_FILE=""
TUI_LOG=""
COLS=100
ROWS=30
T3_TIMEOUT="${LYRICS_E2E_T3_TIMEOUT:-2}"

usage() { sed -n '2,52p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

ORIG_ARGS=("$@")
while [ $# -gt 0 ]; do
    case "$1" in
        --bin)      TM_BIN="$2"; shift 2 ;;
        --frontend) FRONTEND="$2"; shift 2 ;;
        --keep)     KEEP=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage; exit 2 ;;
    esac
done
case "$FRONTEND" in
    remote|local) ;;
    *) echo "错误：--frontend 只能是 remote 或 local（实际 '$FRONTEND'）" >&2; exit 2 ;;
esac

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
[ -f "$SCRIPT_DIR/vt_grid.py" ] || { echo "错误：缺少 $SCRIPT_DIR/vt_grid.py" >&2; exit 2; }

DRIVER="$SCRIPT_DIR/lifecycle-driver.py"
VT="$SCRIPT_DIR/vt_grid.py"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-lyrics-pane-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_CACHE_HOME="$HOME/.cache"
# 宿主若被判定为如意玲珑容器，CLI 会拒绝 fork 后台核心；这里默认关掉该判定。
export TER_MUSIC_SANDBOX="${TER_MUSIC_SANDBOX:-0}"
# 语言以 config.xml 的 ui_language 为准，避免外部 CLI 语言变量干扰标题断言。
unset TER_MUSIC_CLI_LANG
LOG_DIR="$WORK_DIR/logs"
export TER_MUSIC_LOG_DIR="$LOG_DIR"
MUSIC="$WORK_DIR/music"
CONFIG_DIR="$XDG_CONFIG_HOME/ter-music"
mkdir -p "$CONFIG_DIR" "$LOG_DIR" "$MUSIC"

# 30 秒的音：断言窗口里不会自己切歌。00 的歌词在 FLAC 的 lyrics-eng 标签
# （用户启动曲目的 ID3 USLT 在 FFmpeg 里同样是这个键）；01 配同名 .lrc；
# 02 完全没有歌词。
tone() { ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "sine=frequency=$1:duration=30" -c:a pcm_s16le -y "$2" 2>/dev/null; }
tone 330 "$MUSIC/01-with-lrc.wav"
tone 523 "$MUSIC/02-no-lrc.wav"
embedded_lyrics="$(printf '[00:02.00]内嵌第一句\n[00:05.00]内嵌第二句\n[00:08.00]内嵌第三句\n[00:12.00]内嵌第四句\n')"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "sine=frequency=392:duration=30" -c:a flac \
    -metadata "lyrics-eng=$embedded_lyrics" -y "$MUSIC/00-embedded.flac" 2>/dev/null
printf '[00:02.00]第一句\n[00:05.00]第二句\n[00:08.00]第三句\n[00:12.00]第四句\n[00:20.00]第五句\n' \
    > "$MUSIC/01-with-lrc.lrc"

info "写一次性配置：auto_play_on_start=1、布局 = 双格 vinyl（lyrics + vinyl）"
cat > "$CONFIG_DIR/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <auto_play_on_start>1</auto_play_on_start>
    <resume_last_playback>0</resume_last_playback>
    <ui_language>zh_CN</ui_language>
  </preferences>
</ter-music-config>
EOF
cat > "$CONFIG_DIR/layouts.json" <<'EOF'
{
  "version": 2,
  "default": "vinyl",
  "layouts": [
    {
      "id": "vinyl",
      "name": "Vinyl",
      "tree": {
        "split": "row",
        "ratio": 680,
        "children": [
          { "pane": "playlist", "fg": 15, "bg": -1 },
          {
            "split": "col",
            "ratio": 500,
            "children": [
              { "pane": "lyrics", "fg": 198, "bg": -1 },
              { "pane": "vinyl", "fg": 203, "bg": -1 }
            ]
          }
        ]
      }
    }
  ]
}
EOF
printf 'vinyl\n' > "$CONFIG_DIR/ui-layout"

# ── 观测助手 ──────────────────────────────────────────────────────
CTL_FILE="$WORK_DIR/keys.cmd"; : > "$CTL_FILE"
TUI_LOG="$WORK_DIR/tui.bin"

send() { printf 'key:%s\n' "$1" >> "$CTL_FILE"; sleep "${2:-0.4}"; }

mlog() {   # 前端自己的调试日志（同目录里还有核心日志，挑含 [layout] Selected layout 的那份）
    local f
    for f in $(ls -t "$LOG_DIR"/*.log 2>/dev/null); do
        grep -q "\[layout\] Selected layout" "$f" 2>/dev/null && { printf '%s' "$f"; return; }
    done
    printf ''
}

show_get() {   # $1 = python 表达式，作用在 show --json 的 dict 上
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

wait_track() {   # $1 = 期望文件名, $2 = 超时秒
    local i
    for i in $(seq 1 "$(( ${2:-10} * 4 ))"); do
        [ "$(play_name)" = "$1" ] && return 0
        sleep 0.25
    done
    return 1
}

snapshot() {   # 把持续增长的 pty 流截一段到 snap.bin；解析整段前缀即当前画面
    [ -f "$TUI_LOG" ] || return 1
    cp "$TUI_LOG" "$WORK_DIR/snap.bin" 2>/dev/null || return 1
    [ -s "$WORK_DIR/snap.bin" ]
}

screen_has() {   # $1 = 正则（同时接受中英文标题）
    snapshot || return 1
    python3 "$VT" --cols "$COLS" --rows "$ROWS" --has "$1" "$WORK_DIR/snap.bin" >/dev/null 2>&1
}

wait_screen() {   # $1 = 正则, $2 = 超时秒
    local i
    for i in $(seq 1 "$(( ${2:-5} * 5 ))"); do
        screen_has "$1" && return 0
        sleep 0.2
    done
    return 1
}

wait_screen_gone() {   # $1 = 正则, $2 = 超时秒
    local i
    for i in $(seq 1 "$(( ${2:-5} * 5 ))"); do
        screen_has "$1" || return 0
        sleep 0.2
    done
    return 1
}

dump_screen() {
    snapshot || { printf '         （还没有 pty 输出）\n'; return; }
    python3 "$VT" --cols "$COLS" --rows "$ROWS" "$WORK_DIR/snap.bin" 2>/dev/null |
        tail -n 30 | sed 's/^/         /'
}

layout_line_count() {
    local L; L="$(mlog)"
    [ -n "$L" ] || { echo 0; return; }
    grep -c "\[layout\] lyrics pane:" "$L" 2>/dev/null || true
}

# 窗格标题在边框上绘制为 "╭─<标题>"（util.c 的 rounded_box + ui_pane_begin），
# 必须带上 "╭─" 前缀：黑胶回退动画的正文里也有"暂无歌词"字样，裸词会误判。
lyrics_title()   { printf '╭─(歌词|Lyrics)'; }
vinyl_title()    { printf '╭─(黑胶碟片|Vinyl)'; }
playlist_title() { printf '╭─(播放列表|Playlist)'; }

info "起会话：frontend=$FRONTEND，窗口 ${COLS}x${ROWS}，--debug"
EXTRA=("--debug")
if [ "$FRONTEND" = "local" ]; then
    EXTRA+=("--frontend=local")
fi
"$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
python3 "$DRIVER" drive "$TM_BIN" "$MUSIC" "$TUI_LOG" \
    --ctl "$CTL_FILE" --rc-file "$WORK_DIR/tui.rc" --pid-file "$WORK_DIR/tui.pid" \
    --cols "$COLS" --rows "$ROWS" --max-runtime 150 -- "${EXTRA[@]}" >/dev/null 2>&1 &
DRIVER_PID=$!

# ── T1 启动即播内嵌 lyrics-eng 歌词曲目 ──────────────────────────
info "T1 启动自动播放 00-embedded.flac（lyrics-eng 标签）：≤5 s 内歌词标题出现"
if wait_track 00-embedded.flac 20; then
    ok "启动后自动播放到 00-embedded.flac"
else
    bad "启动后没有播到 00-embedded.flac（实测 $(play_name)）"
    dump_screen
fi
if [ -n "$(mlog)" ] && grep -q "Selected layout 'vinyl'" "$(mlog)" 2>/dev/null; then
    ok "活动布局为用户覆盖的双格 vinyl"
else
    bad "前端没有选中双格 vinyl 布局（日志=$(mlog)）"
fi
if wait_screen "$(lyrics_title)" 5; then
    ok "启动后 5 s 内歌词窗格出现（回归目标）"
else
    bad "启动后 5 s 内歌词窗格没有出现（旧缺陷会在这里红）"
    dump_screen
    grep -h "\[layout\] lyrics pane:" "$(mlog)" 2>/dev/null | tail -4 | sed 's/^/         /'
fi
if wait_screen "内嵌第一句" 5; then
    ok "歌词来自 lyrics-eng 内嵌标签（不是 .lrc）"
else
    bad "画面没有 lyrics-eng 内嵌歌词文本"
    dump_screen
fi
if screen_has "$(vinyl_title)"; then
    ok "黑胶窗格与歌词窗格同时存在"
else
    bad "黑胶窗格标题不在画面上"
    dump_screen
fi
if screen_has "$(playlist_title)"; then
    ok "播放列表窗格仍在"
else
    bad "播放列表窗格标题不在画面上"
    dump_screen
fi

# ── T2 切到无歌词曲目 ───────────────────────────────────────────
info "T2 play 02-no-lrc.wav：歌词格让位、黑胶格保留"
"$TM_BIN" play "$MUSIC/02-no-lrc.wav" >/dev/null 2>&1
if wait_track 02-no-lrc.wav 10; then
    ok "已切到 02-no-lrc.wav"
else
    bad "没有切到 02-no-lrc.wav（实测 $(play_name)）"
fi
if wait_screen_gone "$(lyrics_title)" 5; then
    ok "无歌词曲目下歌词窗格标题消失（让位生效）"
else
    bad "无歌词曲目下歌词窗格标题仍在（让位没有生效）"
    dump_screen
fi
if screen_has "$(vinyl_title)"; then
    ok "让位后黑胶窗格仍在"
else
    bad "让位后黑胶窗格标题也不见了"
    dump_screen
fi

# ── T3 切回有歌词曲目：不按键自动恢复 ──────────────────────────
info "T3 play 01-with-lrc.wav：≤${T3_TIMEOUT} s 内自动恢复，期间不发任何按键"
t3_start_ms="$(date +%s%3N)"
if "$TM_BIN" play "$MUSIC/01-with-lrc.wav" >/dev/null 2>&1; then
    ok "切回 01-with-lrc.wav 的命令成功"
else
    bad "切回 01-with-lrc.wav 的命令失败"
fi
if wait_screen "$(lyrics_title)" "$T3_TIMEOUT"; then
    t3_elapsed=$(( $(date +%s%3N) - t3_start_ms ))
    ok "歌词窗格自动恢复，实测 ${t3_elapsed} ms（无任何按键）"
else
    bad "歌词窗格没有自动恢复（旧缺陷会在这里红）"
    dump_screen
    grep -h "\[layout\] lyrics pane:" "$(mlog)" 2>/dev/null | tail -4 | sed 's/^/         /'
fi

# ── T4 稳态不抖动 + 前端存活 ────────────────────────────────────
info "T4 稳态播放 15 s：让位日志零新增、全程 ≤6 行、最终 want=0 applied=0"
lines_before="$(layout_line_count)"
sleep 15
lines_after="$(layout_line_count)"
if [ "$lines_after" -eq "$lines_before" ]; then
    ok "稳态 15 s 内没有新的让位重建日志（${lines_before} → ${lines_after}）"
else
    bad "稳态期间发生抖动：让位日志 ${lines_before} → ${lines_after}"
    grep -h "\[layout\] lyrics pane:" "$(mlog)" 2>/dev/null | tail -8 | sed 's/^/         /'
fi
if [ "$lines_after" -ge 2 ] && [ "$lines_after" -le 6 ]; then
    ok "全程让位日志行数合理（${lines_after} 行，期望 2..6）"
else
    bad "全程让位日志行数异常（${lines_after} 行，期望 2..6）"
fi
if [ -n "$(mlog)" ]; then
    last_line="$(grep -h "\[layout\] lyrics pane:" "$(mlog)" 2>/dev/null | tail -1)"
else
    last_line=""
fi
case "$last_line" in
    *"want=0 applied=0"*) ok "最后一条让位日志为 want=0 applied=0（歌词格已生效）" ;;
    *) bad "最后一条让位日志不是 want=0 applied=0：${last_line:-<无>}" ;;
esac
if kill -0 "$(cat "$WORK_DIR/tui.pid" 2>/dev/null)" 2>/dev/null; then
    ok "跑完全部歌词窗格用例后前端仍存活（没有崩溃）"
else
    bad "前端在用例中退出（rc=$(cat "$WORK_DIR/tui.rc" 2>/dev/null)）"
fi

send "q" 1.0
wait "$DRIVER_PID" 2>/dev/null || true
DRIVER_PID=""

info "结果（frontend=$FRONTEND）"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
