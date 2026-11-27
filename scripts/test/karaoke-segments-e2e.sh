#!/usr/bin/env bash
#
# 分片消音（v2 会话分段缓存）真实模型/真实曲目端到端回归
#
# 覆盖 T4 验收里必须真跑的部分（真实 ONNX 模型 + 真实曲目 + 真实核心）：
#   S0 前置：受支持模型（大小 + SHA256）、libonnxruntime、ffmpeg/ffprobe；
#            缺任一条件相关用例明确 SKIP（不伪装通过）。
#   S1 会话缓存生命周期 + D1 首段起播：
#        - 启动清理同 uid、pid 已不存在的 /tmp/ter-music-karaoke-* 残留；
#        - 无 v1 缓存时首次起播等待首段伴奏就绪（记录秒数，上限 30s），
#          状态日志可见"karaoke segmented source ready"；
#        - 顺序播放推进（位置越过 15s/20s）且不触发段回退；
#        - 段布局 segNNN.inst.flac/soft.flac/ready + manifest.txt，
#          44.1kHz/2ch/16bit、非末段 661500 帧。
#   S2 seek（D4）：seek 到未处理远段 → 先给 worker 优先提示，等待 ≤8s；
#        就绪则继续分片，否则回退原声；位置推进、不卡死。
#   S3 二次播放：同曲再播无需等首段/不重跑推理（段文件 inode/mtime 不变）。
#   S4 退出清理（D6）：daemon 退出后 /tmp 会话目录消失。
#   S5 v1 只读优先（D2）：伪造完整 v1 整曲缓存 → 优先使用（日志无
#        "(segmented)"），且不产生 /tmp 分段；v1 损坏 → 清理并回退 v2/原声。
#   S6 预加载联动（D5）：seamless_preload=0 不预取；=1 当前曲目 READY 后
#        后台预取队列下一曲；完整段集校验（manifest 字段、段帧数、总帧数、
#        soft=0.4×原声+0.6×inst、跨段连续）。
#   S7 边界：CUE 子曲目（cue_offset>0）不使用 v2 分段源，播原声。
#
# 单声道 / >30min / 模型损坏 / 缓存目录不可写等边界由 karaoke-model-e2e.sh
# 覆盖（同一套播放链路），本脚本不重复。
#
# 用法：scripts/test/karaoke-segments-e2e.sh [--bin <ter-music>] [--model <onnx>]
#                                            [--track <audio>] [--keep]
# 环境：会话总线（缺省 dbus-run-session）；TER_MUSIC_FORCE_LOG=1 由脚本设置。

set -uo pipefail

export TER_MUSIC_CLI_LANG=zh_CN
export TER_MUSIC_SANDBOX="${TER_MUSIC_SANDBOX:-0}"
export TER_MUSIC_FORCE_LOG=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/.tmp/stream-build/ter-music}"
MODEL_ARG=""
TRACK_ARG=""
KEEP=0
ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --model) MODEL_ARG="$2"; shift 2 ;;
        --track) TRACK_ARG="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) sed -n '2,44p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

# ── 私有会话总线（与 karaoke-model-e2e 同一约定）─────────────────────
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

SUPPORTED_SIZE=29704436
SUPPORTED_SHA=e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc
MODEL_ID=uvr-mdxnet-3-9662
MODEL_NAME=UVR_MDXNET_3_9662.onnx
SEG_FRAMES=661500

find_model() {
    local c
    for c in "$MODEL_ARG" "${TERMUSIC_KARAOKE_TEST_MODEL:-}" \
             "$REPO_ROOT/.tmp/karaoke-research/model/$MODEL_NAME" \
             "$REPO_ROOT/.tmp/karaoke-model/$MODEL_NAME" \
             "$HOME/Downloads/$MODEL_NAME"; do
        if [ -n "$c" ] && [ -f "$c" ] && [ "$(stat -c %s "$c" 2>/dev/null)" = "$SUPPORTED_SIZE" ]; then
            printf '%s' "$c"; return 0
        fi
    done
    return 1
}
MODEL="$(find_model)" || {
    echo "SKIP karaoke-segments-e2e：未找到受支持模型 $MODEL_NAME（$SUPPORTED_SIZE B）"
    exit 0
}
MODEL_SHA="$(sha256sum "$MODEL" | cut -d' ' -f1)"
[ "$MODEL_SHA" = "$SUPPORTED_SHA" ] || {
    echo "SKIP karaoke-segments-e2e：模型 SHA256 不匹配，拒绝用它做真实模型验证"
    exit 0
}
have_runtime() {
    # 静态链接构建：ORT 编进二进制（nm/readelf 命中 OrtGetApiBase），无需系统
    # .so；不要用 strings（GCC 合并重叠字符串，结果不可靠）。
    if [ -n "${TM_BIN:-}" ] && [ -x "$TM_BIN" ]; then
        local syms
        if command -v nm >/dev/null 2>&1; then
            # 本脚本 set -o pipefail：`nm | grep -q` 会因 grep 提前退出让 nm
            # 收到 SIGPIPE（141）而误判失败；先落成变量再匹配。
            syms="$(nm -C "$TM_BIN" 2>/dev/null || true)"
            case "$syms" in
                *OrtGetApiBase*) return 0 ;;
            esac
        fi
        if command -v readelf >/dev/null 2>&1; then
            syms="$(readelf -sW "$TM_BIN" 2>/dev/null || true)"
            case "$syms" in
                *OrtGetApiBase*) return 0 ;;
            esac
        fi
    fi
    command -v ldconfig >/dev/null 2>&1 && ldconfig -p 2>/dev/null | grep -q 'libonnxruntime\.so' && return 0
    local d
    for d in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /lib/x86_64-linux-gnu; do
        ls "$d"/libonnxruntime.so* >/dev/null 2>&1 && return 0
    done
    return 1
}
have_runtime || { echo "SKIP karaoke-segments-e2e：未找到 libonnxruntime（且二进制非静态链接 ORT）"; exit 0; }
have_ffmpeg=0
command -v ffmpeg >/dev/null 2>&1 && command -v ffprobe >/dev/null 2>&1 && have_ffmpeg=1

find_track() {
    local c
    for c in "$TRACK_ARG" "${TERMUSIC_KARAOKE_TEST_TRACK:-}" \
             "$HOME/Music/2002年的第一场雪 - 刀郎.flac" \
             "$HOME/Music/5_20AM - 刀酱.flac"; do
        [ -n "$c" ] && [ -f "$c" ] && { printf '%s' "$c"; return 0; }
    done
    return 1
}
TRACK="$(find_track)" || {
    echo "SKIP karaoke-segments-e2e：未找到真实曲目（用 --track 指定）"
    exit 0
}

# ── 工作目录与隔离环境 ──────────────────────────────────────────────
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-kar-seg-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
export XDG_DATA_HOME="$WORK_DIR/home/.local/share"
export TER_MUSIC_LOG_DIR="$WORK_DIR/logs"
mkdir -p "$XDG_CONFIG_HOME/ter-music" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
         "$TER_MUSIC_LOG_DIR"

# ffprobe：宿主 PATH 里可能只有 ffmpeg 包装器（linglong 层库路径）。这里复用
# ffmpeg 包装器的层路径造一个本地 ffprobe 包装器，保持脚本自包含。
if ! command -v ffprobe >/dev/null 2>&1; then
    _FF_D="$(sed -n 's/^D=//p' "$(command -v ffmpeg 2>/dev/null)" 2>/dev/null | head -1)"
    if [ -n "${_FF_D:-}" ] && [ -x /opt/host-tools/usr/bin/ffprobe ]; then
        mkdir -p "$WORK_DIR/bin"
        {
            printf '#!/bin/sh\n'
            printf 'export LD_LIBRARY_PATH="%s/lib/x86_64-linux-gnu:%s/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n' "$_FF_D" "$_FF_D"
            printf 'exec /opt/host-tools/usr/bin/ffprobe "$@"\n'
        } >"$WORK_DIR/bin/ffprobe"
        chmod +x "$WORK_DIR/bin/ffprobe"
        export PATH="$WORK_DIR/bin:$PATH"
    fi
fi
if command -v ffprobe >/dev/null 2>&1 && command -v ffmpeg >/dev/null 2>&1; then
    have_ffmpeg=1
fi
CONFIG="$XDG_CONFIG_HOME/ter-music/config.xml"
DATA_DIR="$XDG_DATA_HOME/ter-music"
INSTALLED="$DATA_DIR/models/$MODEL_ID.onnx"
V1_ROOT="$XDG_CACHE_HOME/ter-music/karaoke"
MUSIC="$WORK_DIR/music"
mkdir -p "$MUSIC" "$(dirname "$INSTALLED")"
cp -- "$MODEL" "$INSTALLED"
chmod 600 "$INSTALLED"
cp -- "$TRACK" "$MUSIC/real.flac"
if [ -f "${TRACK%.*}.lrc" ]; then cp -- "${TRACK%.*}.lrc" "$MUSIC/real.lrc"; fi
TRACK_DUR_MS="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$MUSIC/real.flac" 2>/dev/null |
    awk '{printf "%d", $1*1000}')"
[ -n "$TRACK_DUR_MS" ] && [ "${TRACK_DUR_MS:-0}" -gt 0 ] || { echo "错误：读不到曲目时长"; exit 2; }
TRACK_SEC=$((TRACK_DUR_MS / 1000))

CORE_FG_PID=""
DAEMON_LOG_DIR="$XDG_CONFIG_HOME/ter-music"
UMASK_UID="$(id -u)"
CORE_PID=""
SESSION_ROOT=""
LOG_MARK=0

cleanup() {
    [ -n "${CORE_FG_PID:-}" ] && kill "$CORE_FG_PID" 2>/dev/null
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    if [ "$KEEP" -eq 0 ] && [ -d "$WORK_DIR" ]; then rm -rf "$WORK_DIR"; fi
}
trap cleanup EXIT

# ── 核心/日志/状态工具 ──────────────────────────────────────────────
daemon_up() { "$TM_BIN" daemon status 2>/dev/null | grep -q 'pid'; }
wait_daemon() { local i; for i in $(seq 1 80); do daemon_up && return 0; sleep 0.25; done; return 1; }
start_core() {
    "$TM_BIN" daemon start >/dev/null 2>&1
    if ! wait_daemon; then
        "$TM_BIN" daemon foreground >"$WORK_DIR/core-fg.log" 2>&1 &
        CORE_FG_PID=$!
        wait_daemon || return 1
    fi
    CORE_PID="$("$TM_BIN" daemon status --json 2>/dev/null |
        python3 -c 'import json,sys;print(json.load(sys.stdin)["instance"]["pid"])' 2>/dev/null)"
    [ -n "${CORE_PID:-}" ] || return 1
    SESSION_ROOT="/tmp/ter-music-karaoke-$UMASK_UID-$CORE_PID"
    return 0
}
stop_core() {
    "$TM_BIN" stop >/dev/null 2>&1 || true
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    [ -n "$CORE_FG_PID" ] && kill "$CORE_FG_PID" 2>/dev/null
    CORE_FG_PID=""
    sleep 0.5
}
core_log() {
    local f
    f="$(ls -t "$DAEMON_LOG_DIR"/ter-music-debug-*.log 2>/dev/null | head -1)"
    [ -n "$f" ] && printf '%s' "$f"
}
mark_log() {
    local f; f="$(core_log)"
    LOG_MARK=0
    [ -n "$f" ] && LOG_MARK="$(wc -l <"$f")"
    CORE_LOG="$f"
}
log_since() {  # 输出 mark 之后的核心日志
    [ -n "${CORE_LOG:-}" ] && [ -f "$CORE_LOG" ] || return 0
    tail -n "+$((LOG_MARK + 1))" "$CORE_LOG" 2>/dev/null
}
log_has_since() { log_since | grep -qE "$1"; }
wait_log() {  # $1 正则 $2 超时秒
    local pat="$1" timeout="${2:-20}" i
    for i in $(seq 1 $((timeout * 4))); do
        log_has_since "$pat" && return 0
        sleep 0.25
    done
    return 1
}
show_json() { "$TM_BIN" show --json 2>/dev/null; }
json_get() {
    show_json | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
    print(eval(sys.argv[1], {"d": d}))
except Exception:
    pass' "$1" 2>/dev/null
}
pos_ms() { json_get 'int(d["playback"].get("position_ms") or 0)'; }
duration_ms() { json_get 'int(d["playback"].get("duration_ms") or 0)'; }
cfg() { "$TM_BIN" config get "$1" 2>/dev/null | head -1; }
cfgset() { "$TM_BIN" config set "$1" "$2" >/dev/null 2>&1; }

wait_pos() {  # $1 最小毫秒 $2 超时秒
    local want="$1" timeout="${2:-40}" i p
    for i in $(seq 1 $((timeout * 4))); do
        p="$(pos_ms)"; p="${p:-0}"
        [ "$p" -ge "$want" ] && return 0
        sleep 0.25
    done
    return 1
}
wait_pos_after() {  # $1 起始位置 $2 增量毫秒 $3 超时秒
    local base="$1" delta="$2" timeout="${3:-20}" i p
    for i in $(seq 1 $((timeout * 4))); do
        p="$(pos_ms)"; p="${p:-0}"
        [ "$p" -ge $((base + delta)) ] && return 0
        sleep 0.25
    done
    return 1
}
# 会话根下所有 track 目录的 manifest 路径
manifests() { [ -n "${SESSION_ROOT:-}" ] && find "$SESSION_ROOT" -name manifest.txt -type f 2>/dev/null; }
manifest_field() { sed -n "s/^$2=//p" "$1" 2>/dev/null | head -1; }
complete_manifests() {
    local m n=0
    for m in $(manifests); do
        [ "$(manifest_field "$m" complete)" = "1" ] && n=$((n + 1))
    done
    printf '%d' "$n"
}
track_dirs() {  # 含 manifest 的曲目目录数
    local m n=0
    for m in $(manifests); do n=$((n + 1)); done
    printf '%d' "$n"
}
wait_complete_manifest() {  # $1 超时秒 → 输出 manifest 路径
    local timeout="${1:-120}" i m
    for i in $(seq 1 $((timeout * 2))); do
        for m in $(manifests); do
            if [ "$(manifest_field "$m" complete)" = "1" ]; then
                printf '%s' "$m"; return 0
            fi
        done
        sleep 0.5
    done
    return 1
}
seg_ready_count() { [ -n "${SESSION_ROOT:-}" ] && find "$SESSION_ROOT" -name 'seg*.ready' -type f 2>/dev/null | wc -l || printf '0'; }

flac_frames() {  # $1 flac → 每声道帧数（FLAC duration_ts 即样本数）
    local ts
    ts="$(ffprobe -v error -select_streams a:0 -show_entries stream=duration_ts \
          -of csv=p=0 "$1" 2>/dev/null | tr -d ',')"
    [ -n "$ts" ] && [ "$ts" != "N/A" ] && printf '%s' "$ts"
}
flac_fmt() {  # FLAC 的 bits_per_sample 常报 0；位深看 bits_per_raw_sample
    ffprobe -v error -select_streams a:0 -show_entries \
        stream=sample_rate,channels,bits_per_raw_sample -of csv=p=0 "$1" 2>/dev/null
}
decode_f32() {  # $1 输入（可用 -i 参数）→ stdout f32le 交织
    ffmpeg -v error -i "$1" -ar 44100 -ac 2 -f f32le - 2>/dev/null
}

# ── S0 前置 ────────────────────────────────────────────────────────
info "S0 前置"
ok "模型 $MODEL（SHA256 ${MODEL_SHA:0:16}…）"
ok "真实曲目 $TRACK（${TRACK_SEC}s）"
ok "模型置于旧用户 XDG 只读兼容路径 \$XDG_DATA_HOME/ter-music/models/$MODEL_ID.onnx（0600）"

info "S1 会话缓存生命周期 + D1 首段起播 + 顺序推进"
# 陈旧会话目录（同 uid、pid 已不存在）应在启动时清理
DEAD_PID="$(python3 - <<'PY'
import os, time
pid = os.fork()
if pid == 0:
    os._exit(0)
os.waitpid(pid, 0)
print(pid)
PY
)"
STALE_DIR="/tmp/ter-music-karaoke-$UMASK_UID-$DEAD_PID"
mkdir -p "$STALE_DIR/dead/model"
echo x >"$STALE_DIR/dead/model/leftover"
start_core && ok "核心上线（pid=$CORE_PID）" || { bad "核心启动失败"; exit 1; }
[ -d "$STALE_DIR" ] && bad "陈旧会话目录未被启动清理：$STALE_DIR" \
    || ok "启动清理删除同 uid、pid 已不存在的陈旧目录"
[ -d "$SESSION_ROOT" ] && bad "会话根在首次使用前就存在：$SESSION_ROOT" \
    || ok "会话根首次使用前不存在（懒创建）"
# config v12：没有 feature/model 两个配置键，档位即开关（模型随包/兼容路径内置）。
cfgset karaoke_mode 2
cfgset seamless_preload 0
[ "$(cfg karaoke_mode)" = "2" ] \
    && ok "配置档位=full（config v12：无 feature/model 键，档位即开关）" \
    || bad "配置档位设置失败（mode=$(cfg karaoke_mode)）"
mark_log
T0="$(date +%s%3N)"
"$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
if wait_pos 1000 40; then
    T1="$(date +%s%3N)"
    WAIT_MS=$((T1 - T0))
    if [ "$WAIT_MS" -le 30000 ]; then
        ok "首段起播等待 ${WAIT_MS}ms（≤30000ms，D1）"
    else
        bad "首段起播等待 ${WAIT_MS}ms 超过 30s 上限"
    fi
else
    bad "40s 内没有开始播放（首段等待卡死）"
fi
if wait_log 'karaoke segmented source ready' 10; then
    ok "首段就绪后才起播（日志 karaoke segmented source ready）"
else
    bad "日志未见首段就绪记录"
fi
if log_has_since 'playing karaoke cache source=2 \(segmented\)'; then
    ok "播放源为分片伴奏（source=2 segmented）"
else
    bad "播放源不是分片伴奏（可能直接回退原声）"
fi
if manifests | grep -q .; then
    ok "会话根出现曲目 manifest：$(manifests | head -1)"
else
    bad "会话根没有 manifest（未写 /tmp 分段缓存）"
fi
# 段布局与格式（段 0 应已就绪）
SEG0_READY="$(find "$SESSION_ROOT" -name 'seg000.ready' -type f 2>/dev/null | head -1)"
if [ -n "$SEG0_READY" ]; then
    D="$(dirname "$SEG0_READY")"
    if [ -s "$D/seg000.inst.flac" ] && [ -s "$D/seg000.soft.flac" ] && [ -s "$D/manifest.txt" ]; then
        ok "段布局齐全（seg000.inst.flac/seg000.soft.flac/seg000.ready/manifest.txt）"
    else
        bad "段布局不齐：$(ls "$D" 2>/dev/null | tr '\n' ' ')"
    fi
    if [ "$have_ffmpeg" = "1" ]; then
        fmt="$(flac_fmt "$D/seg000.inst.flac")"
        [ "$fmt" = "44100,2,16" ] && ok "段格式 44.1kHz/2ch/16bit" \
            || bad "段格式异常：$fmt（期望 44100,2,16）"
        f0="$(flac_frames "$D/seg000.inst.flac")"
        [ "$f0" = "$SEG_FRAMES" ] && ok "非末段帧数 = $SEG_FRAMES（15s@44.1k）" \
            || bad "非末段帧数异常：$f0"
    fi
else
    bad "seg000.ready 未出现"
fi
# 顺序推进（越过 20s 说明段 1 也续上了，且没有回退）
if wait_pos 20000 60; then
    ok "顺序播放推进到 20s（位置 $(pos_ms)ms）"
else
    bad "顺序播放未推进到 20s（位置 $(pos_ms)ms）"
fi
if log_since | grep -q 'karaoke segment .* fallback'; then
    bad "顺序播放出现段回退（伴奏未跟上）"
else
    ok "顺序播放无段回退"
fi
n_ready="$(seg_ready_count)"
[ "$n_ready" -ge 2 ] && ok "已产出 ≥2 个就绪段（$n_ready）" \
    || bad "就绪段数不足：$n_ready"

info "S2 seek：优先提示 / 等待 ≤8s / 回退"
TARGET=$((TRACK_SEC > 120 ? TRACK_SEC - 70 : TRACK_SEC / 2))
mark_log
"$TM_BIN" seek "$TARGET" >/dev/null 2>&1
if wait_pos $((TARGET * 1000)) 10; then
    ok "seek 到 ${TARGET}s 生效（位置 $(pos_ms)ms）"
else
    bad "seek 未生效（位置 $(pos_ms)ms，目标 ${TARGET}s）"
fi
if wait_log 'karaoke seek to seg [0-9]+ not ready, waiting up to 8s' 8; then
    ok "seek 到未处理段时先等待（≤8s）"
else
    skip "seek 目标段已提前就绪（未触发等待，仍属正确路径）"
fi
P_SEEK="$(pos_ms)"; P_SEEK="${P_SEEK:-0}"
if wait_pos_after "$P_SEEK" 3000 25; then
    ok "seek 后位置继续推进（$(pos_ms)ms，无卡死）"
else
    bad "seek 后位置不推进（$(pos_ms)ms）"
fi
if log_has_since 'karaoke segment fallback to original'; then
    ok "段未就绪时回退原声继续（日志已记录，D3/D4）"
else
    ok "段在等待窗口内就绪，继续分片播放"
fi
if log_has_since 'karaoke seek seg [0-9]+ decode failed'; then
    bad "seek 段解码失败（缓存坏）"
fi
# D4 回填：seek 优先处理后 worker 继续到曲尾再回填缺失段，最终整曲完整。
# 回退原声不影响后台 job；这里等 manifest complete=1（上限 240s）。
BACKFILL_OK=0
for _ in $(seq 1 480); do
    if [ "$(complete_manifests)" -ge 1 ]; then BACKFILL_OK=1; break; fi
    sleep 0.5
done
[ "$BACKFILL_OK" = "1" ] && ok "seek 优先后回填完成：manifest complete=1（整曲段集完整，D4）" \
    || bad "seek 后 240s 内未回填完整（manifest complete≠1）"

info "S3 二次播放：不重跑推理、无需再等首段"
"$TM_BIN" stop >/dev/null 2>&1
sleep 0.8
SEG0D="$(dirname "$(find "${SESSION_ROOT:-/nonexistent}" -name 'seg000.ready' -type f 2>/dev/null | head -1)")"
[ -n "$SEG0D" ] && [ "$SEG0D" != "." ] || SEG0D="/nonexistent"
STAT_BEFORE="$(stat -c '%i:%Y:%s' "$SEG0D/seg000.inst.flac" "$SEG0D/seg000.soft.flac" 2>/dev/null | tr '\n' ' ')"
mark_log
T0="$(date +%s%3N)"
"$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
if wait_pos 1000 15; then
    T1="$(date +%s%3N)"
    FAST_MS=$((T1 - T0))
    if [ "$FAST_MS" -le 8000 ]; then
        ok "二次起播 ${FAST_MS}ms 内出声（复用已就绪段，未重新推理）"
    else
        bad "二次起播等待 ${FAST_MS}ms（可能重跑了推理）"
    fi
else
    bad "二次起播 15s 内没有出声"
fi
STAT_AFTER="$(stat -c '%i:%Y:%s' "$SEG0D/seg000.inst.flac" "$SEG0D/seg000.soft.flac" 2>/dev/null | tr '\n' ' ')"
[ "$STAT_BEFORE" = "$STAT_AFTER" ] && ok "段文件未被重写（inode/mtime/size 不变 ⇒ 未重跑模型）" \
    || bad "段文件被重写（发生第二次推理）"
if log_since | grep -q 'segments ready:.*real.flac'; then
    bad "二次起播触发了新的整曲分离 job"
else
    ok "二次起播没有新的整曲分离 job"
fi

info "S4 退出清理（D6）"
"$TM_BIN" stop >/dev/null 2>&1
"$TM_BIN" daemon stop --all >/dev/null 2>&1
[ -n "$CORE_FG_PID" ] && kill "$CORE_FG_PID" 2>/dev/null
CORE_FG_PID=""
GONE=0
for i in $(seq 1 40); do
    [ -d "$SESSION_ROOT" ] || { GONE=1; break; }
    sleep 0.25
done
[ "$GONE" = "1" ] && ok "退出后 /tmp 会话目录已删除：$SESSION_ROOT" \
    || bad "退出后 /tmp 会话目录仍存在：$SESSION_ROOT"
sleep 0.5

info "S5 v1 整曲缓存只读优先（D2）"
# 伪造完整 v1 缓存：hash 复刻 karaoke_cache_paths 的键材料
REAL="$(readlink -f "$MUSIC/real.flac")"
SIZE="$(stat -c %s "$MUSIC/real.flac")"
MT_SEC="$(stat -c %Y "$MUSIC/real.flac")"
MT_NSEC="$(python3 -c 'import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns%1000000000)' "$MUSIC/real.flac")"
V1_HASH="$(python3 - "$MODEL_ID" "$SUPPORTED_SHA" "$REAL" "$SIZE" "$MT_SEC" "$MT_NSEC" <<'PY'
import hashlib, sys
model, sha, real, size, sec, nsec = sys.argv[1:7]
material = ("ter-music-karaoke-v1\nstem=2\n%s\n%s\n%s\n%s\n%s.%09d\n"
            "overlap=0.250000\nsoft_inst=0.600000\nflac=16bit,2ch\n"
            % (model, sha, real, size, int(sec), int(nsec)))
print(hashlib.sha256(material.encode()).hexdigest())
PY
)"
V1_DIR="$V1_ROOT/$MODEL_ID/$V1_HASH"
mkdir -p "$V1_DIR"
SRC_RATE="$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate -of csv=p=0 "$MUSIC/real.flac" | tr -d ',')"
if [ "$have_ffmpeg" = "1" ]; then
    ffmpeg -v error -i "$MUSIC/real.flac" -c:a flac -y "$V1_DIR/inst.flac" 2>/dev/null
    ffmpeg -v error -i "$MUSIC/real.flac" -c:a flac -y "$V1_DIR/soft.flac" 2>/dev/null
    SRC_FRAMES="$(flac_frames "$V1_DIR/inst.flac")"
    SOFT_FRAMES="$(flac_frames "$V1_DIR/soft.flac")"
    {
        printf 'format=1\nmodel_id=%s\nmodel_sha256=%s\n' "$MODEL_ID" "$SUPPORTED_SHA"
        printf 'overlap=0.250000\nsoft_inst=0.600000\n'
        printf 'source_frames=%s\nsource_rate=%s\n' "${SRC_FRAMES:-0}" "$SRC_RATE"
        printf 'inst_frames=%s\nsoft_frames=%s\n' "${SRC_FRAMES:-0}" "${SOFT_FRAMES:-0}"
    } >"$V1_DIR/meta.txt"
    start_core && ok "核心重启（v1 夹具就位）" || bad "核心启动失败"
    cfgset karaoke_mode 2
    mark_log
    T0="$(date +%s%3N)"
    "$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
    if wait_pos 1000 20; then
        T1="$(date +%s%3N)"
        ok "v1 缓存起播出声（$((T1 - T0))ms）"
    else
        bad "v1 缓存起播 20s 内未出声"
    fi
    if log_has_since 'playing karaoke cache source=2 for'; then
        ok "优先使用 v1 整曲缓存（日志无 segmented）"
    else
        bad "没有使用 v1 整曲缓存"
    fi
    sleep 1
    [ "$(track_dirs)" = "0" ] && ok "v1 命中时不产生 /tmp v2 分段" \
        || bad "v1 命中却产生了 v2 分段目录（$(track_dirs) 个）"
    # v1 损坏 → 清理并回退（v2/原声）
    "$TM_BIN" stop >/dev/null 2>&1
    sleep 0.5
    truncate -s 100 "$V1_DIR/soft.flac" 2>/dev/null
    mark_log
    "$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
    if wait_pos 1000 40; then
        ok "v1 损坏后仍能起播（回退 v2 分段/原声）"
    else
        bad "v1 损坏后 40s 内未起播"
    fi
    [ -d "$V1_DIR" ] && bad "损坏的 v1 缓存目录未被清理" \
        || ok "损坏的 v1 缓存被清理并回退"
    "$TM_BIN" stop >/dev/null 2>&1
    rm -rf "$V1_ROOT"
else
    skip "ffmpeg 缺失：v1 夹具/损坏回退断言"
fi
"$TM_BIN" daemon stop --all >/dev/null 2>&1
[ -n "$CORE_FG_PID" ] && kill "$CORE_FG_PID" 2>/dev/null
CORE_FG_PID=""
sleep 1

info "S6 预加载联动（D5）+ 完整段集校验"
if [ "$have_ffmpeg" != "1" ]; then
    skip "ffmpeg 缺失：预加载/完整段集校验"
else
    mkdir -p "$WORK_DIR/preload"
    ffmpeg -v error -ss 10 -t 60 -i "$MUSIC/real.flac" -c:a flac -y "$WORK_DIR/preload/a.flac" 2>/dev/null
    ffmpeg -v error -ss 90 -t 60 -i "$MUSIC/real.flac" -c:a flac -y "$WORK_DIR/preload/b.flac" 2>/dev/null
    if [ -s "$WORK_DIR/preload/a.flac" ] && [ -s "$WORK_DIR/preload/b.flac" ]; then
        ok "造 2 首 60s 真实片段夹具（预加载两态用）"
    else
        bad "预加载夹具生成失败"
    fi
    for pre in 0 1; do
        start_core || { bad "核心启动失败（preload=$pre）"; continue; }
        cfgset karaoke_mode 2
        cfgset seamless_preload "$pre"
        mark_log
        "$TM_BIN" play "$WORK_DIR/preload" >/dev/null 2>&1
        MAN="$(wait_complete_manifest 150)" || MAN=""
        if [ -z "$MAN" ]; then
            bad "preload=$pre：150s 内当前曲目段集未完整"
            "$TM_BIN" daemon stop --all >/dev/null 2>&1
            continue
        fi
        ok "preload=$pre：当前曲目 60s 分离完成（manifest complete=1）"
        if [ "$pre" = "1" ]; then
            if wait_log 'karaoke preload next track' 20; then
                ok "preload=1：当前曲目 READY 后请求预取队列下一曲"
            else
                bad "preload=1：没有预取下一曲的日志"
            fi
            N2=0
            for _ in $(seq 1 40); do
                N2="$(track_dirs)"
                [ "$N2" -ge 2 ] && break
                sleep 0.5
            done
            [ "$N2" -ge 2 ] && ok "preload=1：后台出现第二首曲目的会话段目录（$N2）" \
                || bad "preload=1：未出现第二首曲目的段目录（$N2）"
        else
            sleep 4
            N0_="$(track_dirs)"
            [ "$N0_" = "1" ] && ok "preload=0：不预取（会话内仅当前曲目段目录）" \
                || bad "preload=0：出现了额外段目录（$N0_）"
        fi
        # 完整段集校验（只在首个完成的曲目目录上做一次）
        if [ "$pre" = "0" ]; then
            D="$(dirname "$MAN")"
            fmt="$(manifest_field "$MAN" format)"; sr="$(manifest_field "$MAN" sample_rate)"
            ss="$(manifest_field "$MAN" segment_seconds)"; total="$(manifest_field "$MAN" total_segments)"
            srcf="$(manifest_field "$MAN" source_frames)"; model="$(manifest_field "$MAN" model_id)"
            sha="$(manifest_field "$MAN" model_sha256)"
            if [ "$fmt" = "2" ] && [ "$sr" = "44100" ] && [ "$ss" = "15" ] \
               && [ "${total:-0}" -ge 4 ] && [ "${srcf:-0}" -gt 0 ] \
               && [ "$model" = "$MODEL_ID" ] && [ "$sha" = "$SUPPORTED_SHA" ]; then
                ok "manifest 字段齐全（format=2/model/sha/44100/15/total=$total/source_frames=$srcf/complete）"
            else
                bad "manifest 字段异常：format=$fmt sr=$sr ss=$ss total=$total src=$srcf model=$model"
            fi
            sum=0; ni=0; bad_frames=0
            for i in $(seq 0 $((total - 1))); do
                f="$D/$(printf 'seg%03d.inst.flac' "$i")"
                [ -s "$f" ] || { bad_frames=$((bad_frames + 1)); continue; }
                fr="$(flac_frames "$f")"
                [ -n "$fr" ] || { bad_frames=$((bad_frames + 1)); continue; }
                sum=$((sum + fr)); ni=$((ni + 1))
                if [ "$i" -lt $((total - 1)) ] && [ "$fr" != "$SEG_FRAMES" ]; then
                    bad "非末段 $i 帧数=$fr（期望 $SEG_FRAMES）"
                fi
            done
            [ "$bad_frames" = "0" ] && [ "$ni" = "$total" ] \
                && ok "全部 $total 段文件存在且可读（非末段均 $SEG_FRAMES 帧）" \
                || bad "段文件缺失/不可读：bad=$bad_frames 读到 $ni/$total"
            [ "$sum" = "$srcf" ] && ok "段帧数总和 = 模型输出总帧数（$sum）" \
                || bad "段帧数总和 $sum ≠ manifest source_frames $srcf"
            last_f="$(flac_frames "$D/$(printf 'seg%03d.inst.flac' $((total - 1)))")"
            [ "$last_f" = "$((srcf - (total - 1) * SEG_FRAMES))" ] \
                && ok "末段帧数 = 剩余帧（$last_f）" \
                || bad "末段帧数异常：$last_f"
            # soft = 0.4×原声 + 0.6×inst；跨段边界连续（20s 处仍在同一时间轴）
            ORIG_RAW="$WORK_DIR/orig.f32"
            INST_RAW="$WORK_DIR/inst.f32"
            SOFT_RAW="$WORK_DIR/soft.f32"
            decode_f32 "$WORK_DIR/preload/a.flac" >"$ORIG_RAW"
            : >"$INST_RAW"; : >"$SOFT_RAW"
            for i in $(seq 0 $((total - 1))); do
                decode_f32 "$D/$(printf 'seg%03d.inst.flac' "$i")" >>"$INST_RAW"
                decode_f32 "$D/$(printf 'seg%03d.soft.flac' "$i")" >>"$SOFT_RAW"
            done
            if python3 - "$ORIG_RAW" "$INST_RAW" "$SOFT_RAW" "$SEG_FRAMES" <<'PY'
import sys, array
orig_p, inst_p, soft_p, seg_frames = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
def load(p):
    a = array.array('f')
    with open(p, 'rb') as f:
        data = f.read()
    a.frombytes(data[:len(data) - (len(data) % 4)])
    return a
o, i, s = load(orig_p), load(inst_p), load(soft_p)
if len(i) != len(s) or len(i) < 2 * 44100 * 10:
    print(f'FAIL lengths inst={len(i)} soft={len(s)}')
    sys.exit(1)
n = min(len(o), len(i))
if abs(len(o) - len(i)) > 2 * 2048:
    print(f'FAIL total-frames orig={len(o)//2} inst={len(i)//2}')
    sys.exit(1)
err2 = ref2 = 0.0
# 检查前 5s 与 18..22s（跨第一个段边界，证明无重复/丢失）
for start_s, dur_s in ((0, 5), (18, 4)):
    for k in range(start_s * 44100, min((start_s + dur_s) * 44100, n)):
        want0 = 0.4 * o[2 * k] + 0.6 * i[2 * k]
        want1 = 0.4 * o[2 * k + 1] + 0.6 * i[2 * k + 1]
        d0 = s[2 * k] - want0
        d1 = s[2 * k + 1] - want1
        err2 += d0 * d0 + d1 * d1
        ref2 += want0 * want0 + want1 * want1
rel = (err2 / ref2) ** 0.5 if ref2 > 0 else 9.9
if rel > 5e-3:
    print(f'FAIL soft-formula rel={rel:.2e}')
    sys.exit(1)
print(f'OK soft-formula rel={rel:.2e} frames={len(i)//2}')
PY
            then
                ok "soft = 0.4×原声 + 0.6×inst 且跨段边界连续（18–22s 复核）"
            else
                bad "soft 公式/跨段连续性校验失败"
            fi
        fi
        "$TM_BIN" stop >/dev/null 2>&1
        "$TM_BIN" daemon stop --all >/dev/null 2>&1
        [ -n "$CORE_FG_PID" ] && kill "$CORE_FG_PID" 2>/dev/null
        CORE_FG_PID=""
        sleep 1
    done
fi

info "S7 CUE 子曲目不使用 v2 分段源"
if [ "$have_ffmpeg" = "1" ]; then
    CUE_DIR="$WORK_DIR/cue"
    mkdir -p "$CUE_DIR"
    cp -- "$MUSIC/real.flac" "$CUE_DIR/album.flac"
    {
        printf 'FILE "album.flac" WAVE\n'
        printf '  TRACK 01 AUDIO\n    TITLE "A"\n    INDEX 01 00:10:00\n'
        printf '  TRACK 02 AUDIO\n    TITLE "B"\n    INDEX 01 01:00:00\n'
    } >"$CUE_DIR/album.cue"
    start_core || bad "核心启动失败（CUE）"
    cfgset karaoke_mode 2
    mark_log
    "$TM_BIN" play "$CUE_DIR/album.cue" >/dev/null 2>&1
    if wait_pos 1000 30; then
        ok "CUE 子曲目起播（原声）"
    else
        bad "CUE 子曲目 30s 内未起播"
    fi
    if wait_log 'karaoke segments skipped for CUE sub-track' 10; then
        ok "CUE 子曲目（cue_offset>0）明确跳过 v2 分段源"
    else
        bad "CUE 子曲目仍走了 v2 分段请求"
    fi
    sleep 1
    [ "$(track_dirs)" = "0" ] && ok "CUE 子曲目未生成 v2 段目录" \
        || bad "CUE 子曲目生成了 v2 段目录（$(track_dirs) 个）"
    "$TM_BIN" daemon stop --all >/dev/null 2>&1
    [ -n "$CORE_FG_PID" ] && kill "$CORE_FG_PID" 2>/dev/null
    CORE_FG_PID=""
else
    skip "ffmpeg 缺失：CUE 子曲目边界"
fi

printf '\n%d passed, %d skipped, %d failed\n' "$PASS" "$SKIPPED" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
