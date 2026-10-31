#!/usr/bin/env bash
#
# PipeWire 后端端到端回归（真实 PipeWire 服务）
#
# 覆盖：
#   - 配置 audio_backend=3 时，核心必须选中 PipeWire 后端（日志出现
#     "PipeWire backend selected"），且不得回退到 PulseAudio/ALSA；
#   - play <wav> 后 show --json 的播放位置持续推进；
#   - 播放中 volume 命令成功且核心仍存活；
#   - 连续 20 轮 play/stop 后核心稳定、/proc/<pid>/fd 不持续增长；
#   - AUTO（audio_backend=0）在 PipeWire 可用时仍选择 PipeWire 且不回退；
#   - 收尾后 pw-cli ls Node 不残留 ter-music 节点。
#
# 无 PipeWire 服务、无 Audio/Sink 输出节点或无 python3 时整体 SKIP（退出 0）。
#
# 配置夹具：version="9"，audio_backend=3，stop_core_on_exit=0，
#           独立 HOME/XDG_CONFIG_HOME（保留现有 XDG_RUNTIME_DIR 与会话总线）。
# 素材：python3 wave 生成 30 秒静音 WAV（S32LE、48kHz、2ch），不依赖 ffmpeg。
#
# 用法：scripts/test/pipewire-backend-e2e.sh [--bin <ter-music>] [--keep]
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
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先用 --bin 指定）" >&2; exit 2; }

# ── 前置条件（不满足则整体 SKIP）──────────────────────────────────
command -v python3 >/dev/null 2>&1 \
    || skip_exit "没有 python3：无法生成 WAV 素材与解析 show --json"
command -v pw-cli >/dev/null 2>&1 \
    || skip_exit "没有 pw-cli：未安装 PipeWire 客户端工具"
timeout 3 pw-cli info 0 >/dev/null 2>&1 \
    || skip_exit "pw-cli info 0 失败：没有可用的 PipeWire 服务"
command -v pw-dump >/dev/null 2>&1 \
    || skip_exit "没有 pw-dump：无法检查 PipeWire 输出节点"
PW_DUMP="$(timeout 5 pw-dump 2>/dev/null)" \
    || skip_exit "pw-dump 执行失败：无法检查 PipeWire 输出节点"
# 用 here-string 而非管道：pipefail 下 grep -q 提前退出会让上游收 SIGPIPE(141)，
# 把“命中”误判成失败。
grep -q '"media.class": *"Audio/Sink"' <<< "$PW_DUMP" \
    || skip_exit "PipeWire 中没有 Audio/Sink 输出节点（无声卡）"
unset PW_DUMP

# ── 保持现有会话总线；没有时用 dbus-run-session 重入 ──────────────
# 不使用私有总线：PipeWire 客户端需要连到当前会话的 PipeWire/会话总线。
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：当前没有会话总线，且找不到 dbus-run-session。" >&2
        exit 2
    fi
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

# ── 夹具：独立 HOME/配置目录，保留 XDG_RUNTIME_DIR / 会话总线 ──────
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-pipewire-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
MUSIC_DIR="$WORK_DIR/music"
mkdir -p "$XDG_CONFIG_HOME/ter-music" "$XDG_CACHE_HOME" "$MUSIC_DIR"

cat > "$XDG_CONFIG_HOME/ter-music/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <audio_backend>3</audio_backend>
    <stop_core_on_exit>0</stop_core_on_exit>
  </preferences>
</ter-music-config>
EOF

WAV="$MUSIC_DIR/pipewire-tone.wav"
python3 - "$WAV" <<'PY'
import sys, wave

path = sys.argv[1]
# 30 秒：下面的 PipeWire 侧断言要在"确实还在播"的窗口里做（节点一旦随曲目结束被销毁，
# 断言就会踩空），8 秒不够用。
rate, channels, seconds = 48000, 2, 30
with wave.open(path, "wb") as w:
    w.setnchannels(channels)
    w.setsampwidth(4)          # S32LE
    w.setframerate(rate)
    w.writeframes(b"\x00" * (rate * channels * 4 * seconds))
PY
[ -s "$WAV" ] || { echo "错误：WAV 素材生成失败：$WAV" >&2; exit 2; }

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

# playback.position_ms（毫秒）；结构缺失/解析失败回 -1。
# 兼容字段可能嵌套：优先 playback.position_ms，再退到数字 position /
# "M:SS" 文本 position（info.c 的 JSON 同时给出 position_ms 与文本 position）。
show_position_ms() {
    "$TM_BIN" show --json 2>/dev/null | python3 -c '
import json, sys

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

try:
    doc = json.load(sys.stdin)
except Exception:
    print(-1)
    raise SystemExit
pb = find_playback(doc)
print(as_ms(pb) if pb is not None else -1)
' 2>/dev/null
}

fd_count() {
    local pid="$1"
    [ -n "$pid" ] && [ -d "/proc/$pid/fd" ] || { echo 0; return; }
    ls "/proc/$pid/fd" 2>/dev/null | wc -l | tr -d ' '
}

latest_log() {
    local newest
    newest="$(ls -1t "$XDG_CONFIG_HOME/ter-music"/ter-music-debug-*.log 2>/dev/null | head -1)"
    printf '%s' "$newest"
}

diag() {
    printf '       诊断[%s]\n' "$1"
    printf '         daemon status: %s\n' "$("$TM_BIN" daemon status 2>&1 | head -1)"
    printf '         show --json: %s\n' "$("$TM_BIN" show --json 2>/dev/null | head -c 700)"
    if [ -n "$LOG_FILE" ] && [ -f "$LOG_FILE" ]; then
        printf '         日志尾部（%s）:\n' "$LOG_FILE"
        tail -n 15 "$LOG_FILE" | sed 's/^/           | /'
    fi
}

# PipeWire 侧观测（与 pipewire-playback-census-e2e.sh 同一口径）：
# 只看 type=Node 且 props.node.name=="ter-music" 且 media.category=="Playback" 的节点。
#   pw_node_count            : 这类节点的总数
#   pw_node_field <字段>      : 第一个这类节点的 info.<字段>（state / volume）
#     volume 取 info.params.Props[0].volume（SPA_PARAM_Props 里的流音量），
#     它是 pw_stream_set_control(SPA_PROP_volume) 真正落到 PipeWire 后的唯一机检口径。
pw_nodes_json() {
    timeout 5 pw-dump 2>/dev/null
}

pw_node_count() {
    pw_nodes_json | python3 -c '
import json, sys
try:
    doc = json.loads(sys.stdin.read() or "[]")
except Exception:
    print(0); raise SystemExit
n = 0
for o in doc:
    if o.get("type") != "PipeWire:Interface:Node":
        continue
    props = ((o.get("info") or {}).get("props") or {})
    if props.get("node.name") == "ter-music" and props.get("media.category") == "Playback":
        n += 1
print(n)' 2>/dev/null
}

pw_node_state() {
    pw_nodes_json | python3 -c '
import json, sys
try:
    doc = json.loads(sys.stdin.read() or "[]")
except Exception:
    print("unknown"); raise SystemExit
for o in doc:
    if o.get("type") != "PipeWire:Interface:Node":
        continue
    info = o.get("info") or {}
    props = info.get("props") or {}
    if props.get("node.name") == "ter-music" and props.get("media.category") == "Playback":
        print(info.get("state") or "unknown")
        break
else:
    print("none")' 2>/dev/null
}

pw_node_volume() {
    pw_nodes_json | python3 -c '
import json, sys
try:
    doc = json.loads(sys.stdin.read() or "[]")
except Exception:
    print(-1); raise SystemExit
for o in doc:
    if o.get("type") != "PipeWire:Interface:Node":
        continue
    info = o.get("info") or {}
    props = info.get("props") or {}
    if props.get("node.name") != "ter-music" or props.get("media.category") != "Playback":
        continue
    prm = info.get("params") or {}
    p = prm.get("Props")
    if isinstance(p, list) and p:
        p = p[0]
    if isinstance(p, dict) and isinstance(p.get("volume"), (int, float)):
        print("%.4f" % p["volume"])
    else:
        print(-1)
    break
else:
    print(-1)' 2>/dev/null
}

core_pids() {
    pgrep -f 'daemon[ ]foreground' 2>/dev/null | tr '\n' ' '
}

core_count() {
    local n=0 pid
    for pid in $(core_pids); do
        n=$((n + 1))
    done
    printf '%s' "$n"
}

# ── 起核心 ────────────────────────────────────────────────────────
info "启动核心（audio_backend=3，音乐目录 $MUSIC_DIR）"
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

# ── 播放与进度推进 ────────────────────────────────────────────────
info "播放 30 秒静音 WAV，轮询 show --json 的播放位置"
"$TM_BIN" play "$WAV" > "$WORK_DIR/play.log" 2>&1
PLAY_RC=$?
if [ "$PLAY_RC" -ne 0 ]; then
    bad "play <wav> 失败（退出码 $PLAY_RC）"
    diag "play"
    finish
    exit 1
fi
ok "play <wav> 返回成功"

FIRST_POS=""
MAX_POS=-1
for _ in $(seq 1 25); do     # 最多约 5 秒
    POS="$(show_position_ms)"
    case "$POS" in
        ''|*[!0-9-]*) POS=-1 ;;
    esac
    if [ "$POS" -ge 0 ]; then
        [ -z "$FIRST_POS" ] && FIRST_POS="$POS"
        [ "$POS" -gt "$MAX_POS" ] && MAX_POS="$POS"
    fi
    sleep 0.2
done
if [ "$MAX_POS" -ge 1000 ]; then
    ok "播放位置推进：首个采样 ${FIRST_POS:-?}ms → 最大 ${MAX_POS}ms（>=1s）"
else
    bad "播放位置未推进到 1 秒（首个采样 ${FIRST_POS:-无}ms，最大 ${MAX_POS}ms）"
    diag "进度"
fi
# PipeWire 的 1MB 环可先装约 3 秒音频。进度若按“已写入”统计，play 刚返回
# 时首个采样就会是 ~3000ms；按“可听位置”统计则应接近 0。
if [ -n "$FIRST_POS" ] && [ "$FIRST_POS" -le 1500 ]; then
    ok "首个进度采样未超前（${FIRST_POS}ms，已扣除 PipeWire 未消费缓冲）"
else
    bad "首个进度采样已超前（${FIRST_POS:-无}ms，>1500ms）：进度统计的是写入量而非可听位置，断点会多走一段"
    diag "进度超前"
fi

# ── 音量命令 ──────────────────────────────────────────────────────
info "播放中设置音量 50%"
"$TM_BIN" volume 50 > "$WORK_DIR/volume.log" 2>&1
VOL_RC=$?
if [ "$VOL_RC" -eq 0 ]; then
    ok "volume 50 返回成功"
else
    bad "volume 50 失败（退出码 $VOL_RC）"
    sed 's/^/           | /' "$WORK_DIR/volume.log" >&2 || true
fi
if daemon_alive; then
    ok "设置音量后核心仍存活（pid $DAEMON_PID）"
else
    bad "设置音量后核心已退出"
    diag "volume"
fi

# ── PipeWire 侧硬断言：调用上下文与"至多一路"不变量 ────────────────
# 只认 Node 对象（与 pipewire-playback-census-e2e.sh 同口径）。
# 判别性交代（勿夸大）：实测 pw_stream_set_control/set_active 即便未持循环锁，
# 控制值仍能落到流上（HEAD 上 volume 20 → Props.volume 0.2、pause → state idle 同样成立），
# 所以"音量落流/暂停落流"两条只是**行为守卫**；真正判别"必须持循环锁"这条契约的
# 是下面的 wrong context 计数（HEAD=11 次 → 修复后 0 次）。
info "PipeWire 侧断言：流数量 / 音量落流 / 调用上下文"
# 前提：确实还在播。曲目一旦自然播完，播放线程退出并把流销毁，节点就消失了，
# 那时断言会踩空——所以素材用 30 秒，且这里先重新起播并等节点进 running。
"$TM_BIN" play "$WAV" > "$WORK_DIR/pw-assert-play.log" 2>&1
PW_STATE=""
for _ in $(seq 1 40); do
    PW_STATE="$(pw_node_state)"
    [ "$PW_STATE" = "running" ] && break
    sleep 0.2
done
PW_NODES="$(pw_node_count)"
if [ "$PW_NODES" = "1" ]; then
    ok "播放中 ter-music 播放流节点恰好 1 路"
else
    bad "播放中 ter-music 播放流节点数为 $PW_NODES（期望 1；节点 state=${PW_STATE:-?}）"
fi
# 音量必须用一个**与前面 50% 不同**的值：PipeWire 对新节点本来就可能自带 0.5 的默认音量，
# 断言 50%→0.5 是空洞的；40%→0.4 只能由 pw_stream_set_control 真正落流产生。
"$TM_BIN" volume 40 > "$WORK_DIR/pw-assert-volume.log" 2>&1
PW_VOL=""
for _ in $(seq 1 20); do
    PW_VOL="$(pw_node_volume)"
    [ "$PW_VOL" = "0.4000" ] && break
    sleep 0.2
done
if [ "$PW_VOL" = "0.4000" ]; then
    ok "volume 40 已落到 PipeWire 流上（Params.Props.volume=$PW_VOL）"
else
    bad "volume 40 未落到 PipeWire 流上（Params.Props.volume=${PW_VOL:-?}，期望 0.4000；"
    bad "  典型成因：pw_stream_set_control 未持 thread loop 锁，被库以 -EPERM 静默拒绝）"
fi
"$TM_BIN" pause > "$WORK_DIR/pw-assert-pause.log" 2>&1
PW_PAUSED=""
for _ in $(seq 1 20); do
    PW_PAUSED="$(pw_node_state)"
    [ "$PW_PAUSED" != "running" ] && break
    sleep 0.2
done
case "$PW_PAUSED" in
    running|none|"") bad "pause 未落到 PipeWire 流上（节点 state=${PW_PAUSED:-?}）" ;;
    *) ok "pause 已落到 PipeWire 流上（节点 state=$PW_PAUSED）" ;;
esac
"$TM_BIN" resume > "$WORK_DIR/pw-assert-resume.log" 2>&1
PW_RESUMED=""
for _ in $(seq 1 20); do
    PW_RESUMED="$(pw_node_state)"
    [ "$PW_RESUMED" = "running" ] && break
    sleep 0.2
done
if [ "$PW_RESUMED" = "running" ]; then
    ok "resume 后节点回到 running"
else
    bad "resume 后节点状态为 ${PW_RESUMED:-?}（期望 running）"
fi
WRONGCTX=0
DAEMON_LOG="$XDG_CONFIG_HOME/ter-music/daemon.log"
if [ -f "$DAEMON_LOG" ]; then
    WRONGCTX="$(grep -c 'wrong context' "$DAEMON_LOG")"
fi
if [ "${WRONGCTX:-0}" -eq 0 ]; then
    ok "daemon.log 无 PipeWire wrong context（pw_stream_* 调用上下文正确）"
else
    bad "daemon.log 出现 $WRONGCTX 次 wrong context（pw_stream_* 未持 thread loop 锁）"
    grep -n 'wrong context' "$DAEMON_LOG" | head -5 | sed 's/^/           | /' || true
fi

# ── 调试日志：PipeWire 选中且未回退 ──────────────────────────────
info "检查核心调试日志的后端选择"
LOG_FILE="$(latest_log)"
if [ -z "$LOG_FILE" ] || [ ! -f "$LOG_FILE" ]; then
    bad "找不到 $XDG_CONFIG_HOME/ter-music/ter-music-debug-*.log"
else
    if grep -Eq 'PipeWire backend selected|audio\.backend\.pipewire|Trying PipeWire backend' "$LOG_FILE"; then
        ok "日志确认 PipeWire 后端被选中（$(basename "$LOG_FILE")）"
    else
        bad "日志中没有 PipeWire 后端选中记录"
    fi
    if grep -Eq 'Falling back to PulseAudio|falling back to PulseAudio|PipeWire backend failed|PipeWire connection failed|PipeWire library could not be loaded|PipeWire stream error|PipeWire stream stuck|PipeWire set volume failed|Audio backend switched|looking for a fallback|No usable audio backend' "$LOG_FILE"; then
        bad "日志出现 PipeWire 失败/回退记录"
        grep -En 'Falling back to PulseAudio|falling back to PulseAudio|PipeWire backend failed|PipeWire connection failed|PipeWire library could not be loaded|PipeWire stream error|PipeWire stream stuck|PipeWire set volume failed|Audio backend switched|looking for a fallback|No usable audio backend' \
            "$LOG_FILE" | sed 's/^/           | /' || true
    else
        ok "日志中没有 PulseAudio 回退或 PipeWire 失败记录"
    fi
fi

# ── 连续 play/stop 稳定性 ─────────────────────────────────────────
info "连续 20 轮 play/stop，观察核心与 fd 数量"
LOOP_FAIL=0
FD_FIRST=-1
FD_LAST=-1
for i in $(seq 1 20); do
    "$TM_BIN" play "$WAV" > "$WORK_DIR/loop-play-$i.log" 2>&1
    RC=$?
    if [ "$RC" -ne 0 ]; then
        bad "第 $i 轮 play 失败（退出码 $RC）"
        LOOP_FAIL=$((LOOP_FAIL + 1))
    fi
    sleep 0.1

    "$TM_BIN" stop > "$WORK_DIR/loop-stop-$i.log" 2>&1
    RC=$?
    if [ "$RC" -ne 0 ]; then
        bad "第 $i 轮 stop 失败（退出码 $RC）"
        LOOP_FAIL=$((LOOP_FAIL + 1))
    fi
    sleep 0.1

    if ! daemon_alive; then
        bad "第 $i 轮 play/stop 后核心已退出"
        LOOP_FAIL=$((LOOP_FAIL + 1))
        break
    fi

    CURRENT_FD="$(fd_count "$DAEMON_PID")"
    [ "$FD_FIRST" -lt 0 ] && FD_FIRST="$CURRENT_FD"
    FD_LAST="$CURRENT_FD"
done
if [ "$LOOP_FAIL" -eq 0 ]; then
    ok "20 轮 play/stop 全部成功，核心始终存活"
fi
# 与普查脚本同口径的"至多一路"守卫：反复建流/拆流之后，核心进程与
# ter-music 播放流节点都不许累积（叠播的形态就是这两项之一超过 1）。
LOOP_CORES="$(core_count)"
LOOP_NODES="$(pw_node_count)"
if [ "$LOOP_CORES" = "1" ]; then
    ok "20 轮 play/stop 后核心进程数仍为 1"
else
    bad "20 轮 play/stop 后核心进程数为 $LOOP_CORES（期望 1）"
fi
if [ "$LOOP_NODES" -le 1 ]; then
    ok "20 轮 play/stop 后 ter-music 播放流节点数为 $LOOP_NODES（≤1，未累积）"
else
    bad "20 轮 play/stop 后 ter-music 播放流节点数为 $LOOP_NODES（>1，出现叠播形态）"
fi
if [ "$FD_FIRST" -ge 0 ] && [ "$FD_LAST" -ge 0 ]; then
    FD_DELTA=$((FD_LAST - FD_FIRST))
    if [ "$FD_DELTA" -le 10 ]; then
        ok "fd 数量稳定：首轮 $FD_FIRST → 末轮 $FD_LAST（差值 $FD_DELTA）"
    else
        bad "fd 数量持续增长：首轮 $FD_FIRST → 末轮 $FD_LAST（差值 $FD_DELTA > 10）"
    fi
else
    bad "未能采集 fd 数量"
fi

# ── AUTO 模式：仍然选择 PipeWire 且不回退 ─────────────────────────
info "验证 AUTO 模式（audio_backend=0）仍选择 PipeWire"
"$TM_BIN" daemon stop >/dev/null 2>&1 || true
for _ in $(seq 1 25); do     # 等旧核心完全退出，避免主实例名冲突
    [ -z "$(daemon_pid)" ] && break
    sleep 0.2
done
cat > "$XDG_CONFIG_HOME/ter-music/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <audio_backend>0</audio_backend>
    <stop_core_on_exit>0</stop_core_on_exit>
  </preferences>
</ter-music-config>
EOF
# 旧日志清掉，确保下面读到的是本次 AUTO 启动产生的日志
rm -f "$XDG_CONFIG_HOME/ter-music"/ter-music-debug-*.log

TER_MUSIC_FORCE_LOG=1 "$TM_BIN" daemon start --open "$MUSIC_DIR" \
    > "$WORK_DIR/daemon-start-auto.log" 2>&1
if [ $? -ne 0 ] || ! wait_for_daemon; then
    bad "AUTO 模式核心启动失败"
    diag "AUTO 启动"
else
    if "$TM_BIN" play "$WAV" > "$WORK_DIR/play-auto.log" 2>&1; then
        AUTO_MAX=-1
        for _ in $(seq 1 15); do     # 最多约 3 秒
            POS="$(show_position_ms)"
            case "$POS" in
                ''|*[!0-9-]*) POS=-1 ;;
            esac
            [ "$POS" -gt "$AUTO_MAX" ] && AUTO_MAX="$POS"
            sleep 0.2
        done
        if [ "$AUTO_MAX" -ge 1000 ]; then
            ok "AUTO 模式播放位置推进（最大 ${AUTO_MAX}ms）"
        else
            bad "AUTO 模式播放位置未推进到 1 秒（最大 ${AUTO_MAX}ms）"
            diag "AUTO 进度"
        fi
    else
        bad "AUTO 模式 play <wav> 失败"
        diag "AUTO play"
    fi

    LOG_FILE="$(latest_log)"
    if [ -n "$LOG_FILE" ] && [ -f "$LOG_FILE" ] \
        && grep -Eq 'PipeWire backend selected|audio\.backend\.pipewire|Trying PipeWire backend' "$LOG_FILE"; then
        ok "AUTO 模式日志确认选择 PipeWire"
    else
        bad "AUTO 模式日志未显示 PipeWire 选择"
    fi
    if [ -n "$LOG_FILE" ] && [ -f "$LOG_FILE" ] \
        && grep -Eq 'Falling back to PulseAudio|falling back to PulseAudio|PipeWire backend failed|PipeWire stream error|PipeWire stream stuck|PipeWire set volume failed|Audio backend switched|looking for a fallback|No usable audio backend' "$LOG_FILE"; then
        bad "AUTO 模式出现 PipeWire 失败/回退记录"
        grep -En 'Falling back to PulseAudio|falling back to PulseAudio|PipeWire backend failed|PipeWire stream error|PipeWire stream stuck|PipeWire set volume failed|Audio backend switched|looking for a fallback|No usable audio backend' \
            "$LOG_FILE" | sed 's/^/           | /' || true
    else
        ok "AUTO 模式无 PulseAudio 回退或 PipeWire 失败记录"
    fi
    "$TM_BIN" stop >/dev/null 2>&1 || true
fi

# ── 收尾：停止播放与核心，确认 PipeWire 无残留节点 ───────────────
info "停止播放并关闭核心"
"$TM_BIN" stop >/dev/null 2>&1 || true
"$TM_BIN" daemon stop >/dev/null 2>&1 || true

NODE_GONE=0
for _ in $(seq 1 10); do     # 最多约 5 秒
    NODES="$(pw-cli ls Node 2>/dev/null)" || NODES=""
    if ! grep -qi 'ter-music' <<< "$NODES"; then
        NODE_GONE=1
        break
    fi
    sleep 0.5
done
if [ "$NODE_GONE" -eq 1 ]; then
    ok "pw-cli ls Node 中无 ter-music 残留节点"
else
    bad "核心停止后 PipeWire 仍有 ter-music 节点"
    grep -i 'ter-music' <<< "$NODES" | sed 's/^/           | /' || true
fi
# 同口径复核（普查脚本用的就是这两个计数，两套脚本不许互相矛盾）
FINAL_CORES="$(core_count)"
FINAL_NODES="$(pw_node_count)"
if [ "$FINAL_CORES" = "0" ] && [ "$FINAL_NODES" = "0" ]; then
    ok "收尾后核心进程数=0、ter-music 播放流节点数=0（与普查口径一致）"
else
    bad "收尾后仍残留核心 $FINAL_CORES 个 / 播放流节点 $FINAL_NODES 路"
fi

finish
