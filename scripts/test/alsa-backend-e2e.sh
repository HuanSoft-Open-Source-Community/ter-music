#!/usr/bin/env bash
#
# ALSA 后端端到端回归（真实 ALSA 设备）
#
# 覆盖：
#   - 配置 audio_backend=2 时，核心必须选中 ALSA 后端（日志出现
#     "ALSA backend selected"），且不得回退到 PipeWire/PulseAudio；
#   - play <wav> 后 show --json 的播放位置持续推进；
#   - 播放中 pause 不得触发"写入音频设备失败"（日志与 Control.StatusMessage
#     里都不能出现该文案或 audio.err.device_write 键），核心仍存活，
#     且暂停期间播放位置不推进；
#   - resume 后播放位置继续推进；
#   - 暂停状态下 next 能切到下一首并继续播放（不卡死、不报写入错误）；
#   - 收尾后无残留 ter-music 进程。
#
# 无可用 ALSA 设备（/dev/snd 缺失、aplay -l 列不出播放设备，或 aplay 未安装
# 时 /proc/asound/cards 为空）或没有 python3 时整体 SKIP（退出 0）。
#
# 配置夹具：version="9"，audio_backend=2，stop_core_on_exit=0，
#           独立 HOME/XDG_CONFIG_HOME（保留现有 XDG_RUNTIME_DIR 与会话总线）。
# 素材：python3 wave 生成 8 秒静音 WAV（S32LE、48kHz、2ch），不依赖 ffmpeg；
#       两首（01/02）以便 next 有明确的下一首可切。
#
# 用法：scripts/test/alsa-backend-e2e.sh [--bin <ter-music>] [--keep]
#   --bin <path>  指定 ter-music 可执行文件（默认 build/ter-music）
#   --keep        保留临时工作目录（失败时无论如何都会保留）
#   -h, --help    显示本帮助
#
# 环境：TM_TEST_BUS_READY=1 表示已由本脚本用 dbus-run-session 重入；
#       当前无会话总线时自动用 dbus-run-session 包裹重入。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""
DAEMON_PID=""
LOG_FILE=""

usage() {
    sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
}

# 原始参数：解析循环会 shift 掉 "$@"，dbus-run-session 重入时必须带着它
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

finish() {
    info "结果"
    printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
    if [ "$FAIL" -ne 0 ]; then
        KEEP=1
        printf '（工作目录保留在 %s；核心日志：%s）\n' \
            "$WORK_DIR" "${LOG_FILE:-$XDG_CONFIG_HOME/ter-music}" >&2
    fi
    [ "$FAIL" -eq 0 ]
}

# SKIP 是整体前置条件不满足：打印原因后以 0 退出。
skip_exit() {
    skip "$1"
    finish
    exit 0
}

cleanup() {
    if [ -n "$TM_BIN" ] && [ -x "$TM_BIN" ]; then
        "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    fi
    if [ -n "${MONITOR_PID:-}" ]; then
        kill "$MONITOR_PID" 2>/dev/null || true
    fi
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先用 --bin 指定）" >&2; exit 2; }

# ── 前置条件（不满足则整体 SKIP）──────────────────────────────────
command -v python3 >/dev/null 2>&1 \
    || skip_exit "没有 python3：无法生成 WAV 素材与解析 show --json"

# 可用 ALSA 设备判定：/dev/snd 必须存在；有 aplay 时以 aplay -l 列出的
# 播放设备为准；aplay 未安装时退回 /dev/snd + /proc/asound/cards。
has_alsa_device() {
    [ -d /dev/snd ] || return 1
    if command -v aplay >/dev/null 2>&1; then
        local cards
        cards="$(aplay -l 2>/dev/null)" || cards=""
        grep -qE '^card [0-9]+:' <<< "$cards" || return 1
        return 0
    fi
    [ -r /proc/asound/cards ] || return 1
    grep -qE '^[[:space:]]*[0-9]+ \[' /proc/asound/cards || return 1
    return 0
}
has_alsa_device \
    || skip_exit "没有可用的 ALSA 设备：/dev/snd 缺失，或声卡没有播放设备"

# ── 保持现有会话总线；没有时用 dbus-run-session 重入 ──────────────
# 不使用私有总线：ALSA 走真实声卡，核心仍需挂在当前会话总线上。
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：当前没有会话总线，且找不到 dbus-run-session。" >&2
        exit 2
    fi
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

# ── 夹具：独立 HOME/配置目录，保留 XDG_RUNTIME_DIR / 会话总线 ──────
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-alsa-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
MUSIC_DIR="$WORK_DIR/music"
mkdir -p "$XDG_CONFIG_HOME/ter-music" "$XDG_CACHE_HOME" "$MUSIC_DIR"

cat > "$XDG_CONFIG_HOME/ter-music/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <audio_backend>2</audio_backend>
    <stop_core_on_exit>0</stop_core_on_exit>
  </preferences>
</ter-music-config>
EOF

WAV_A="$MUSIC_DIR/01-alpha.wav"
WAV_B="$MUSIC_DIR/02-bravo.wav"
python3 - "$WAV_A" "$WAV_B" <<'PY'
import sys, wave

for path in sys.argv[1:]:
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(4)          # S32LE
        w.setframerate(48000)
        w.writeframes(b"\x00" * (48000 * 2 * 4 * 8))
PY
[ -s "$WAV_A" ] && [ -s "$WAV_B" ] \
    || { echo "错误：WAV 素材生成失败：$MUSIC_DIR" >&2; exit 2; }

# ── 观测助手 ──────────────────────────────────────────────────────
daemon_pid() {
    "$TM_BIN" daemon status --json 2>/dev/null | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    print("")
    raise SystemExit
inst = doc.get("instance") if isinstance(doc, dict) else None
pid = inst.get("pid") if isinstance(inst, dict) else None
if pid is None and isinstance(doc, dict):
    pid = doc.get("pid")
print(int(pid) if isinstance(pid, int) and pid > 0 else "")
' 2>/dev/null
}

daemon_alive() {
    local pid
    pid="$(daemon_pid)"
    [ -n "$pid" ] && [ -d "/proc/$pid" ]
}

wait_for_daemon() {
    local i
    for i in $(seq 1 50); do
        DAEMON_PID="$(daemon_pid)"
        if [ -n "$DAEMON_PID" ] && [ -d "/proc/$DAEMON_PID" ]; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

# playback.position_ms（毫秒）与当前曲目路径。
# show --json 的 playback 在顶层，但这里仍做一次递归兜底：位置字段
# 兼容 position_ms / 数字 position / "M:SS" 文本 position。
show_field() {
    "$TM_BIN" show --json 2>/dev/null | python3 -c '
import json, sys

def find(node, key):
    if isinstance(node, dict):
        if key in node:
            return node[key]
        for value in node.values():
            found = find(value, key)
            if found is not None:
                return found
    elif isinstance(node, list):
        for value in node:
            found = find(value, key)
            if found is not None:
                return found
    return None

def as_ms(pb):
    ms = pb.get("position_ms")
    if isinstance(ms, (int, float)):
        return int(ms)
    pos = pb.get("position")
    if isinstance(pos, (int, float)):
        return int(float(pos) * 1000)
    if isinstance(pos, str) and ":" in pos:
        try:
            total = 0
            for part in pos.split(":"):
                total = total * 60 + int(part)
            return total * 1000
        except ValueError:
            return -1
    return -1

def find_playback(node):
    if isinstance(node, dict):
        pb = node.get("playback")
        if isinstance(pb, dict) and ("position_ms" in pb or "position" in pb):
            return pb
        for value in node.values():
            found = find_playback(value)
            if found is not None:
                return found
    elif isinstance(node, list):
        for value in node:
            found = find_playback(value)
            if found is not None:
                return found
    return None

try:
    doc = json.load(sys.stdin)
except Exception:
    print(-1)
    raise SystemExit

key = sys.argv[1]
if key == "position_ms":
    pb = find_playback(doc)
    print(as_ms(pb) if pb is not None else -1)
else:
    value = find(doc, key)
    print(value if isinstance(value, str) else "")
' "$1" 2>/dev/null
}

show_position_ms() { show_field position_ms; }
show_track_path() { show_field path; }

# 核心最后一条状态消息（core_status_push 的落点）：写入设备失败就是
# core_status_push(i18n_get("audio.err.device_write")) 推上来的。
core_status_message() {
    command -v gdbus >/dev/null 2>&1 || return 1
    timeout 10 gdbus call --session \
        --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 \
        --method cn.org.huansoft.ter_music.Info.GetStatus 2>/dev/null \
    | python3 -c '
import json, re, sys
raw = sys.stdin.read()
m = re.search(r"(\{.*\})", raw)
if not m:
    print("")
    raise SystemExit
try:
    print(json.loads(m.group(1)).get("message") or "")
except Exception:
    print("")
' 2>/dev/null
}

latest_log() {
    ls -1t "$XDG_CONFIG_HOME/ter-music"/ter-music-debug-*.log 2>/dev/null | head -1
}

daemon_out_log() { printf '%s' "$XDG_CONFIG_HOME/ter-music/daemon.log"; }

file_size() {
    local f="$1"
    [ -f "$f" ] || { echo 0; return; }
    stat -c %s "$f" 2>/dev/null || echo 0
}

# 日志偏移标记：用于只检查某个阶段"新产生"的日志内容。
log_marker() {
    printf '%s:%s' "$(file_size "$(latest_log)")" "$(file_size "$(daemon_out_log)")"
}

# 打印标记之后新增的日志内容（核心调试日志 + 核心 stdout/stderr）。
new_log_text() {
    local marker="$1"
    local dbg_off="${marker%%:*}" dmn_off="${marker##*:}"
    local dbg_file
    dbg_file="$(latest_log)"
    [ -n "$dbg_file" ] && tail -c "+$((dbg_off + 1))" "$dbg_file" 2>/dev/null
    tail -c "+$((dmn_off + 1))" "$(daemon_out_log)" 2>/dev/null
    return 0
}

# 状态信号日志的偏移标记：与日志标记同理，只看某个阶段新增的信号。
signal_marker() {
    if [ -n "${SIGNAL_LOG:-}" ] && [ -f "$SIGNAL_LOG" ]; then
        file_size "$SIGNAL_LOG"
    else
        echo 0
    fi
}

new_signal_text() {
    local off="$1"
    if [ -n "${SIGNAL_LOG:-}" ] && [ -f "$SIGNAL_LOG" ]; then
        tail -c "+$((off + 1))" "$SIGNAL_LOG" 2>/dev/null
    fi
    return 0
}

# "写入音频设备失败"的三个形态：中文文案、英文文案、i18n 键。
WRITE_ERROR_RE='写入音频设备失败|Audio device write failed|audio\.err\.device_write'

diag() {
    printf '       诊断[%s]\n' "$1"
    printf '         daemon status: %s\n' "$("$TM_BIN" daemon status 2>&1 | head -1)"
    printf '         show --json: %s\n' "$("$TM_BIN" show --json 2>/dev/null | head -c 700)"
    if [ -n "$LOG_FILE" ] && [ -f "$LOG_FILE" ]; then
        printf '         日志尾部（%s）:\n' "$LOG_FILE"
        tail -n 15 "$LOG_FILE" | sed 's/^/           | /'
    fi
}

# ── 状态信号监听：core_status_push 走 D-Bus 状态消息，不进调试日志，
#    所以"写入音频设备失败"必须同时看信号流。gdbus 不可用时该检查降级为跳过。
start_signal_monitor() {
    command -v gdbus >/dev/null 2>&1 || return 1
    SIGNAL_LOG="$WORK_DIR/status-signals.log"
    gdbus monitor --session \
        --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 2>/dev/null \
        | grep --line-buffered -E 'StatusMessage|Error' > "$SIGNAL_LOG" &
    MONITOR_PID=$!
    sleep 0.5
    return 0
}

# ── 起核心 ────────────────────────────────────────────────────────
info "启动核心（audio_backend=2，音乐目录 $MUSIC_DIR）"
if start_signal_monitor; then
    ok "已挂上 D-Bus 状态信号监听（StatusMessage/Error）"
else
    skip "没有 gdbus：跳过状态消息通道的写入错误检查"
fi

TER_MUSIC_FORCE_LOG=1 "$TM_BIN" daemon start --open "$MUSIC_DIR" \
    > "$WORK_DIR/daemon-start.log" 2>&1
START_RC=$?
if [ "$START_RC" -ne 0 ]; then
    bad "daemon start --open 失败（退出码 $START_RC）"
    printf '         启动输出:\n' >&2
    sed 's/^/           | /' "$WORK_DIR/daemon-start.log" >&2 || true
    finish
    exit 1
fi
if ! wait_for_daemon; then
    bad "daemon status 未出现有效 pid"
    diag "启动"
    finish
    exit 1
fi
ok "核心已启动（pid $DAEMON_PID）"

LOG_FILE="$(latest_log)"
if [ -z "$LOG_FILE" ] || [ ! -f "$LOG_FILE" ]; then
    bad "找不到 $XDG_CONFIG_HOME/ter-music/ter-music-debug-*.log"
    LOG_FILE=""
else
    if grep -q 'ALSA backend selected' "$LOG_FILE"; then
        ok "日志确认 ALSA 后端被选中（$(basename "$LOG_FILE")）"
    else
        bad "日志中没有 ALSA backend selected 记录"
        grep -E 'backend|PipeWire|PulseAudio' "$LOG_FILE" \
            | sed 's/^/           | /' | tail -n 10 || true
    fi
    if grep -Eq 'PipeWire backend selected|PulseAudio backend selected|selected but unavailable|Audio backend switched|looking for a fallback|No usable audio backend' "$LOG_FILE"; then
        bad "日志出现 PipeWire/PulseAudio 选中或回退记录"
        grep -E 'PipeWire backend selected|PulseAudio backend selected|selected but unavailable|Audio backend switched|looking for a fallback|No usable audio backend' \
            "$LOG_FILE" | sed 's/^/           | /' || true
    else
        ok "日志中没有回退到 PipeWire/PulseAudio 的记录"
    fi
fi

# ── 播放与进度推进 ────────────────────────────────────────────────
info "播放 8 秒静音 WAV，轮询 show --json 的播放位置"
PLAY_MARKER="$(log_marker)"
"$TM_BIN" play "$WAV_A" > "$WORK_DIR/play.log" 2>&1
PLAY_RC=$?
if [ "$PLAY_RC" -ne 0 ]; then
    bad "play <wav> 失败（退出码 $PLAY_RC）"
    diag "play"
    finish
    exit 1
fi
ok "play <wav> 返回成功"

POS_AT_PLAY="$(show_position_ms)"
case "$POS_AT_PLAY" in
    ''|*[!0-9-]*) POS_AT_PLAY=-1 ;;
esac
MAX_POS=-1
for _ in $(seq 1 25); do     # 最多约 5 秒
    POS="$(show_position_ms)"
    case "$POS" in
        ''|*[!0-9-]*) POS=-1 ;;
    esac
    if [ "$POS" -ge 0 ]; then
        [ "$POS" -gt "$MAX_POS" ] && MAX_POS="$POS"
        # 断言只要求推进到 1s：达标即停，免得把 8 秒素材播到 EOS
        # （EOS 会自动切下一首并把位置清零，干扰后面的暂停断言）。
        [ "$MAX_POS" -ge 1000 ] && break
    fi
    sleep 0.2
done
if [ "$MAX_POS" -ge 1000 ]; then
    ok "播放位置推进：play 后首个采样 ${POS_AT_PLAY}ms → 最大 ${MAX_POS}ms（>=1s）"
else
    bad "播放位置未推进到 1 秒（首个采样 ${POS_AT_PLAY}ms，最大 ${MAX_POS}ms）"
    diag "进度"
fi

# ── 暂停 ──────────────────────────────────────────────────────────
# 暂停断言要求"位置真的在走、然后被冻住"，所以先落到一个确定状态：
# 当前曲目是 01-alpha，位置在 1s~5s 之间。8 秒素材在本测试窗口内可能
# 自然播完（EOS 自动切下一首、位置清零），那样"暂停期间位置不推进"
# 就成了 0→0 的空断言。
rearm_known_state() {
    "$TM_BIN" stop >/dev/null 2>&1 || true
    sleep 0.3
    "$TM_BIN" play "$WAV_A" >/dev/null 2>&1 || true
    local i p
    for i in $(seq 1 15); do
        p="$(show_position_ms)"
        case "$p" in
            ''|*[!0-9-]*) p=-1 ;;
        esac
        if [ "$p" -ge 1000 ] && [ "$p" -le 5000 ]; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

CUR_PATH="$(show_track_path)"
CUR_POS="$(show_position_ms)"
case "$CUR_POS" in
    ''|*[!0-9-]*) CUR_POS=-1 ;;
esac
if [ "${CUR_PATH##*/}" != "01-alpha.wav" ] || [ "$CUR_POS" -lt 1000 ] || [ "$CUR_POS" -gt 5000 ]; then
    if rearm_known_state; then
        ok "已把播放状态归位到 01-alpha 的 1s~5s 区间（实测 ${CUR_POS}ms）"
    else
        bad "无法把播放状态归位到 01-alpha 的 1s~5s 区间（实测 ${CUR_POS}ms）"
        diag "归位"
    fi
fi

info "播放中 pause：检查写入错误、核心存活与位置冻结"
# "pause 触发设备写入失败"是竞态：只有 pause 恰好落在一次设备写入的间隙时
# 才会复现（实测同一份构建约三次里命中一次）。因此同一组断言重复 3 轮：
# 每轮都完整做 pause → 采样 → resume，任一轮命中即判定失败。
PAUSE_ROUNDS=3
PAUSE_ERROR_SEEN=0
POS_PAUSE_2=-1

for round in $(seq 1 "$PAUSE_ROUNDS"); do
    ROUND_MARKER="$(log_marker)"
    "$TM_BIN" pause > "$WORK_DIR/pause-$round.log" 2>&1
    PAUSE_RC=$?
    if [ "$PAUSE_RC" -ne 0 ]; then
        bad "第 $round 轮 pause 失败（退出码 $PAUSE_RC）"
        sed 's/^/           | /' "$WORK_DIR/pause-$round.log" >&2 || true
        PAUSE_ERROR_SEEN=1
        break
    fi

    sleep 0.5
    POS_PAUSE_1="$(show_position_ms)"
    case "$POS_PAUSE_1" in
        ''|*[!0-9-]*) POS_PAUSE_1=-1 ;;
    esac
    sleep 1.0
    POS_PAUSE_2="$(show_position_ms)"
    case "$POS_PAUSE_2" in
        ''|*[!0-9-]*) POS_PAUSE_2=-1 ;;
    esac

    if ! daemon_alive; then
        bad "第 $round 轮暂停后核心已退出"
        diag "pause"
        PAUSE_ERROR_SEEN=1
        break
    fi

    if [ "$POS_PAUSE_1" -ge 0 ] && [ "$POS_PAUSE_2" -ge 0 ] \
        && [ "$((POS_PAUSE_2 - POS_PAUSE_1))" -eq 0 ] && [ "$POS_PAUSE_1" -gt 0 ]; then
        ok "第 $round 轮：暂停期间播放位置不推进（两次采样均为 ${POS_PAUSE_1}ms）"
    elif [ "$POS_PAUSE_1" -ge 0 ] && [ "$POS_PAUSE_2" -ge 0 ] \
        && [ "$((POS_PAUSE_2 - POS_PAUSE_1))" -eq 0 ]; then
        bad "第 $round 轮：暂停期间播放位置为 0（播放已被中止，不是真暂停）"
        diag "pause"
        PAUSE_ERROR_SEEN=1
        break
    else
        bad "第 $round 轮：暂停期间播放位置仍在推进（${POS_PAUSE_1}ms → ${POS_PAUSE_2}ms）"
        diag "pause"
        PAUSE_ERROR_SEEN=1
        break
    fi

    # 写入错误的三条通道：本轮新增日志、状态信号、核心最后一条状态消息。
    ROUND_LOG="$(new_log_text "$ROUND_MARKER")"
    if printf '%s' "$ROUND_LOG" | grep -Eq "$WRITE_ERROR_RE"; then
        bad "第 $round 轮暂停期间出现\"写入音频设备失败\"（日志）"
        printf '%s' "$ROUND_LOG" | grep -En "$WRITE_ERROR_RE" | sed 's/^/           | /' || true
        PAUSE_ERROR_SEEN=1
        break
    fi
    if [ -n "${SIGNAL_LOG:-}" ] && [ -f "$SIGNAL_LOG" ] \
        && grep -Eq "$WRITE_ERROR_RE" "$SIGNAL_LOG"; then
        bad "第 $round 轮暂停期间出现\"写入音频设备失败\"（状态消息）"
        grep -En "$WRITE_ERROR_RE" "$SIGNAL_LOG" | sed 's/^/           | /' || true
        PAUSE_ERROR_SEEN=1
        break
    fi
    ROUND_STATUS="$(core_status_message)"
    if [ -n "$ROUND_STATUS" ] && printf '%s' "$ROUND_STATUS" | grep -Eq "$WRITE_ERROR_RE"; then
        bad "第 $round 轮暂停后核心状态消息是\"写入音频设备失败\""
        printf '         状态消息：%s\n' "$ROUND_STATUS" >&2
        PAUSE_ERROR_SEEN=1
        break
    fi
    ok "第 $round 轮：暂停期间没有\"写入音频设备失败\"（日志与状态消息均干净）"

    # ── 恢复：位置必须继续推进 ──────────────────────────────────
    "$TM_BIN" resume > "$WORK_DIR/resume-$round.log" 2>&1
    RESUME_RC=$?
    if [ "$RESUME_RC" -ne 0 ]; then
        bad "第 $round 轮 resume 失败（退出码 $RESUME_RC）"
        sed 's/^/           | /' "$WORK_DIR/resume-$round.log" >&2 || true
        PAUSE_ERROR_SEEN=1
        break
    fi
    RESUME_MAX=-1
    for _ in $(seq 1 20); do     # 最多约 4 秒
        POS="$(show_position_ms)"
        case "$POS" in
            ''|*[!0-9-]*) POS=-1 ;;
        esac
        if [ "$POS" -ge 0 ] && [ "$POS" -gt "$RESUME_MAX" ]; then
            RESUME_MAX="$POS"
        fi
        if [ "$RESUME_MAX" -gt "$POS_PAUSE_2" ] && [ "$POS_PAUSE_2" -ge 0 ]; then
            break
        fi
        sleep 0.2
    done
    if [ "$POS_PAUSE_2" -ge 0 ] && [ "$RESUME_MAX" -gt "$POS_PAUSE_2" ]; then
        ok "第 $round 轮：恢复后播放位置继续推进（${POS_PAUSE_2}ms → ${RESUME_MAX}ms）"
    else
        bad "第 $round 轮：恢复后播放位置没有继续推进（暂停于 ${POS_PAUSE_2}ms，最大 ${RESUME_MAX}ms）"
        diag "resume"
        PAUSE_ERROR_SEEN=1
        break
    fi
done

if [ "$PAUSE_ERROR_SEEN" -eq 0 ] && [ "$PAUSE_ROUNDS" -gt 1 ]; then
    ok "连续 $PAUSE_ROUNDS 轮 pause/resume 均未触发设备写入错误"
fi

# ── 暂停状态下切歌 ────────────────────────────────────────────────
info "暂停状态下 next：切到下一首并继续播放"
NEXT_MARKER="$(log_marker)"
PATH_BEFORE="$(show_track_path)"
POS_BEFORE="$(show_position_ms)"
case "$POS_BEFORE" in
    ''|*[!0-9-]*) POS_BEFORE=-1 ;;
esac

"$TM_BIN" pause >/dev/null 2>&1 || true
sleep 0.4
"$TM_BIN" next > "$WORK_DIR/next.log" 2>&1
NEXT_RC=$?
if [ "$NEXT_RC" -ne 0 ]; then
    bad "next 失败（退出码 $NEXT_RC）"
    sed 's/^/           | /' "$WORK_DIR/next.log" >&2 || true
else
    ok "next 返回成功"
fi

NEXT_MAX=-1
NEXT_PATH=""
for _ in $(seq 1 25); do     # 最多约 5 秒
    POS="$(show_position_ms)"
    case "$POS" in
        ''|*[!0-9-]*) POS=-1 ;;
    esac
    if [ "$POS" -ge 0 ] && [ "$POS" -gt "$NEXT_MAX" ]; then
        NEXT_MAX="$POS"
    fi
    [ -z "$NEXT_PATH" ] && NEXT_PATH="$(show_track_path)"
    if [ "$NEXT_MAX" -ge 1000 ] && [ -n "$NEXT_PATH" ]; then
        break
    fi
    sleep 0.2
done

if [ -n "$NEXT_PATH" ] && [ "${NEXT_PATH##*/}" != "${PATH_BEFORE##*/}" ]; then
    ok "next 切到了下一首：${PATH_BEFORE##*/} → ${NEXT_PATH##*/}"
else
    bad "next 没有切到下一首（切歌前 ${PATH_BEFORE##*/}，切歌后 ${NEXT_PATH##*/}）"
    diag "next"
fi

if [ "$NEXT_MAX" -ge 1000 ]; then
    ok "新曲目播放位置推进（切歌前 ${POS_BEFORE}ms → 新曲目最大 ${NEXT_MAX}ms）"
else
    bad "新曲目播放位置没有推进（切歌前 ${POS_BEFORE}ms，新曲目最大 ${NEXT_MAX}ms）"
    diag "next"
fi

if daemon_alive; then
    ok "切歌后核心仍存活（pid $DAEMON_PID）"
else
    bad "切歌后核心已退出"
    diag "next"
fi

NEXT_LOG="$(new_log_text "$NEXT_MARKER")"
if printf '%s' "$NEXT_LOG" | grep -Eq "$WRITE_ERROR_RE"; then
    bad "切歌期间出现\"写入音频设备失败\"（日志）"
    printf '%s' "$NEXT_LOG" | grep -En "$WRITE_ERROR_RE" | sed 's/^/           | /' || true
else
    ok "切歌期间日志没有\"写入音频设备失败\""
fi
if [ -n "${SIGNAL_LOG:-}" ] && [ -f "$SIGNAL_LOG" ]; then
    if grep -Eq "$WRITE_ERROR_RE" "$SIGNAL_LOG"; then
        bad "切歌期间出现\"写入音频设备失败\"（状态消息）"
        grep -En "$WRITE_ERROR_RE" "$SIGNAL_LOG" | sed 's/^/           | /' || true
    else
        ok "切歌期间状态消息没有\"写入音频设备失败\""
    fi
fi
NEXT_STATUS="$(core_status_message)"
if [ -n "$NEXT_STATUS" ]; then
    if printf '%s' "$NEXT_STATUS" | grep -Eq "$WRITE_ERROR_RE"; then
        bad "切歌后核心状态消息是\"写入音频设备失败\""
        printf '         状态消息：%s\n' "$NEXT_STATUS" >&2
    else
        ok "切歌后核心状态消息正常（$NEXT_STATUS）"
    fi
fi

# ── 收尾：停止播放与核心，确认无残留进程 ─────────────────────────
info "停止播放并关闭核心"
"$TM_BIN" stop >/dev/null 2>&1 || true
"$TM_BIN" daemon stop >/dev/null 2>&1 || true

for _ in $(seq 1 30); do     # 最多约 6 秒等核心退出
    [ -z "$(daemon_pid)" ] && break
    sleep 0.2
done

# 残留判定只看"本脚本拉起"的实例：可执行文件相同，且 environ 带着本脚本
# 独有的 XDG_CONFIG_HOME 标记。别处（如桌面端）的实例不算残留。
count_own_residual() {
    local target marker pid exe n=0
    target="$(readlink -f "$TM_BIN" 2>/dev/null)" || target="$TM_BIN"
    marker="$XDG_CONFIG_HOME"
    for pid_dir in /proc/[0-9]*; do
        pid="${pid_dir#/proc/}"
        exe="$(readlink -f "$pid_dir/exe" 2>/dev/null)" || continue
        [ "$exe" = "$target" ] || continue
        if tr '\0' '\n' < "$pid_dir/environ" 2>/dev/null \
             | grep -qxF "XDG_CONFIG_HOME=$marker"; then
            n=$((n + 1))
        fi
    done
    printf '%s\n' "$n"
}

RESIDUAL="$(count_own_residual)"
if [ "${RESIDUAL:-0}" -eq 0 ]; then
    ok "收尾后无残留 ter-music 进程"
else
    bad "收尾后仍有 $RESIDUAL 个本脚本拉起的 ter-music 进程"
    for pid_dir in /proc/[0-9]*; do
        pid="${pid_dir#/proc/}"
        exe="$(readlink -f "$pid_dir/exe" 2>/dev/null)" || continue
        [ "$exe" = "$(readlink -f "$TM_BIN" 2>/dev/null)" ] || continue
        tr '\0' '\n' < "$pid_dir/environ" 2>/dev/null \
            | grep -qxF "XDG_CONFIG_HOME=$XDG_CONFIG_HOME" \
            && printf '           | pid %s\n' "$pid"
    done
fi

finish
