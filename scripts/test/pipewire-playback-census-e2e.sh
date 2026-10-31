#!/usr/bin/env bash
#
# 播放路径普查（叠播回归取证）：反复"启动 TUI → 起播 → q 退出 → 立刻再启动"，
# 每轮量化"到底有几路在出声"，用于坐实"多播放线程并行/声音叠加"这一可听回归。
#
# 量化四项（每轮在“播放中”与“退出后”各采一次）：
#   (a) 核心进程数与 pid 列表        —— 是否叠的是**进程**
#   (b) 每进程线程数与线程名          —— 是否叠的是**线程**（ter-music-pw / module-rt）
#   (c) PipeWire ter-music 播放流数与各自 state —— 是否叠的是**流**（running 的流数）
#   (d) show --json 的 state/position_ms —— 与“听得到”对齐的旁证
#
# 不变量（任一被破坏即 FAIL）：
#   - 播放中：核心进程数 ≤1、每进程 ter-music-pw 线程数 ≤1、PipeWire running 播放流 ≤1
#   - 播放中：**远端前端 TUI 进程本身不得持有 ter-music-pw 音频线程**——
#     resume_last_playback=1 时若 facade 尚未初始化就按默认 LOCAL 判断，
#     TUI 会在本进程起一路流，与核心的一路叠播；该证据必须在 TUI 存活时采样。
#   - 退出后（stop_core_on_exit=1）：宽限期后核心进程数 ==0、ter-music 播放流 ==0
#   - 调试日志：播放线程 started 计数与 exiting 计数之差 ≤1（不允许只增不减）
#   - daemon.log：`wrong context` 计数为 0（PipeWire 调用必须在 thread loop 锁内）
#
# 素材：3 首可辨识正弦音（220/440/880 Hz，≥20s，S32LE/48kHz/2ch，python3 wave 生成，
#       不依赖 ffmpeg）。按周期整除取样，无点击噪声。
#
# 用法：scripts/test/pipewire-playback-census-e2e.sh [--bin <ter-music>]
#           [--backend N] [--rounds N] [--stop-core-on-exit 0|1] [--tight|--gap S] [--keep]
#   --bin <path>              被测可执行文件（默认 build/ter-music）
#   --backend N               audio_backend：0=AUTO 1=Pulse 2=ALSA 3=PipeWire（默认 3）
#   --rounds N                启停轮次（默认 6）
#   --stop-core-on-exit 0|1   前端退出是否终止核心（默认 1，与用户缺省一致）
#   --tight                   轮次间隔压到 0.3s（"q 之后立刻再开"口径，跳过宽限采样）
#   --gap S                   自定义轮次间隔秒数（默认 3.0）
#   --sample-after S          启动后第几秒采样（默认 5；必须早于 TUI 按 q，
#                             否则前端越界起播的线程证据会随进程退出消失）
#   --tui-wait S              TUI 在采样后继续存活到第几秒才按 q（默认 6）
#   --music DIR               用既有音乐目录（默认：临时生成 3 首正弦素材）
#   --config FILE             用既有 config.xml 作为初始夹具（默认：生成最小夹具）
#                             与 --music/--config 一起用时即为"用户真实环境"口径：
#                             夹具只在开头拷一次，之后由被测程序自己演化（resume 字段等）
#   --keep                    保留临时工作目录（失败时无论如何都会保留）
#   -h, --help                显示本帮助
#
# 环境：无 python3 / pw-dump / PipeWire 服务 / Audio/Sink 节点时整体 SKIP（退出 0）。
#       当前无会话总线时用 dbus-run-session 重入（PipeWire 客户端需会话环境）。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
BACKEND=3
ROUNDS=6
STOP_CORE_ON_EXIT=1
KEEP=0
WORK_DIR=""
USER_MUSIC=""
USER_CONFIG=""
# 轮次间隔：用户口径是"q 之后立刻再开"，用 --tight 把间隔压到 0.3s，
# 覆盖"旧核心刚释放总线名、进程仍在收尾"的窗口（默认 3.0s 是宽松口径）
GAP=3.0
# 真实曲库扫描比合成素材慢得多，采样点与前端的起播等待要能跟着调。
# TUI_WAIT 必须让 TUI 在 during 采样时仍然存活：远端前端"越界起播"表现为
# TUI 进程自己拥有 ter-music-pw 线程，TUI 一退出该证据就随进程消失。
SAMPLE_AFTER=5
TUI_WAIT=6

usage() {
    sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
}

ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --backend) BACKEND="$2"; shift 2 ;;
        --rounds) ROUNDS="$2"; shift 2 ;;
        --stop-core-on-exit) STOP_CORE_ON_EXIT="$2"; shift 2 ;;
        --tight) GAP=0.3; shift ;;
        --gap) GAP="$2"; shift 2 ;;
        --music) USER_MUSIC="$2"; shift 2 ;;
        --config) USER_CONFIG="$2"; shift 2 ;;
        --sample-after) SAMPLE_AFTER="$2"; shift 2 ;;
        --tui-wait) TUI_WAIT="$2"; shift 2 ;;
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
    printf '%d 通过, %d 失败, %d 跳过（backend=%s rounds=%s stop_core_on_exit=%s gap=%ss）\n' \
        "$PASS" "$FAIL" "$SKIP" "$BACKEND" "$ROUNDS" "$STOP_CORE_ON_EXIT" "$GAP"
    if [ "$FAIL" -ne 0 ]; then
        KEEP=1
        printf '（工作目录保留在 %s；逐轮普查：%s；核心日志：%s）\n' \
            "$WORK_DIR" "${CENSUS_TSV:-}" "${DAEMON_LOG_DIR:-}" >&2
    fi
    [ "$FAIL" -eq 0 ]
}

skip_exit() {
    skip "$1"
    finish
    exit 0
}

cleanup() {
    if [ -x "$TM_BIN" ]; then
        "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    fi
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先用 --bin 指定）" >&2; exit 2; }
TM_BIN="$(realpath "$TM_BIN")"
command -v python3 >/dev/null 2>&1 || skip_exit "没有 python3：无法生成素材与解析 pw-dump/show --json"
command -v pw-dump >/dev/null 2>&1 || skip_exit "没有 pw-dump：无法普查 PipeWire 播放流"
timeout 3 pw-cli info 0 >/dev/null 2>&1 || skip_exit "pw-cli info 0 失败：没有可用的 PipeWire 服务"
PW_DUMP_CHK="$(timeout 5 pw-dump 2>/dev/null)" || skip_exit "pw-dump 执行失败"
# here-string 而非管道：pipefail 下 grep -q 提前退出会让上游收 SIGPIPE(141) 造成误判
grep -q '"media.class": *"Audio/Sink"' <<< "$PW_DUMP_CHK" || skip_exit "PipeWire 中没有 Audio/Sink 输出节点（无声卡）"
unset PW_DUMP_CHK

if [ -z "${TM_TEST_BUS_READY:-}" ] && [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    command -v dbus-run-session >/dev/null 2>&1 \
        || { echo "错误：当前没有会话总线，且找不到 dbus-run-session。" >&2; exit 2; }
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

# ── 夹具：独立 HOME/配置目录 + 3 首正弦素材 ────────────────────────
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-census-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_CACHE_HOME="$HOME/.cache"
export TER_MUSIC_FORCE_LOG=1
MUSIC_DIR="${USER_MUSIC:-$WORK_DIR/music}"
mkdir -p "$XDG_CONFIG_HOME/ter-music" "$XDG_CACHE_HOME" "$MUSIC_DIR"
DAEMON_LOG_DIR="$XDG_CONFIG_HOME/ter-music"

# 夹具来源：--config 提供则按其原样拷一次并保持（模拟用户真实环境，
# resume 等字段由被测程序自己演化）；否则每轮重写最小夹具（可重复的合成分支）。
if [ -n "$USER_CONFIG" ]; then
    [ -f "$USER_CONFIG" ] || { echo "错误：--config 文件不存在：$USER_CONFIG" >&2; exit 2; }
    cp "$USER_CONFIG" "$XDG_CONFIG_HOME/ter-music/config.xml"
    FIXTURE_MODE=user
else
    FIXTURE_MODE=generated
fi

write_fixture() {
    [ "$FIXTURE_MODE" = generated ] || return 0
    cat > "$XDG_CONFIG_HOME/ter-music/config.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <audio_backend>$BACKEND</audio_backend>
    <stop_core_on_exit>$STOP_CORE_ON_EXIT</stop_core_on_exit>
    <auto_play_on_start>1</auto_play_on_start>
    <resume_last_playback>1</resume_last_playback>
    <remember_last_path>0</remember_last_path>
    <last_played_position>2</last_played_position>
  </preferences>
  <paths>
    <last_played_track_path>$MUSIC_DIR/tone-220.wav</last_played_track_path>
    <last_played_folder_path>$MUSIC_DIR</last_played_folder_path>
  </paths>
</ter-music-config>
EOF
}
write_fixture

if [ -n "$USER_MUSIC" ]; then
    [ -d "$USER_MUSIC" ] || { echo "错误：--music 目录不存在：$USER_MUSIC" >&2; exit 2; }
    echo "素材目录沿用：$USER_MUSIC（$(find "$USER_MUSIC" -maxdepth 1 -type f | wc -l) 个文件）"
else
python3 - "$MUSIC_DIR" <<'PY'
import math, os, sys
from array import array
from wave import open as wopen

out = sys.argv[1]
rate, ch, sec = 48000, 2, 24
amp = int(0.25 * (2 ** 31 - 1))

def gcd(a, b):
    while b:
        a, b = b, a % b
    return a

for name, freq in (("tone-220.wav", 220), ("tone-440.wav", 440), ("tone-880.wav", 880)):
    # 取样周期使整秒内正好是整数个正弦周期，避免接缝点击
    cycle = rate // gcd(freq, rate)
    mono = [int(amp * math.sin(2 * math.pi * i / cycle)) for i in range(cycle)]
    total = rate * sec
    frames = array('i')
    for _ in range(total // cycle + 1):
        frames.extend(mono)
    frames = frames[:total]
    interleaved = array('i')
    for v in frames:
        interleaved.extend((v,) * ch)
    with wopen(os.path.join(out, name), "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(4)
        w.setframerate(rate)
        w.writeframes(interleaved.tobytes())
PY
    [ "$(ls -1 "$MUSIC_DIR"/*.wav 2>/dev/null | wc -l)" -eq 3 ] \
        || { echo "错误：正弦素材生成失败" >&2; exit 2; }
fi

# ── 普查脚本：pw-dump → 每客户端的 running 播放流数 ────────────────
cat > "$WORK_DIR/census_pw.py" <<'PY'
"""输出 TSV：core_pid<TAB>client_id<TAB>node_id<TAB>node_name<TAB>state"""
import json, subprocess, sys

try:
    doc = json.loads(subprocess.run(["pw-dump"], capture_output=True, text=True,
                                    timeout=10).stdout or "[]")
except Exception as exc:                                    # noqa: BLE001
    print("#pw-dump 解析失败: %s" % exc, file=sys.stderr)
    sys.exit(0)

clients = {}   # client_id -> process pid
for obj in doc:
    if obj.get("type") != "PipeWire:Interface:Client":
        continue
    props = (obj.get("info") or {}).get("props") or {}
    if props.get("application.name") != "ter-music":
        continue
    pid = props.get("application.process.id")
    clients[str(obj["id"])] = int(pid) if str(pid).isdigit() else -1

for obj in doc:
    if obj.get("type") != "PipeWire:Interface:Node":
        continue
    info = obj.get("info") or {}
    props = info.get("props") or {}
    if props.get("node.name") != "ter-music":
        continue
    if props.get("media.category") != "Playback":
        continue
    cid = str(props.get("client.id", ""))
    print("\t".join(str(v) for v in
                    (clients.get(cid, -1), cid, obj.get("id"),
                     props.get("node.name"), info.get("state"))))
PY

core_pids() {
    # 只匹配核心进程（`daemon foreground`），pgrep -f 不会匹配到本脚本自身命令行
    pgrep -f 'daemon[ ]foreground' 2>/dev/null | tr '\n' ' '
}

# 前端 TUI 进程（argv 带 -o、但不是 daemon foreground）。用 /proc 的
# exe basename + cmdline 判定，避免 pgrep 把本脚本/包装进程算进来。
tui_pids() {
    local d exe cmd
    for d in /proc/[0-9]*; do
        [ -r "$d/cmdline" ] || continue
        exe="$(readlink "$d/exe" 2>/dev/null)" || continue
        [ "$(basename "$exe" 2>/dev/null)" = "ter-music" ] || continue
        cmd="$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null)"
        case "$cmd" in
            *" daemon foreground"*) continue ;;
            *" -o "*) printf '%s\n' "${d#/proc/}" ;;
        esac
    done
}

pw_rows() {
    python3 "$WORK_DIR/census_pw.py" 2>/dev/null
}

thread_lines() {
    local pid="$1"
    [ -d "/proc/$pid/task" ] || return 0
    ps -T -p "$pid" -o lwp=,stat=,comm= 2>/dev/null
}

pw_thread_count() {
    thread_lines "$1" | awk '$3=="ter-music-pw"' | grep -c .
}

rt_thread_count() {
    thread_lines "$1" | awk '$3=="module-rt"' | grep -c .
}

log_total() {
    local pattern="$1"
    local files
    files="$(ls -1 "$DAEMON_LOG_DIR"/ter-music-debug-*.log 2>/dev/null)"
    [ -n "$files" ] || { echo 0; return; }
    # shellcheck disable=SC2086  # 需要按行拆成多个文件参数
    grep -hc "$pattern" $files 2>/dev/null | awk '{s+=$1} END {print s+0}'
}

CENSUS_TSV="$WORK_DIR/census.tsv"
: > "$CENSUS_TSV"
printf 'round\twhen\tcores\tpids\tpw_threads\ttotal_threads\trunning_streams\tall_streams\tshow_state\tposition_ms\n' >> "$CENSUS_TSV"

sample() {
    local round="$1" when="$2"
    local pids pid cores=0 pwthreads=0 totalthreads=0 rows run all st pos
    pids="$(core_pids)"
    for pid in $pids; do
        cores=$((cores + 1))
        pwthreads=$((pwthreads + $(pw_thread_count "$pid")))
        totalthreads=$((totalthreads + $(thread_lines "$pid" | grep -c .)))
    done
    rows="$(pw_rows)"
    run="$(printf '%s\n' "$rows" | awk -F'\t' '$5=="running"' | grep -c .)"
    all="$(printf '%s\n' "$rows" | grep -c .)"
    local shown
    shown="$("$TM_BIN" show --json 2>/dev/null | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print("unknown\t-1"); raise SystemExit
pb = d.get("playback") or {}
st = d.get("state") or pb.get("state") or "unknown"
pos = pb.get("position_ms")
print("%s\t%s" % (st, int(pos) if isinstance(pos, (int, float)) else -1))')"
    st="$(printf '%s' "$shown" | cut -f1)"
    pos="$(printf '%s' "$shown" | cut -f2)"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$round" "$when" "$cores" "${pids:-none}" "$pwthreads" "$totalthreads" \
        "${run:-0}" "${all:-0}" "$st" "$pos" >> "$CENSUS_TSV"
    printf '  [%s/%s] cores=%s(%s) ter-music-pw=%s threads=%s pw_streams=%s(running %s/%s) show=%s@%sms\n' \
        "$round" "$when" "$cores" "${pids:-none}" "$pwthreads" "$totalthreads" \
        "${all:-0}" "${run:-0}" "${all:-0}" "$st" "$pos"
}

viol_running=0
viol_after=0
max_running_streams=0
max_cores=0
# 反假绿：全程一次都没进入播放状态时，上面的"至多一路"断言会空洞地全绿，
# 那绝不能算通过——必须显式判红（实测：真实曲库夹具下 6 轮全程 cores=0）。
observed_playing=0

info "普查开始（夹具=$FIXTURE_MODE backend=$BACKEND rounds=$ROUNDS stop_core_on_exit=$STOP_CORE_ON_EXIT gap=${GAP}s，素材 $MUSIC_DIR）"
rm -f "$DAEMON_LOG_DIR"/ter-music-debug-*.log "$DAEMON_LOG_DIR/daemon.log"

cd "$REPO_ROOT/scripts/test" || exit 2

for i in $(seq 1 "$ROUNDS"); do
    printf -- '=== 第 %s 轮 ===\n' "$i"
    # 不要每轮重写 config.xml：核心会在首轮起播后写入 resume_last_playback /
    # last_played_track_path，下一轮 TUI 才能走到"断点恢复"路径。这里正是
    # 历史回归点——远端前端若在 facade 初始化前按默认 LOCAL 判断，会在本进程
    # 起一路本地 PipeWire 流，与核心再起的一路叠播（本脚本的 streams>1 会抓到）。
    # 每轮重写会把 last_played_* 抹掉，恰好把该回归掩盖掉。
    # 每轮都真正切歌起播（键位 track.next = "n"，见 keymap/actions.c），
    # 让"上一路尚在收尾、新一路已 prepare"的交错反复发生；
    # 只靠 auto_play_on_start 会命中 play_audio() 的 "Skipping play: same index already playing"
    timeout 40 python3 tui-probe.py --bin "$TM_BIN" --music "$MUSIC_DIR" \
        --wait "$TUI_WAIT" --keys "nn" > "$WORK_DIR/probe-$i.log" 2>&1 &
    PROBE=$!

    sleep "$SAMPLE_AFTER"
    sample "$i" during
    DURING_STREAMS="$(awk -F'\t' -v r="$i" '$1==r && $2=="during"{print $8}' "$CENSUS_TSV")"
    DURING_STATE="$(awk -F'\t' -v r="$i" '$1==r && $2=="during"{print $9}' "$CENSUS_TSV")"
    if [ "${DURING_STREAMS:-0}" -gt 0 ] || [ "$DURING_STATE" = "playing" ]; then
        observed_playing=1
    fi
    RUNNING_STREAMS="$(awk -F'\t' -v r="$i" '$1==r && $2=="during"{print $7}' "$CENSUS_TSV")"
    CORES="$(awk -F'\t' -v r="$i" '$1==r && $2=="during"{print $3}' "$CENSUS_TSV")"
    [ "${CORES:-0}" -gt "$max_cores" ] && max_cores="$CORES"
    [ "${RUNNING_STREAMS:-0}" -gt "$max_running_streams" ] && max_running_streams="$RUNNING_STREAMS"
    if [ "${CORES:-0}" -gt 1 ]; then
        bad "第 $i 轮播放中出现 $CORES 个核心进程（叠的是进程）"
        viol_running=$((viol_running + 1))
    fi
    if [ "${RUNNING_STREAMS:-0}" -gt 1 ]; then
        bad "第 $i 轮播放中出现 $RUNNING_STREAMS 路 running 的 ter-music 播放流（叠的是流）"
        viol_running=$((viol_running + 1))
    fi
    for pid in $(core_pids); do
        PWT="$(pw_thread_count "$pid")"
        if [ "${PWT:-0}" -gt 1 ]; then
            bad "第 $i 轮核心 pid $pid 内出现 $PWT 个 ter-music-pw 线程（叠的是线程）"
            viol_running=$((viol_running + 1))
        fi
    done
    # 远端前端绝不允许自己拥有音频线程：resume_last_playback=1 时若
    # facade 尚未初始化、按默认 LOCAL 判定，TUI 会在本进程起一路 PipeWire
    # 流，与核心的一路叠播（而且 TUI 退出时不会按 remote 分支停它）。
    TUI_SEEN=0
    for pid in $(tui_pids); do
        TUI_SEEN=1
        TPW="$(pw_thread_count "$pid")"
        if [ "${TPW:-0}" -gt 0 ]; then
            bad "第 $i 轮远端前端 TUI pid $pid 自己持有 $TPW 个 ter-music-pw 音频线程（前端越界起播）"
            viol_running=$((viol_running + 1))
        fi
    done
    if [ "$TUI_SEEN" -eq 0 ]; then
        bad "第 $i 轮采样时没有前端 TUI 存活：前端越界起播的证据已随进程消失，本轮判定不可信（调大 --tui-wait）"
        viol_running=$((viol_running + 1))
    fi

    wait "$PROBE"
    sample "$i" after-quit
    RESIDUAL_WHEN="after-quit"
    # 间隔 <1s 视为"q 之后立刻再开"口径：跳过宽限采样，直接进入下一轮
    if [ "$(awk -v g="$GAP" 'BEGIN{print (g < 1.0) ? 1 : 0}')" = "1" ]; then
        sleep "$GAP"
    else
        sleep 1
        sample "$i" after-1s
        sleep 2
        sample "$i" after-3s
        RESIDUAL_WHEN="after-3s"
    fi

    if [ "$STOP_CORE_ON_EXIT" = "1" ]; then
        AFTER_CORES="$(awk -F'\t' -v r="$i" -v w="$RESIDUAL_WHEN" '$1==r && $2==w{print $3}' "$CENSUS_TSV")"
        AFTER_STREAMS="$(awk -F'\t' -v r="$i" -v w="$RESIDUAL_WHEN" '$1==r && $2==w{print $8}' "$CENSUS_TSV")"
        if [ "${AFTER_CORES:-0}" -ne 0 ] || [ "${AFTER_STREAMS:-0}" -ne 0 ]; then
            bad "第 $i 轮退出后（采样点 $RESIDUAL_WHEN）仍残留核心 ${AFTER_CORES:-0} 个 / 播放流 ${AFTER_STREAMS:-0} 路"
            viol_after=$((viol_after + 1))
        fi
    fi
done

# ── 汇总判定 ───────────────────────────────────────────────────────
info "逐轮普查表（$CENSUS_TSV）"
column -t -s $'\t' "$CENSUS_TSV" 2>/dev/null || cat "$CENSUS_TSV"

STARTED="$(log_total 'Playback thread started')"
EXITING="$(log_total 'Playback thread exiting')"
READY="$(log_total 'PipeWire stream ready')"
WRONGCTX=0
[ -f "$DAEMON_LOG_DIR/daemon.log" ] && \
    WRONGCTX="$(grep -c 'wrong context' "$DAEMON_LOG_DIR/daemon.log")"

info "日志计数：播放线程 started=$STARTED exiting=$EXITING（差值 $((STARTED - EXITING))），pw_stream_ready=$READY，wrong_context=$WRONGCTX"

if [ "$max_cores" -le 1 ]; then
    ok "全程核心进程数 ≤1（最大 $max_cores）"
else
    bad "全程核心进程数最大达到 $max_cores（>1）"
fi
if [ "$max_running_streams" -le 1 ]; then
    ok "全程 running 播放流 ≤1（最大 $max_running_streams）"
else
    bad "全程 running 播放流最大达到 $max_running_streams（>1）"
fi
if [ $((STARTED - EXITING)) -le 1 ]; then
    ok "播放线程未只增不减：started $STARTED / exiting $EXITING"
else
    bad "播放线程只增不减：started $STARTED 但只 exiting $EXITING（泄漏 $((STARTED - EXITING)) 路）"
fi
if [ "$WRONGCTX" -eq 0 ]; then
    ok "PipeWire 调用上下文正确（daemon.log 无 wrong context）"
else
    bad "daemon.log 出现 $WRONGCTX 次 wrong context（pw_stream_* 未在 thread loop 锁内调用）"
fi
[ "$viol_running" -eq 0 ] || bad "播放中共有 $viol_running 处违反“至多一路”不变量"
[ "$viol_after" -eq 0 ] || bad "退出后共有 $viol_after 处残留未清零"

if [ "$observed_playing" -eq 1 ]; then
    ok "至少一轮在采样点处于播放状态（断言非空洞）"
else
    bad "全程没有任何一轮在采样点处于播放状态：本轮普查是空洞通过（假绿），请调大 --tui-wait 或调小 --sample-after"
fi

if [ "$viol_running" -eq 0 ] && [ "$viol_after" -eq 0 ] && [ "$observed_playing" -eq 1 ]; then
    ok "$ROUNDS 轮反复启停后未复现叠播（至多一路不变量成立）"
fi

finish
