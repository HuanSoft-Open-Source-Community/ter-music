#!/usr/bin/env bash
#
# 统一文件/目录选择器端到端回归（t2）
#
# 覆盖真实 TUI + pty 下的四个路径选择入口与三种后端语义：
#
#   Z1  打开文件夹（主视图 o）：zenity 后端带 --file-selection --directory，
#       选中后按原有 playlist 加载路径走（状态行"目录加载成功"、
#       last_opened_path 落到配置）。
#   Z2  追加文件夹（主视图 I）：同样走 --directory，按 append_playlist 追加
#       （状态行"已追加 N 首歌曲"），不改回"打开"的语义。
#   Z3  语言包添加（F7 → a）：这是**文件**选择，zenity 不允许带 --directory，
#       过滤器是 *.tar.gz *.tgz；选中后仍走 i18n_add_language() 原有校验/安装
#       （用户语言目录出现 <id>.xml）。
#   Z4  默认音乐目录（F2 → 默认路径 → 回车）：目录选择，标题/--directory 正确，
#       保存后配置 default_startup_path 更新、状态行"默认路径已保存"。
#   Z5  取消（退出码 1）：不写配置、不显示错误（三种后端一致）。
#   Z6  错误（对话框回传不存在/非目录路径）：可读错误可见、配置保持原值、TUI 存活。
#   K1  kdialog 后端：目录模式必须用 --getexistingdirectory，文件模式必须用
#       --getopenfilename（且目录模式不传过滤器）。
#   T1  TTY 回退：PATH 无 zenity/kdialog 且无 DISPLAY/WAYLAND_DISPLAY 时自动落到
#       TTY 手输（或系统装有对话框时显式强制 tty），input 支持 ~/ 展开、
#       空格/UTF-8 原样；空输入=取消；设置页默认路径同样可手输。
#   X1  备用屏幕（t8 回归）：从 TUI 打开 zenity/kdialog/portal 三种桌面对话框
#       期间，pty 字节不得出现 rmcup（\E[?1049l 等离开备用屏幕的序列），
#       也不得重新 smcup；对话框打开期间 TUI 进程存活，关闭后 TUI 画面/状态行
#       可识别（不重启、不白屏）。假对话框支持 HOLD 门，可在"对话框仍打开"
#       的时刻抓 pty 片段断言，而不是只看结束后。
#   X2  子进程 stderr：假对话框故意向 stderr 写噪音，pty 流里必须看不到
#       （stdout 仍只回传路径）。
#   L1  AppImage 旧库污染（t18）：AppRun 的 LD_LIBRARY_PATH 不得传给宿主
#       zenity/kdialog（子进程看到的必须为空）；对话框异常退出（127，模拟
#       GLIBCXX 不匹配崩溃）时回退 TTY 手输并在提示里说明原因；用户取消
#       （1/5）不回退。
#
# 判定依据（都是程序自己的事实，不靠猜屏幕）：
#   · PATH 里的假 zenity/kdialog 记录到的 argv（--directory/--getexistingdirectory/
#     --file-filter/--title），以及子进程 cwd；
#   · 前端 --debug 日志里的 [file_picker] backend=.../picked/cancelled/failed；
#   · D-Bus 事实：CLI `config get last_opened_path|default_startup_path`；
#   · 用户语言目录里实际安装的 <id>.xml；
#   · pty 收屏（vt_grid.py）里的状态行文案。
#
# 用法：scripts/test/folder-picker-e2e.sh [--bin <ter-music>] [--keep]
# 环境：会话总线（缺省自行 dbus-run-session；可 TM_TEST_REUSE_SESSION_BUS=1 复用）、
#       ffmpeg（造素材）、python3（lifecycle-driver/vt_grid）、tar（语言包夹具）。
# 可重复运行：全部夹具在 mktemp 目录里自建自删，假对话框只进 PATH，不写系统。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

PASS=0
FAIL=0
SKIPPED=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
skip() { SKIPPED=$((SKIPPED + 1)); printf '  SKIP %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

# ── 私有会话总线（与其它 E2E 同一约定）────────────────────────────────
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ "${TM_TEST_REUSE_SESSION_BUS:-0}" != "1" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：需要 dbus-run-session，或设 TM_TEST_REUSE_SESSION_BUS=1。" >&2
        exit 2
    fi
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（用 --bin 指定）" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "错误：需要 python3" >&2; exit 2; }
command -v tar >/dev/null 2>&1 || { echo "错误：需要 tar（语言包夹具）" >&2; exit 2; }
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "SKIP folder-picker-e2e：需要 ffmpeg 造音频夹具（源 .tmp/test-env.sh 后重跑）"
    exit 0
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-folder-picker-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_CACHE_HOME="$HOME/.cache"
export XDG_DATA_HOME="$HOME/.local/share"
export TER_MUSIC_SANDBOX="${TER_MUSIC_SANDBOX:-0}"
export TER_MUSIC_LOG_DIR="$WORK_DIR/logs"
unset TER_MUSIC_CLI_LANG TERM_MUSIC_FILE_CHOOSER
mkdir -p "$XDG_CONFIG_HOME/ter-music" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
         "$TER_MUSIC_LOG_DIR" "$WORK_DIR/run"

CONFIG="$XDG_CONFIG_HOME/ter-music/config.xml"
USER_LANG_DIR="$XDG_CONFIG_HOME/ter-music/lang"
DRIVER="$SCRIPT_DIR/lifecycle-driver.py"
VT="$SCRIPT_DIR/vt_grid.py"
CTL="$WORK_DIR/keys.cmd"
TUI_LOG="$WORK_DIR/tui.bin"
RC_FILE="$WORK_DIR/tui.rc"
PID_FILE="$WORK_DIR/tui.pid"
DRIVER_PID=""
LOG_FILE=""
LABEL=""

BASE_PATH="$PATH"
FAKEBIN="$WORK_DIR/bin"
FAKE_LOG="$WORK_DIR/fake-dialog.log"
FAKE_SPEC="$WORK_DIR/fake-dialog.spec"
mkdir -p "$FAKEBIN"
EMPTY_BIN="$WORK_DIR/empty-bin"
mkdir -p "$EMPTY_BIN"

cleanup() {
    if [ -n "$DRIVER_PID" ]; then
        kill -9 "$DRIVER_PID" 2>/dev/null || true
        wait "$DRIVER_PID" 2>/dev/null || true
    fi
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    if [ "$KEEP" -eq 0 ] && [ -n "${WORK_DIR:-}" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    else
        [ -n "${WORK_DIR:-}" ] && printf '保留工作目录：%s\n' "$WORK_DIR"
    fi
}
trap cleanup EXIT

# ── 夹具：初始曲库 / 打开目录 / 追加目录 / 带空格 UTF-8 目录 / 语言包 ──
FIX="$WORK_DIR/fixtures"
INIT_DIR="$FIX/initial"
OPEN_DIR="$FIX/open"
APPEND_DIR="$FIX/append"
WEIRD_DIR="$HOME/午休 音乐"
TTY_DIR="$HOME/TTY 目录 2"
LANG_DIR_FIX="$FIX/langs"
LANG_PACK="$LANG_DIR_FIX/xx_AA.tar.gz"
mkdir -p "$INIT_DIR" "$OPEN_DIR" "$APPEND_DIR" "$WEIRD_DIR" "$TTY_DIR" "$LANG_DIR_FIX"
ERROR_DIR="$WORK_DIR/no-such-dir"

tone() { ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "sine=frequency=$1:duration=3" -c:a pcm_s16le -y "$2" 2>/dev/null; }
tone 300 "$INIT_DIR/01-init.wav"
tone 320 "$OPEN_DIR/01-open.wav"
tone 340 "$OPEN_DIR/02-open.wav"
tone 360 "$APPEND_DIR/01-append.wav"
tone 380 "$WEIRD_DIR/01-weird.wav"
tone 400 "$TTY_DIR/01-tty.wav"

cat > "$WORK_DIR/lang.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<lang id="xx_AA" name="Testish (e2e)">
  <string key="lang.name">Testish (e2e)</string>
</lang>
EOF
tar -czf "$LANG_PACK" -C "$WORK_DIR" lang.xml
rm -f "$WORK_DIR/lang.xml"

# ── 配置夹具（v12；空路径，避免"默认音乐目录"干扰断言）──────────────
cat > "$CONFIG" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="12">
  <preferences>
    <auto_play_on_start>0</auto_play_on_start>
    <remember_last_path>1</remember_last_path>
    <stop_core_on_exit>0</stop_core_on_exit>
    <ui_language>zh_CN</ui_language>
    <volume_percent>0</volume_percent>
  </preferences>
  <paths>
    <default_startup_path></default_startup_path>
    <last_opened_path></last_opened_path>
  </paths>
</ter-music-config>
EOF

# ── 假 zenity/kdialog：记录 argv/cwd，按指令文件回显路径与退出码 ──────
# 指令文件第 1 行=要回显的路径（空=不回显），第 2 行=退出码（空/缺省=0）。
# zenity/kdialog 的取消约定都是 exit 1；其它非 0 按错误。
cat > "$FAKEBIN/zenity" <<'FAKE'
#!/bin/sh
{
  printf 'BEGIN prog=%s cwd=%s argc=%s\n' "${0##*/}" "$(pwd)" "$#"
  n=0
  for a in "$@"; do printf 'ARG%d=%s\n' "$n" "$a"; n=$((n+1)); done
  printf 'END\n'
  printf 'ENV_LD_LIBRARY_PATH=%s\n' "${LD_LIBRARY_PATH:-}"
  printf 'ENV_LD_PRELOAD=%s\n' "${LD_PRELOAD:-}"
} >> "${TM_FAKE_DIALOG_LOG:?}"
# 子进程 stderr 必须被选择器丢弃：这里故意写噪音，E2E 断言它从不出现在 pty。
printf 'TM_FAKE_DIALOG_STDERR_NOISE prog=%s\n' "${0##*/}" >&2
# 可选 HOLD 门：E2E 在"对话框仍打开"时检查备用屏幕字节（见 X1）。
if [ -n "${TM_FAKE_DIALOG_HOLD:-}" ] && [ -e "$TM_FAKE_DIALOG_HOLD" ]; then
  printf 'HOLDING\n' >> "${TM_FAKE_DIALOG_LOG:?}"
  i=0
  while [ -e "$TM_FAKE_DIALOG_HOLD" ] && [ "$i" -lt 200 ]; do
    sleep 0.05
    i=$((i+1))
  done
fi
out=""
code=0
if [ -n "${TM_FAKE_DIALOG_SPEC:-}" ] && [ -f "$TM_FAKE_DIALOG_SPEC" ]; then
  out=$(sed -n '1p' "$TM_FAKE_DIALOG_SPEC")
  code=$(sed -n '2p' "$TM_FAKE_DIALOG_SPEC")
  [ -n "$code" ] || code=0
fi
[ -n "$out" ] && printf '%s\n' "$out"
exit "$code"
FAKE
cp "$FAKEBIN/zenity" "$FAKEBIN/kdialog"
chmod 755 "$FAKEBIN/zenity" "$FAKEBIN/kdialog"

export TM_FAKE_DIALOG_LOG="$FAKE_LOG"
export TM_FAKE_DIALOG_SPEC="$FAKE_SPEC"
fake_spec() {   # $1=回显路径（空=取消/不回显） $2=退出码
    printf '%s\n%s\n' "${1:-}" "${2:-0}" > "$FAKE_SPEC"
}
fake_mark() { [ -f "$FAKE_LOG" ] && wc -l < "$FAKE_LOG" || printf '0'; }
fake_since() { tail -n +"$(( ${1:-0} + 1 ))" "$FAKE_LOG" 2>/dev/null; }
fake_has() { fake_since "$1" | grep -qF -- "$2"; }
fake_count() { fake_since "$1" | grep -cF -- "$2" 2>/dev/null || true; }

export TM_FAKE_DIALOG_HOLD="$WORK_DIR/fake-dialog.hold"
fake_hold_begin() { : > "$TM_FAKE_DIALOG_HOLD"; }
fake_hold_release() { rm -f "$TM_FAKE_DIALOG_HOLD"; }
fake_wait_holding() {   # $1=日志行标记 $2=超时秒；假对话框报告已进入 HOLD
    local mark="$1" timeout="${2:-10}" i
    for i in $(seq 1 $((timeout * 10))); do
        fake_since "$mark" | grep -qx 'HOLDING' && return 0
        sleep 0.1
    done
    return 1
}

# pty 原始字节里找"备用屏幕切换"序列：xterm 系 terminfo 的 smcup/rmcup 是
# \E[?1049h / \E[?1049l（少数终端用 \E[?47h/l），两个都查。$3=leave 只查
# 离开（rmcup），any 进入/离开都查。命中时打印序列名，未命中打印空。
segment_alt_screen_hits() {   # $1=pty 日志 $2=字节偏移 $3=leave|any
    python3 - "$1" "$2" "${3:-any}" <<'PY'
import sys

path, offset, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3]
try:
    data = open(path, 'rb').read()[offset:]
except OSError:
    data = b''
seqs = []
if mode in ('leave', 'any'):
    seqs += [('rmcup(1049l)', b'\x1b[?1049l'), ('rmcup(47l)', b'\x1b[?47l')]
if mode == 'any':
    seqs += [('smcup(1049h)', b'\x1b[?1049h'), ('smcup(47h)', b'\x1b[?47h')]
print(','.join(name for name, seq in seqs if seq in data))
PY
}
segment_dump() {   # $1=pty 日志 $2=起始偏移 $3=输出文件（证据片段）
    python3 - "$1" "$2" "$3" <<'PY'
import sys

try:
    data = open(sys.argv[1], 'rb').read()
except OSError:
    data = b''
open(sys.argv[3], 'wb').write(data[int(sys.argv[2]):])
PY
}
tui_alive() { [ -s "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; }

assert_no_alt_screen_leave() {   # $1=描述 $2=起始字节偏移
    local hits
    hits="$(segment_alt_screen_hits "$TUI_LOG" "$2" leave)"
    if [ -z "$hits" ]; then
        ok "$1：没有 rmcup（TUI 保持备用屏幕）"
    else
        bad "$1：出现 $hits（TUI 退出备用屏幕，对话框背后会露出 shell）"
    fi
}
assert_no_alt_screen_toggle() {   # $1=描述 $2=起始字节偏移
    local hits
    hits="$(segment_alt_screen_hits "$TUI_LOG" "$2" any)"
    if [ -z "$hits" ]; then
        ok "$1：没有 rmcup/smcup（备用屏幕未切换）"
    else
        bad "$1：出现 $hits（备用屏幕被切换）"
    fi
}
assert_no_stderr_noise() {   # $1=描述：子进程 stderr 不得画到 curses 屏幕
    if python3 - "$TUI_LOG" <<'PY'
import sys

try:
    data = open(sys.argv[1], 'rb').read()
except OSError:
    data = b''
sys.exit(1 if b'TM_FAKE_DIALOG_STDERR_NOISE' in data else 0)
PY
    then
        ok "$1：子进程 stderr 没有画到 curses 屏幕"
    else
        bad "$1：子进程 stderr 噪音出现在 pty（未重定向）"
    fi
}

# 打开外部对话框并保留一个"对话框仍打开"的可控窗口（t8 X1）：
#   HOLD 文件存在 → 假对话框记录 HOLDING 并停住；
#   本函数在停住期间抓 pty 片段断言没有 rmcup/进程没重启，然后放开 HOLD。
# 调用后可用 $DLG_MARK 断言本次调用的 argv（argv 在 HOLD 之前已记录）。
# $1=描述 $2=回显路径 $3=退出码 $4=按键（默认 o）
dialog_with_hold() {
    local desc="$1" out="$2" code="$3" key="${4:-o}"
    DLG_MARK="$(fake_mark)"
    DLG_TUI_OFF="$(wc -c < "$TUI_LOG")"
    fake_hold_begin
    fake_spec "$out" "$code"
    send "$key" 0.2
    if fake_wait_holding "$DLG_MARK" 10; then
        segment_dump "$TUI_LOG" "$DLG_TUI_OFF" "$WORK_DIR/segment-$LABEL-$desc-mid.bin"
        assert_no_alt_screen_leave "$desc（对话框打开期间）" "$DLG_TUI_OFF"
        if tui_alive; then
            ok "$desc：对话框打开期间 TUI 进程存活（未重启）"
        else
            bad "$desc：对话框打开期间 TUI 进程消失"
        fi
    else
        bad "$desc：假对话框没有进入 HOLD（无法验证对话框打开期间的备用屏幕）"
    fi
    fake_hold_release
    sleep 0.9
    segment_dump "$TUI_LOG" "$DLG_TUI_OFF" "$WORK_DIR/segment-$LABEL-$desc-full.bin"
    assert_no_alt_screen_toggle "$desc（含对话框返回）" "$DLG_TUI_OFF"
    if tui_alive; then
        ok "$desc：对话框关闭后 TUI 进程仍存活"
    else
        bad "$desc：对话框关闭后 TUI 进程消失"
    fi
}

# ── 前端日志 / 屏幕 / 配置辅助 ───────────────────────────────────────
flog_count() {   # $1 = 固定串
    local n=0
    if [ -n "$LOG_FILE" ]; then
        n="$(grep -cF -- "$1" "$LOG_FILE" 2>/dev/null || true)"
    fi
    printf '%s' "${n:-0}"
}
flog_has() { [ -n "$LOG_FILE" ] && grep -qF -- "$1" "$LOG_FILE" 2>/dev/null; }

screen_has() {
    [ -s "$TUI_LOG" ] || return 1
    python3 "$VT" --cols "${TERM_COLS:-100}" --rows "${TERM_ROWS:-30}" \
        --has "$1" "$TUI_LOG" >/dev/null 2>&1
}
wait_screen() {   # $1=正则 $2=超时秒
    local pat="$1" timeout="${2:-10}" i
    for i in $(seq 1 $((timeout * 2))); do
        screen_has "$pat" && return 0
        sleep 0.5
    done
    return 1
}

daemon_up() { "$TM_BIN" daemon status 2>/dev/null | grep -q 'pid'; }
wait_daemon() { local i; for i in $(seq 1 40); do daemon_up && return 0; sleep 0.25; done; return 1; }

cfg_get() { "$TM_BIN" config get "$1" 2>/dev/null | head -1; }
wait_cfg() {   # $1=key $2=期望值 $3=超时秒
    local key="$1" want="$2" timeout="${3:-10}" i got
    for i in $(seq 1 $((timeout * 2))); do
        got="$(cfg_get "$key")"
        [ "$got" = "$want" ] && return 0
        sleep 0.5
    done
    printf '      （期望 %s=%q，实际 %q）\n' "$key" "$want" "$got" >&2
    return 1
}
cfg_expect() {   # $1=key $2=期望值 $3=描述
    if wait_cfg "$1" "$2" 10; then ok "$3"; else bad "$3"; fi
}

send() { printf 'key:%s\n' "$1" >>"$CTL"; sleep "${2:-0.8}"; }

start_session() {   # $1=标签
    LABEL="$1"
    if ! start_core; then
        bad "核心未上线（$LABEL 会话）"
        return 1
    fi
    : > "$CTL"
    # 每个会话一份 pty 原始字节（会话之间不互相覆盖）：X1 的 rmcup 断言按
    # 会话取片段，--keep 时也能直接拿文件做证据。
    TUI_LOG="$WORK_DIR/tui-$LABEL.bin"
    : > "$TUI_LOG"
    LOG_FILE=""
    # 本轮会话的日志必须是"新出现的"那份：日志目录里还留着前几轮会话的
    # 文件，只按 ls -t 取最新会在新进程刚起、还没落盘时抓到上一轮的日志。
    local before_logs="$WORK_DIR/logs-before-$LABEL.txt"
    ls "$TER_MUSIC_LOG_DIR" 2>/dev/null > "$before_logs" || : > "$before_logs"
    TM_TUI_STDERR="$WORK_DIR/tui-stderr-$LABEL.log" \
        python3 "$DRIVER" drive "$TM_BIN" "$INIT_DIR" "$TUI_LOG" \
        --ctl "$CTL" --rc-file "$RC_FILE" --pid-file "$PID_FILE" \
        --cols 100 --rows 30 --max-runtime 300 -- --debug \
        >"$WORK_DIR/driver-$LABEL.log" 2>&1 &
    DRIVER_PID=$!
    local i f name
    for i in $(seq 1 80); do
        for f in "$TER_MUSIC_LOG_DIR"/*.log; do
            [ -e "$f" ] || continue
            name="$(basename "$f")"
            grep -qxF -- "$name" "$before_logs" && continue
            if grep -q '\[layout\] Selected layout\|\[file_picker\]' "$f" 2>/dev/null; then
                LOG_FILE="$f"
                break 2
            fi
        done
        sleep 0.2
    done
    if [ -z "$LOG_FILE" ]; then
        bad "前端没有写出调试日志（$LABEL 会话）"
        return 1
    fi
    if wait_screen '控 ?制 ?区|Controls' 25; then
        ok "TUI 就绪（$LABEL 会话）"
        return 0
    fi
    bad "TUI 未就绪（$LABEL 会话）"
    return 1
}

stop_session() {
    send '<F1>' 0.8
    printf 'key:q\n' >>"$CTL" 2>/dev/null || true
    sleep 1.2
    if [ -n "$DRIVER_PID" ]; then
        kill -9 "$DRIVER_PID" 2>/dev/null || true
        wait "$DRIVER_PID" 2>/dev/null || true
        DRIVER_PID=""
    fi
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    sleep 0.4
}

# 起核心：常规走 CLI detach；受限宿主机回退前台核心（与 karaoke E2E 同约定）。
start_core() {
    "$TM_BIN" daemon start >/dev/null 2>&1 || true
    if wait_daemon; then
        return 0
    fi
    "$TM_BIN" daemon foreground >"$WORK_DIR/core-foreground.log" 2>&1 &
    local i
    for i in $(seq 1 40); do daemon_up && return 0; sleep 0.25; done
    return 1
}

info "夹具：初始曲库=$INIT_DIR 打开目录=$OPEN_DIR 追加目录=$APPEND_DIR"
info "      带空格 UTF-8 目录=$WEIRD_DIR"
info "      语言包=$LANG_PACK"

# ════════════════════════════════════════════════════════════════════
# 会话 1：zenity 后端（四个入口 + 取消 + 错误）
# ════════════════════════════════════════════════════════════════════
info "会话 1：zenity（TERM_MUSIC_FILE_CHOOSER=zenity，DISPLAY=:0）"
export DISPLAY="${DISPLAY:-:0}"
unset WAYLAND_DISPLAY
export PATH="$FAKEBIN:$BASE_PATH"
export TERM_MUSIC_FILE_CHOOSER=zenity
export TM_FAKE_DIALOG_LOG="$FAKE_LOG" TM_FAKE_DIALOG_SPEC="$FAKE_SPEC"

if start_session zenity; then
    base_last="$(cfg_get last_opened_path)"
    info "会话 1 初始配置：default_startup_path='$(cfg_get default_startup_path)' last_opened_path='$base_last'"

    # Z5 取消：退出码 1，不写配置、不显示错误。
    mark="$(fake_mark)"
    c_before="$(flog_count 'cancelled via zenity')"
    fake_spec "" 1
    send o 1.6
    if fake_has "$mark" 'BEGIN prog=zenity' && fake_has "$mark" 'ARG1=--directory'; then
        ok "打开文件夹走 zenity --file-selection --directory（取消用例）"
    else
        bad "打开文件夹没有调用 zenity 目录模式"
    fi
    if [ "$(flog_count 'cancelled via zenity')" -gt "$c_before" ]; then
        ok "取消后前端日志记录 cancelled via zenity（不当作错误）"
    else
        bad "取消没有被识别（日志无 cancelled via zenity）"
    fi
    if [ "$(cfg_get last_opened_path)" = "$base_last" ]; then
        ok "取消不写 last_opened_path（配置保持原值）"
    else
        bad "取消改动了 last_opened_path（$(cfg_get last_opened_path)）"
    fi
    if ! screen_has '所选路径不是'; then
        ok "取消不显示错误提示"
    else
        bad "取消被错误地显示成路径错误"
    fi

    # Z6 错误：对话框回传不存在的目录 → 可读错误、配置不变、TUI 存活。
    mark="$(fake_mark)"
    f_before="$(flog_count 'folder rejected')"
    fake_spec "$ERROR_DIR" 0
    send o 1.2
    if [ "$(flog_count 'folder rejected')" -gt "$f_before" ]; then
        ok "不存在的目录被选择器拒绝（folder rejected）"
    else
        bad "不存在的目录没有被拒绝（日志无 folder rejected）"
    fi
    if wait_screen '所选路径不是已存在的目录' 4; then
        ok "状态行显示可读错误（所选路径不是已存在的目录）"
    else
        bad "错误不可见（状态行没有可读原因）"
    fi
    if [ "$(cfg_get last_opened_path)" = "$base_last" ]; then
        ok "错误不写 last_opened_path（不破坏已有配置）"
    else
        bad "错误改动了 last_opened_path（$(cfg_get last_opened_path)）"
    fi
    if wait_screen '控 ?制 ?区|Controls' 4; then
        ok "错误后 TUI 仍存活"
    else
        bad "错误后 TUI 不可见（可能崩溃/卡死）"
    fi

    # Z1 打开文件夹：选中 OPEN_DIR → 原有 playlist 加载路径。
    # X1（t8）：保留"对话框仍打开"的可控窗口，断言备用屏幕没有切换。
    dialog_with_hold "zenity 打开文件夹" "$OPEN_DIR" 0 o
    mark="$DLG_MARK"
    if fake_has "$mark" 'ARG1=--directory' &&
       fake_has "$mark" 'ARG2=--title=打开音乐文件夹'; then
        ok "打开文件夹：zenity 目录模式 + 本地化标题（打开音乐文件夹）"
    else
        bad "打开文件夹的 zenity argv 不符合预期"
    fi
    if wait_screen '目录加载成功' 3; then
        ok "状态行提示目录加载成功（playlist 原路径）"
    else
        bad "播放列表没有加载成功提示"
    fi
    cfg_expect last_opened_path "$OPEN_DIR" "选中目录写入 last_opened_path（走原有加载路径）"
    if fake_has "$mark" "ARG3=--filename=$INIT_DIR/"; then
        ok "初始目录取上一条内容路径（--filename=<上次目录>/）"
    else
        bad "zenity 初始目录参数不符合预期"
    fi
    if wait_screen '控 ?制 ?区|Controls' 3; then
        ok "zenity 对话框关闭后 TUI 画面可识别（主界面仍在）"
    else
        bad "zenity 对话框关闭后 TUI 画面不可识别（疑似白屏/退出备用屏幕）"
    fi

    # Z2 追加文件夹：选中 APPEND_DIR → append_playlist + "已追加"。
    mark="$(fake_mark)"
    fake_spec "$APPEND_DIR" 0
    send I 1.2
    if fake_has "$mark" 'ARG1=--directory'; then
        ok "追加文件夹同样使用 zenity 目录模式"
    else
        bad "追加文件夹没有使用目录模式"
    fi
    if wait_screen '已追加' 3; then
        ok "状态行提示已追加 N 首歌曲（append 语义未被改成打开）"
    else
        bad "追加没有成功提示"
    fi
    cfg_expect last_opened_path "$APPEND_DIR" "追加后 last_opened_path 指向追加目录"

    # Z3 语言包添加：文件选择，不允许 --directory，过滤器 *.tar.gz *.tgz。
    rm -rf "$USER_LANG_DIR"
    mark="$(fake_mark)"
    fake_spec "$LANG_PACK" 0
    send '<F7>' 1.0
    send a 1.2
    if fake_has "$mark" 'ARG0=--file-selection' &&
       ! fake_has "$mark" 'ARG1=--directory' &&
       fake_has "$mark" 'ARG1=--file-filter=语言包 | *.tar.gz *.tgz'; then
        ok "语言包添加走文件选择（无 --directory，过滤器 *.tar.gz *.tgz）"
    else
        bad "语言包添加的 zenity argv 不符合文件选择语义"
    fi
    if wait_screen '语言包已添加' 3; then
        ok "状态行提示语言包已添加"
    else
        bad "语言包添加没有成功提示"
    fi
    if [ -f "$USER_LANG_DIR/xx_AA.xml" ]; then
        ok "语言包按原有 i18n_add_language 路径安装（lang/xx_AA.xml）"
    else
        bad "语言包没有安装到用户语言目录"
    fi

    # Z4 默认音乐目录：F2 → 默认路径 → 内容区 → 回车 → 选择 WEIRD_DIR。
    mark="$(fake_mark)"
    fake_spec "$WEIRD_DIR" 0
    send '<F2>' 1.0
    send '<Down>' 0.5
    send '<Tab>' 0.5
    send '<Enter>' 1.2
    if fake_has "$mark" 'ARG1=--directory' &&
       fake_has "$mark" 'ARG2=--title=选择默认音乐目录'; then
        ok "默认音乐目录入口使用目录选择（本地化标题）"
    else
        bad "默认音乐目录入口的 zenity argv 不符合预期"
    fi
    if wait_screen '默认路径已保存' 3; then
        ok "状态行提示默认路径已保存"
    else
        bad "默认路径保存没有成功提示"
    fi
    cfg_expect default_startup_path "$WEIRD_DIR" \
        "默认音乐目录保存含空格/UTF-8 的路径（原样，不破坏配置）"

    # X2（t8）：整场会话的 pty 流里不得出现假对话框写到 stderr 的噪音。
    assert_no_stderr_noise "zenity 会话（四个入口）"

    stop_session
fi

# ════════════════════════════════════════════════════════════════════
# 会话 2：kdialog 后端（目录/文件两种模式 + 取消）
# ════════════════════════════════════════════════════════════════════
info "会话 2：kdialog（TERM_MUSIC_FILE_CHOOSER=kdialog）"
export DISPLAY="${DISPLAY:-:0}"
export PATH="$FAKEBIN:$BASE_PATH"
export TERM_MUSIC_FILE_CHOOSER=kdialog
export TM_FAKE_DIALOG_LOG="$FAKE_LOG" TM_FAKE_DIALOG_SPEC="$FAKE_SPEC"

if start_session kdialog; then
    before_last="$(cfg_get last_opened_path)"

    # K1a 目录模式：--getexistingdirectory，不传文件过滤器；初始目录沿用
    # 上一轮写下的 last_opened_path。X1（t8）：HOLD 窗口内断言备用屏幕未切换。
    dialog_with_hold "kdialog 打开文件夹" "$OPEN_DIR" 0 o
    mark="$DLG_MARK"
    if fake_has "$mark" 'BEGIN prog=kdialog' &&
       fake_has "$mark" 'ARG2=--getexistingdirectory'; then
        ok "kdialog 目录选择使用 --getexistingdirectory"
    else
        bad "kdialog 目录选择 argv 不符合预期"
    fi
    if [ "$(fake_count "$mark" '--getopenfilename')" = "0" ] &&
       [ "$(fake_count "$mark" '--file-filter')" = "0" ]; then
        ok "kdialog 目录模式不传文件打开参数/过滤器"
    else
        bad "kdialog 目录模式混入了文件模式参数"
    fi
    if wait_screen '目录加载成功' 3; then
        ok "kdialog 选中目录后走原有加载路径（目录加载成功）"
    else
        bad "kdialog 选中目录后没有加载成功提示"
    fi
    cfg_expect last_opened_path "$OPEN_DIR" "kdialog 选中的目录同样走 playlist 加载"
    if wait_screen '控 ?制 ?区|Controls' 3; then
        ok "kdialog 对话框关闭后 TUI 画面可识别（主界面仍在）"
    else
        bad "kdialog 对话框关闭后 TUI 画面不可识别（疑似白屏/退出备用屏幕）"
    fi

    # K1b 取消：kdialog exit 1 → 不写配置。
    mark="$(fake_mark)"
    c_before="$(flog_count 'cancelled via kdialog')"
    prev_last="$(cfg_get last_opened_path)"
    fake_spec "" 1
    send o 1.6
    if [ "$(flog_count 'cancelled via kdialog')" -gt "$c_before" ]; then
        ok "kdialog 取消被识别为 CANCEL（不当作错误）"
    else
        bad "kdialog 取消没有被识别（日志无 cancelled via kdialog）"
    fi
    if [ "$(cfg_get last_opened_path)" = "$prev_last" ]; then
        ok "kdialog 取消不写配置（last_opened_path 保持原值）"
    else
        bad "kdialog 取消改动了配置（$(cfg_get last_opened_path)）"
    fi

    # K1c 语言包：文件模式用 --getopenfilename。
    rm -rf "$USER_LANG_DIR"
    mark="$(fake_mark)"
    fake_spec "$LANG_PACK" 0
    send '<F7>' 1.0
    send a 1.2
    if fake_has "$mark" 'ARG2=--getopenfilename' &&
       [ "$(fake_count "$mark" '--getexistingdirectory')" = "0" ]; then
        ok "kdialog 语言包添加使用 --getopenfilename（不是目录模式）"
    else
        bad "kdialog 文件选择 argv 不符合预期"
    fi
    if wait_screen '语言包已添加' 3; then
        ok "kdialog 语言包添加成功提示"
    else
        bad "kdialog 语言包添加没有成功提示"
    fi
    if [ -f "$USER_LANG_DIR/xx_AA.xml" ]; then
        ok "kdialog 选中的语言包按原有校验/安装路径落盘"
    else
        bad "kdialog 语言包没有安装"
    fi

    # K1d 默认音乐目录：目录模式再次命中 --getexistingdirectory。
    mark="$(fake_mark)"
    fake_spec "$WEIRD_DIR" 0
    send '<F2>' 1.0
    send '<Down>' 0.5
    send '<Tab>' 0.5
    send '<Enter>' 1.2
    if fake_has "$mark" 'ARG2=--getexistingdirectory'; then
        ok "kdialog 默认音乐目录同样使用 --getexistingdirectory"
    else
        bad "kdialog 默认音乐目录 argv 不符合预期"
    fi
    cfg_expect default_startup_path "$WEIRD_DIR" "kdialog 保存默认音乐目录"

    # X2（t8）：kdialog 会话的 pty 流里同样不得出现子进程 stderr 噪音。
    assert_no_stderr_noise "kdialog 会话（四个入口）"

    stop_session
fi

# ════════════════════════════════════════════════════════════════════
# 会话 3：portal 偏好（TERM_MUSIC_FILE_CHOOSER=portal，由 zenity 承接）
# ════════════════════════════════════════════════════════════════════
info "会话 3：portal 偏好（TERM_MUSIC_FILE_CHOOSER=portal，DISPLAY=:0）"
export DISPLAY="${DISPLAY:-:0}"
unset WAYLAND_DISPLAY
export PATH="$FAKEBIN:$BASE_PATH"
export TERM_MUSIC_FILE_CHOOSER=portal
export TM_FAKE_DIALOG_LOG="$FAKE_LOG" TM_FAKE_DIALOG_SPEC="$FAKE_SPEC"

if start_session portal; then
    # X1（t8）：portal 偏好值由桌面对话框承接，打开期间同样不得退出备用屏幕。
    dialog_with_hold "portal 打开文件夹" "$APPEND_DIR" 0 o
    mark="$DLG_MARK"
    if fake_has "$mark" 'BEGIN prog=zenity' && fake_has "$mark" 'ARG1=--directory'; then
        ok "portal 偏好值由 zenity 承接目录选择（--directory）"
    else
        bad "portal 偏好值没有落到桌面对话框（argv 不符合预期）"
    fi
    if wait_screen '目录加载成功' 3; then
        ok "portal 选中目录后走原有加载路径（目录加载成功）"
    else
        bad "portal 目录加载没有成功提示"
    fi
    if wait_screen '控 ?制 ?区|Controls' 3; then
        ok "portal 对话框关闭后 TUI 画面可识别（主界面仍在）"
    else
        bad "portal 对话框关闭后 TUI 画面不可识别"
    fi
    cfg_expect last_opened_path "$APPEND_DIR" "portal 目录同样写入 last_opened_path"
    assert_no_stderr_noise "portal 会话"
    stop_session
fi

# ════════════════════════════════════════════════════════════════════
# 会话 4：TTY 回退（无桌面对话框 / 强制 tty）
# ════════════════════════════════════════════════════════════════════
info "会话 4：TTY 回退（PATH 无 zenity/kdialog、无 DISPLAY/WAYLAND 或强制 tty）"
TTY_PATH="$EMPTY_BIN:/usr/bin:/bin:/usr/sbin:/sbin"
if PATH="$TTY_PATH" command -v zenity >/dev/null 2>&1 ||
   PATH="$TTY_PATH" command -v kdialog >/dev/null 2>&1; then
    export TERM_MUSIC_FILE_CHOOSER=tty
    TTY_MODE_DESC="强制 tty（系统 PATH 里装有桌面对话框，无法用缺失验证自动回退）"
else
    unset TERM_MUSIC_FILE_CHOOSER
    TTY_MODE_DESC="auto→tty（PATH 无 zenity/kdialog 且无 DISPLAY/WAYLAND）"
fi
export PATH="$TTY_PATH"
unset DISPLAY WAYLAND_DISPLAY
export TM_FAKE_DIALOG_LOG="$FAKE_LOG" TM_FAKE_DIALOG_SPEC="$FAKE_SPEC"
tty_dialog_mark="$(fake_mark)"

if start_session tty; then
    ok "TTY 会话后端选择：$TTY_MODE_DESC"

    # T1a 手输 ~/ 展开 + 空格/UTF-8 原样：o → "~/午休 音乐" → Enter。
    p_before="$(flog_count 'via tty')"
    send o 1.0
    # 故意发送字面量 ~/ 前缀：验证 TUI 路径输入的 ~ 展开，不是 shell 展开。
    # shellcheck disable=SC2088
    send '~/午休 音乐' 0.3
    send '<Enter>' 2.0
    if flog_has 'backend=tty mode=folder'; then
        ok "前端日志确认目录选择使用 tty 后端（桌面对话框不可用时不报死）"
    else
        bad "目录选择没有落到 tty 后端"
    fi
    if wait_screen '目录加载成功' 3; then
        ok "TTY 选中目录后走原有加载路径（目录加载成功）"
    else
        bad "TTY 选中目录后没有加载成功提示"
    fi
    cfg_expect last_opened_path "$WEIRD_DIR" \
        "TTY 手输 ~/午休 音乐：~/ 展开且空格/UTF-8 原样（last_opened_path）"
    if [ "$(flog_count 'via tty')" -gt "$p_before" ] &&
       flog_has "picked '$WEIRD_DIR' via tty"; then
        ok "前端日志记录 picked '<展开后的绝对路径>' via tty"
    else
        bad "TTY 选择路径没有按预期展开"
    fi

    # T1b 空输入 = 取消：o → Enter。
    c_before="$(flog_count 'cancelled via tty')"
    send o 1.0
    send '<Enter>' 1.2
    if [ "$(flog_count 'cancelled via tty')" -gt "$c_before" ]; then
        ok "TTY 空输入按取消处理（不写半截路径、不报错）"
    else
        bad "TTY 空输入没有识别为取消"
    fi
    if [ "$(cfg_get last_opened_path)" = "$WEIRD_DIR" ]; then
        ok "TTY 取消不写 last_opened_path"
    else
        bad "TTY 取消改动了 last_opened_path（$(cfg_get last_opened_path)）"
    fi

    # T1c 设置页默认路径：TTY 手输绝对路径（含空格/UTF-8）。
    send '<F2>' 1.0
    send '<Down>' 0.5
    send '<Tab>' 0.5
    send '<Enter>' 1.0
    send "$TTY_DIR" 0.3
    send '<Enter>' 2.0
    if wait_screen '默认路径已保存' 3; then
        ok "TTY 默认路径保存后状态行提示"
    else
        bad "TTY 默认路径保存没有提示"
    fi
    cfg_expect default_startup_path "$TTY_DIR" "TTY 手输默认音乐目录（含空格/UTF-8）"

    # 全 TTY 会话里不应执行任何外置对话框（假 zenity/kdialog 不在 PATH）。
    if [ "$(fake_mark)" = "$tty_dialog_mark" ]; then
        ok "TTY 会话没有执行外置对话框（回退真实发生）"
    else
        bad "TTY 会话意外执行了外置对话框"
    fi

    send '<F1>' 0.8
    if wait_screen '控 ?制 ?区|Controls' 5; then
        ok "TTY 会话全程 TUI 存活"
    else
        bad "TTY 会话结束后 TUI 不可见"
    fi

    stop_session
fi

# ════════════════════════════════════════════════════════════════════
# 会话 5：AppImage 旧库污染 + GUI 失败回退 TTY（t18）
# ════════════════════════════════════════════════════════════════════
info "会话 5：AppImage 旧库污染 + GUI 失败回退 TTY（t18）"
export DISPLAY="${DISPLAY:-:0}"
unset WAYLAND_DISPLAY
export PATH="$FAKEBIN:$BASE_PATH"
export TERM_MUSIC_FILE_CHOOSER=zenity
export TM_FAKE_DIALOG_LOG="$FAKE_LOG" TM_FAKE_DIALOG_SPEC="$FAKE_SPEC"
# 模拟 AppImage AppRun：把捆绑库目录放在 LD_LIBRARY_PATH。修复前这个值会
# 原样传给宿主的 zenity/kdialog（t18 根因）；修复后子进程必须看不到它。
APPIMAGE_LIB="$WORK_DIR/appimage-bundled-lib"
mkdir -p "$APPIMAGE_LIB"
export LD_LIBRARY_PATH="$APPIMAGE_LIB"

if start_session appimage; then
    # L1a 正常对话框：子进程继承环境里不得有 AppImage 的 LD_LIBRARY_PATH。
    mark="$(fake_mark)"
    fake_spec "$OPEN_DIR" 0
    send o 1.6
    env_seen="$(fake_since "$mark" | grep '^ENV_LD_LIBRARY_PATH=' | head -1)"
    if [ "$env_seen" = "ENV_LD_LIBRARY_PATH=" ]; then
        ok "AppImage 环境：zenity 子进程看不到 LD_LIBRARY_PATH（旧库不泄漏）"
    else
        bad "AppImage 环境：zenity 子进程仍继承 LD_LIBRARY_PATH（$env_seen）"
    fi
    if wait_screen '目录加载成功' 3; then
        ok "清理环境后 zenity 仍能正常工作（目录加载成功）"
    else
        bad "清理环境后 zenity 选择失败（没有目录加载成功提示）"
    fi
    cfg_expect last_opened_path "$OPEN_DIR" "AppImage 环境正常选中目录写入配置"

    # L1b 对话框异常退出（127，模拟旧 libstdc++ 导致 GLIBCXX 崩溃）：
    # 必须回退 TTY 手输，提示可见，手输路径仍按原语义加载。
    mark="$(fake_mark)"
    fake_spec "" 127
    send o 1.2
    if wait_screen '图形对话框不可用' 6; then
        ok "对话框异常退出后回退 TTY（提示说明图形对话框不可用）"
    else
        bad "对话框异常退出后没有回退 TTY（无“图形对话框不可用”提示）"
    fi
    send "$TTY_DIR" 0.3
    send '<Enter>' 2.0
    if wait_screen '目录加载成功' 3; then
        ok "回退 TTY 手输路径成功（目录加载成功）"
    else
        bad "回退 TTY 手输路径没有成功加载"
    fi
    cfg_expect last_opened_path "$TTY_DIR" "TTY 回退选中目录写入 last_opened_path"
    if flog_has 'falling back to TTY'; then
        ok "前端日志记录 GUI 失败回退 TTY（可诊断）"
    else
        bad "前端日志没有记录回退 TTY"
    fi
    if [ "$(fake_mark)" -gt "$mark" ]; then
        ok "回退前确实尝试过外置对话框（不是跳过对话框直接 TTY）"
    else
        bad "没有尝试外置对话框就回退（无法证明失败回退语义）"
    fi

    # L1c 用户取消（exit 1）：不触发 TTY 回退、不改配置。
    prev_last="$(cfg_get last_opened_path)"
    c_before="$(flog_count 'cancelled via zenity')"
    fake_spec "" 1
    send o 1.6
    if [ "$(flog_count 'cancelled via zenity')" -gt "$c_before" ]; then
        ok "AppImage 环境：zenity 取消仍识别为 CANCEL"
    else
        bad "AppImage 环境：zenity 取消没有被识别"
    fi
    if [ "$(cfg_get last_opened_path)" = "$prev_last" ]; then
        ok "AppImage 环境：取消不写配置"
    else
        bad "AppImage 环境：取消改动了配置"
    fi
    if ! screen_has '图形对话框不可用' && ! screen_has '请直接输入目录路径'; then
        ok "用户取消不触发 TTY 回退（没有回退提示）"
    else
        bad "用户取消后仍出现 TTY 回退提示"
    fi

    stop_session
fi
unset LD_LIBRARY_PATH

printf '\n%d passed, %d skipped, %d failed\n' "$PASS" "$SKIPPED" "$FAIL"
[ "$FAIL" -eq 0 ] && exit 0
exit 1
