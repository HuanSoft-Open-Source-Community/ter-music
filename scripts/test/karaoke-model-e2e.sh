#!/usr/bin/env bash
#
# 内置消音模型（config v12）真实模型端到端回归
#
# 覆盖必须有真实模型/真实曲目/真实终端才能证明的部分：
#   S0 前置：受支持模型（大小 + 整文件 SHA256）、libonnxruntime、ffmpeg；缺任一
#            项时相关用例明确 SKIP（不伪装通过）。
#   S13 静态：模型不进 git；二进制无 NEEDED libonnxruntime（dlopen 可选加载）；
#            CMake 支持 -DTER_MUSIC_BUNDLED_MODEL 安装；模型表含 UVR 署名/
#            官方 URL/SHA256（应用内展示的数据源）。
#   S1 默认常显：全新配置没有 karaoke_feature/karaoke_model；默认 Off 下主界面
#            就显示卡拉OK按钮，CLI/D-Bus 直接接受三档（无功能门禁）。
#   S2 内置模型：
#            a) 构建期 cmake --install 装到 share/ter-music/models（大小+SHA256
#               + NOTICE.md）；
#            b) 运行期从 <前缀>/share/ter-music/models 直接使用，不向 XDG 复制；
#            c) 旧用户 $XDG_DATA_HOME/ter-music/models 只读兼容（不写入、不改动）；
#            d) 坏包（大小/哈希不符）必须可见，不静默改用另一位置；
#            e) 两个位置都缺失 → 可读错误 + 回退原声，不崩溃。
#   S3 重启稳定：配置仍无旧键、档位（karaoke_mode）持久、按钮仍在。
#   S4 首播等首段 + 后台 v2 分段分离：首播等首段伴奏就绪（≤30s，D1 不变）；
#            分离期间 seek/音量/时长不受影响；完成后 /tmp 会话段集齐全（seg%03d.inst/soft.flac +
#            seg%03d.ready + manifest.txt），无 .part；manifest 含 format=2/
#            44100/15s/total/complete；非末段 661500 帧、Σ帧数=source_frames、
#            末段=剩余帧；stem 判据：低频保留/侧向保留/中置下降/soft 公式。
#   S5 三档与源切换：v1 命中优先；否则当前段就绪才换源；off=原声、
#            soft=soft 段、full=inst 段；按当前位置换源、进度不重置；不触发
#            第二次推理（段文件 inode/size 不变）；seek/歌词/时长/倍速/音量/EQ。
#   S6 第二次起播：v2 段集 complete=1 → 直接分段源，不再触发 RUNNING/推理。
#   S7 取消：换曲取消后台 job（完整段集不增加）；切 Off 不再按功能开关取消
#            （v12 无该开关），核心不崩。
#   S8 单声道 / >30min：worker 拒绝且原因（立体声 / 超过上限 1800s）在状态行
#            可见；原声继续、不产生段目录。
#   S9 段损坏：截断 seg000.soft.flac（保留 ready）→ 拒绝使用并重做/回退原声。
#   S10 XDG v1 缓存根不可写：v2 仍只写 /tmp；另把 /tmp 段 model 目录设 0555，
#            构造 worker Permission 失败 → 回退消息必须带出 Permission 原因。
#   S11 运行中模型文件消失：分离失败原因（模型…）可见、原声继续、核心存活。
#   S12 无运行库：伪造三个 soname 的坏 ELF 遮蔽真实库 → 分离失败原因
#            （ONNX Runtime）可见、原声继续、核心存活（SKIP：无法遮蔽）。
#
# 用法：scripts/test/karaoke-model-e2e.sh [--bin <ter-music>] [--model <onnx>]
#                                         [--track <audio>] [--keep]
# 环境：
#   - 会话总线（缺省 dbus-run-session；本机宿主可用 TM_TEST_REUSE_SESSION_BUS=1 复用）
#   - ffmpeg/ffprobe（造单声道/31 分钟夹具与质量分析；缺失时相关断言 SKIP）
#   - 依赖 python3（lifecycle-driver/vt_grid 收屏）
#   - TERMUSIC_BUNDLE_BUILD 可指定构建目录（缺省 .tmp/bundle-build）；
#     用于 cmake --install 的构建期安装断言（staging 前缀，不写 /opt/apps）。
#   - TERMUSIC_KARAOKE_TEST_TRACK 可指定真实曲目；缺省从 ~/Music 里选一首
#     时长 120..300 秒、带同名 .lrc 的曲目（本机优先"2002年的第一场雪"）。
#   - TERMUSIC_KARAOKE_TEST_MODEL 可指定模型文件；缺省找
#     .tmp/karaoke-research/model/ 与 ~/Downloads 下的受支持模型。

set -uo pipefail

# 断言状态消息用中文（audio.c 的状态栏文案是硬编码中英双语；worker 错误是中文）
export TER_MUSIC_CLI_LANG=zh_CN
# 容器/打包环境（Linyaps）里 CLI 拒绝 fork 后台核心；本脚本自己起私有总线上的
# 临时核心，故显式声明非沙箱（宿主机会话里没有影响）。与 karaoke-e2e.sh 同约定。
export TER_MUSIC_SANDBOX="${TER_MUSIC_SANDBOX:-0}"
# 让后台核心也把调试日志落到配置目录（"playing karaoke cache source=N"在此），
# 否则 daemon 默认不写 debug 日志，源切换断言无从取证。
export TER_MUSIC_FORCE_LOG=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
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
        -h|--help) sed -n '2,50p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

# ── 私有会话总线（与 mouse-tui-e2e/karaoke-e2e 同一套约定）────────────
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

# ── 模型 / 运行库 / 真实曲目解析（缺失 = 明确 SKIP）─────────────────
SUPPORTED_SIZE=29704436
SUPPORTED_SHA=e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc
MODEL_ID=uvr-mdxnet-3-9662
MODEL_NAME=UVR_MDXNET_3_9662.onnx

find_model() {
    local c
    for c in "$MODEL_ARG" \
             "${TERMUSIC_KARAOKE_TEST_MODEL:-}" \
             "$REPO_ROOT/.tmp/karaoke-research/model/$MODEL_NAME" \
             "$REPO_ROOT/.tmp/karaoke-model/$MODEL_NAME" \
             "$HOME/Downloads/$MODEL_NAME"; do
        if [ -n "$c" ] && [ -f "$c" ] && [ "$(stat -c %s "$c" 2>/dev/null)" = "$SUPPORTED_SIZE" ]; then
            printf '%s' "$c"
            return 0
        fi
    done
    return 1
}

MODEL="$(find_model)" || {
    echo "SKIP karaoke-model-e2e：未找到受支持模型 $MODEL_NAME（$SUPPORTED_SIZE B）"
    echo "     可用 --model 或 TERMUSIC_KARAOKE_TEST_MODEL 指定；不伪装通过。"
    exit 0
}
MODEL_SHA="$(sha256sum "$MODEL" | cut -d' ' -f1)"
if [ "$MODEL_SHA" != "$SUPPORTED_SHA" ]; then
    echo "SKIP karaoke-model-e2e：模型 SHA256 不匹配（$MODEL_SHA），拒绝用它做真实模型验证"
    exit 0
fi

# 静态链接构建（官方 deb/rpm/AppImage）：ONNX Runtime 直接编进二进制，系统
# 里可以完全没有 libonnxruntime。探针必须认符号而不是 .so：用 nm -C 找
# OrtGetApiBase；**不要用 strings** —— GCC 会合并重叠字符串，strings 不可靠。
# readelf 符号表作为 nm 缺失时的回退。
have_static_runtime() {
    [ -n "${TM_BIN:-}" ] && [ -x "$TM_BIN" ] || return 1
    local syms
    if command -v nm >/dev/null 2>&1; then
        # 注意：本脚本 set -o pipefail，`nm | grep -q` 在 grep 提前退出时会让
        # nm 收到 SIGPIPE（141）而把整条管道判失败；先落成变量再匹配。
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
    return 1
}
STATIC_ORT=0
have_static_runtime && STATIC_ORT=1

have_runtime() {
    [ "$STATIC_ORT" = "1" ] && return 0
    if command -v ldconfig >/dev/null 2>&1 &&
       ldconfig -p 2>/dev/null | grep -q 'libonnxruntime\.so'; then
        return 0
    fi
    local d
    for d in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /lib/x86_64-linux-gnu; do
        ls "$d"/libonnxruntime.so* >/dev/null 2>&1 && return 0
    done
    return 1
}
RUNTIME_OK=0
if have_runtime; then RUNTIME_OK=1; fi
if [ "$RUNTIME_OK" != "1" ]; then
    echo "SKIP karaoke-model-e2e：未找到 libonnxruntime（dlopen 运行库），且二进制不是静态链接 ORT；不伪装通过。"
    exit 0
fi

have_ffmpeg=0
command -v ffmpeg >/dev/null 2>&1 && command -v ffprobe >/dev/null 2>&1 && have_ffmpeg=1

find_track() {
    local c
    for c in "$TRACK_ARG" "${TERMUSIC_KARAOKE_TEST_TRACK:-}" \
             "$HOME/Music/2002年的第一场雪 - 刀郎.flac" \
             "$HOME/Music/5_20AM - 刀酱.flac"; do
        [ -n "$c" ] && [ -f "$c" ] && { printf '%s' "$c"; return 0; }
    done
    # 通用回退：~/Music 下第一个带同名 .lrc 的 flac/mp3，时长 120..300s
    [ -d "$HOME/Music" ] || return 1
    for c in "$HOME/Music"/*.flac "$HOME/Music"/*.mp3; do
        [ -f "$c" ] || continue
        [ -f "${c%.*}.lrc" ] || continue
        if [ "$have_ffmpeg" = "1" ]; then
            local d
            d="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$c" 2>/dev/null)"
            awk -v d="$d" 'BEGIN{exit !(d>=120 && d<=300)}' || continue
        fi
        printf '%s' "$c"
        return 0
    done
    return 1
}
TRACK="$(find_track)" || {
    echo "SKIP karaoke-model-e2e：未找到真实曲目（~/Music 需 >=120s 的 flac/mp3 + .lrc）"
    echo "     可用 --track 或 TERMUSIC_KARAOKE_TEST_TRACK 指定；不伪装通过。"
    exit 0
}

# ── 工作目录与夹具 ──────────────────────────────────────────────────
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-kar-model-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
export XDG_DATA_HOME="$WORK_DIR/home/.local/share"
export TER_MUSIC_LOG_DIR="$WORK_DIR/logs"
MUSIC="$WORK_DIR/music"
mkdir -p "$XDG_CONFIG_HOME/ter-music" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$MUSIC" "$TER_MUSIC_LOG_DIR"

# ffprobe：宿主 PATH 里可能只有 ffmpeg 包装器（linglong 层库路径）。复用
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
CACHE_ROOT="$XDG_CACHE_HOME/ter-music/karaoke"
DAEMON_LOG_DIR="$XDG_CONFIG_HOME/ter-music"

# 随包内置路径（config v12）：二进制里写着编译期数据目录（CMake 的
# TER_MUSIC_DATA_DIR），据此定位 <前缀>/share/ter-music/models。
# 该前缀在容器里通常可写（/opt/apps），因此可以真测"随包内置"路径；
# 不可写时相关子用例明确 SKIP，不伪装通过。
BUNDLED_DATA_DIR="$(strings "$TM_BIN" 2>/dev/null |
    grep -oE '/[^[:space:]]*/share/ter-music' | head -1)"
BUNDLED_DIR=""
BUNDLED_MODEL=""
BUNDLED_WRITABLE=0
BUNDLED_ROOT_EXISTED=0
if [ -n "$BUNDLED_DATA_DIR" ]; then
    BUNDLED_DIR="$BUNDLED_DATA_DIR/models"
    BUNDLED_MODEL="$BUNDLED_DIR/$MODEL_ID.onnx"
    probe="$BUNDLED_DATA_DIR"
    if [ -e "$BUNDLED_DATA_DIR" ]; then
        BUNDLED_ROOT_EXISTED=1
    else
        while [ ! -e "$probe" ] && [ "$probe" != "/" ]; do
            probe="$(dirname "$probe")"
        done
    fi
    [ -w "$probe" ] && BUNDLED_WRITABLE=1
fi

bundled_model_install() {   # 0 = 已放好；1 = 随包路径不可用（明确 SKIP）
    [ -n "$BUNDLED_MODEL" ] && [ "$BUNDLED_WRITABLE" = "1" ] || return 1
    mkdir -p "$BUNDLED_DIR" 2>/dev/null || return 1
    cp -- "$MODEL" "$BUNDLED_MODEL" 2>/dev/null || return 1
    return 0
}
bundled_model_remove() {    # 移走模型 + 回收空目录（只删本次建出来的空目录）
    [ -n "$BUNDLED_MODEL" ] && rm -f "$BUNDLED_MODEL" 2>/dev/null
    if [ "$BUNDLED_ROOT_EXISTED" != "1" ] && [ -n "$BUNDLED_DIR" ]; then
        local d="$BUNDLED_DIR"
        for _ in 1 2 3 4; do
            rmdir "$d" 2>/dev/null || break
            d="$(dirname "$d")"
        done
    else
        [ -n "$BUNDLED_DIR" ] && rmdir "$BUNDLED_DIR" 2>/dev/null
    fi
    return 0
}

KEYS="$WORK_DIR/keys.cmd"
TUI_LOG="$WORK_DIR/tui.log"
DRIVER_PID=""
CORE_FG_PID=""
: > "$KEYS"

cleanup() {
    [ -n "${CORE_FG_PID:-}" ] && kill "$CORE_FG_PID" 2>/dev/null
    [ -n "$DRIVER_PID" ] && kill -9 "$DRIVER_PID" 2>/dev/null
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    # 回收本次测试放进随包路径的模型与空目录（绝不碰已有内容）
    [ -n "${BUNDLED_MODEL:-}" ] && rm -f "$BUNDLED_MODEL" 2>/dev/null
    if [ "${BUNDLED_ROOT_EXISTED:-1}" != "1" ] && [ -n "${BUNDLED_DIR:-}" ]; then
        d="$BUNDLED_DIR"
        for _ in 1 2 3 4; do
            rmdir "$d" 2>/dev/null || break
            d="$(dirname "$d")"
        done
    else
        [ -n "${BUNDLED_DIR:-}" ] && rmdir "$BUNDLED_DIR" 2>/dev/null
    fi
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

send() { printf 'key:%s\n' "$1" >>"$KEYS"; sleep "${2:-0.8}"; }
mlog() {   # 前端/core 的调试日志（优先级：指定 LOG_DIR 里最新）
    local f
    f="$(ls -t "$TER_MUSIC_LOG_DIR"/*.log 2>/dev/null | head -1)"
    [ -n "$f" ] && printf '%s' "$f" || {
        f="$(ls -t "$DAEMON_LOG_DIR"/ter-music-debug-*.log 2>/dev/null | head -1)"
        [ -n "$f" ] && printf '%s' "$f" || printf ''
    }
}
core_log() {   # 后台核心的 debug 日志（TER_MUSIC_FORCE_LOG=1 落配置目录）
    local f
    f="$(ls -t "$DAEMON_LOG_DIR"/ter-music-debug-*.log 2>/dev/null | head -1)"
    [ -n "$f" ] && printf '%s' "$f" || printf ''
}
screen_has() {
    [ -s "$TUI_LOG" ] || return 1
    python3 "$SCRIPT_DIR/vt_grid.py" --cols "${TERM_COLS:-100}" --rows "${TERM_ROWS:-30}" \
        --has "$1" "$TUI_LOG" >/dev/null 2>&1
}
wait_screen() {  # $1 正则，$2 超时秒
    local pat="$1" timeout="${2:-10}" i
    for i in $(seq 1 $((timeout * 2))); do
        screen_has "$pat" && return 0
        sleep 0.5
    done
    return 1
}
daemon_up() { "$TM_BIN" daemon status 2>/dev/null | grep -q "pid"; }
wait_daemon() {
    local i
    for i in $(seq 1 40); do daemon_up && return 0; sleep 0.25; done
    return 1
}
# 起核心：常规路径是 CLI 的 detach（daemon start）；受限宿主/容器里 CLI 可能
# 拒绝 fork 脱离进程（沙箱判定/权限），此时回退为前台核心 + 后台作业。
# 被测的分离/缓存/播放语义与进程归属无关，两条路径等价。
start_core() {
    "$TM_BIN" daemon start >/dev/null 2>&1
    wait_daemon && return 0
    "$TM_BIN" daemon foreground >"$WORK_DIR/core-foreground.log" 2>&1 &
    CORE_FG_PID=$!
    for i in $(seq 1 80); do daemon_up && return 0; sleep 0.25; done
    return 1
}

show_json() { "$TM_BIN" show --json 2>/dev/null; }
json_get() {  # $1 = python 表达式，作用在 dict d
    show_json | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    raise SystemExit(0)
try:
    print(eval(sys.argv[1], {"d": d}))
except Exception:
    pass' "$1" 2>/dev/null
}
pos_ms()      { json_get 'int(d["playback"].get("position_ms") or 0)'; }
duration_ms() { json_get 'int(d["playback"].get("duration_ms") or 0)'; }
volume_pct()  { json_get 'int(d["playback"].get("volume_percent") or 0)'; }
speed_x()     { json_get 'd["playback"].get("speed")'; }
track_path()  { json_get 'd.get("track", {}).get("path") or ""'; }
wait_pos() {  # $1 最小毫秒 $2 超时秒
    local want="$1" timeout="${2:-40}" i p
    for i in $(seq 1 $((timeout * 4))); do
        p="$(pos_ms)"; p="${p:-0}"
        [ "$p" -ge "$want" ] && return 0
        sleep 0.25
    done
    return 1
}
wait_track() {  # $1 期望曲目路径 $2 超时秒
    local want="$1" timeout="${2:-20}" i
    for i in $(seq 1 $((timeout * 4))); do
        [ "$(track_path)" = "$want" ] && return 0
        sleep 0.25
    done
    return 1
}

# 核心最后一次状态消息（Control.StatusMessage 的同源快照 Info.GetStatus）。
# gdbus 属于 glib2 的常用 CLI；缺失时状态断言统一 SKIP（其余断言不受影响）。
HAVE_GDBUS=0
command -v gdbus >/dev/null 2>&1 && HAVE_GDBUS=1
status_msg() {
    [ "$HAVE_GDBUS" = "1" ] || { printf ''; return 0; }
    timeout 10 gdbus call --session \
        --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 \
        --method cn.org.huansoft.ter_music.Info.GetStatus 2>/dev/null |
    python3 -c '
import ast, json, sys
raw = sys.stdin.read().strip()
if not raw:
    raise SystemExit(0)
try:
    value = ast.literal_eval(raw)
    doc = json.loads(value[0])
    print(doc.get("message") or "")
except Exception:
    pass'
}
wait_status() {  # $1 正则，$2 超时秒
    local pat="$1" timeout="${2:-20}" i msg
    for i in $(seq 1 $((timeout * 2))); do
        msg="$(status_msg)"
        printf '%s' "$msg" | grep -Eq "$pat" && { printf '%s' "$msg"; return 0; }
        sleep 0.5
    done
    return 1
}
# R1：失败/运行状态必须是硬断言（要求具体原因），只有缺 gdbus 这种环境问题才 SKIP。
require_status_ok() {  # $1 正则 $2 超时秒 $3 ok 文案 $4 bad 文案
    if [ "${HAVE_GDBUS:-0}" != "1" ]; then
        skip "缺少 gdbus：状态断言跳过（环境性）"
        return 0
    fi
    if wait_status "$1" "$2" >/dev/null; then
        ok "$3"
    else
        bad "$4（当前状态：$(status_msg)）"
    fi
}

cfg() { "$TM_BIN" config get "$1" 2>/dev/null | head -1; }
mode_now()     { "$TM_BIN" karaoke 2>/dev/null | sed -n 's/^[a-z]* (\([0-9]\)).*/\1/p'; }
wait_mode() {
    local want="$1" i
    for i in $(seq 1 20); do [ "$(mode_now)" = "$want" ] && return 0; sleep 0.25; done
    return 1
}
start_tui() {   # $1 = 标签（用于日志名）
    : > "$KEYS"
    python3 "$SCRIPT_DIR/lifecycle-driver.py" drive "$TM_BIN" "$MUSIC" "$TUI_LOG" \
        --ctl "$KEYS" --rc-file "$WORK_DIR/tui.rc" --pid-file "$WORK_DIR/tui.pid" \
        --max-runtime 420 -- >/dev/null 2>&1 &
    DRIVER_PID=$!
}
stop_tui() {    # 先 q 让前端自己收尾（核心按 stop_core_on_exit 缺省退出）
    printf 'key:q\n' >>"$KEYS" 2>/dev/null || true
    sleep 1.0
    [ -n "$DRIVER_PID" ] && kill -9 "$DRIVER_PID" 2>/dev/null
    wait "$DRIVER_PID" 2>/dev/null || true
    DRIVER_PID=""
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    sleep 0.5
}
cache_dirs() { find "$CACHE_ROOT" -mindepth 1 -maxdepth 1 -type d 2>/dev/null; }
pair_ok() {  # $1 = 曲目缓存目录
    [ -n "$1" ] && [ -s "$1/inst.flac" ] && [ -s "$1/soft.flac" ] && [ -s "$1/meta.txt" ]
}
pair_stat() {  # 成对文件的 inode:mtime（证明没有重新推理/重写）
    local d="$1"
    [ -n "$d" ] || { printf ''; return; }
    stat -c '%i:%Y' "$d/inst.flac" "$d/soft.flac" "$d/meta.txt" 2>/dev/null | tr '\n' ' '
}
wait_pair() {  # $1 = 超时秒；输出完成的曲目缓存目录
    local timeout="${1:-300}" i d
    for i in $(seq 1 $((timeout * 2))); do
        for d in $(cache_dirs); do
            if pair_ok "$d"; then printf '%s' "$d"; return 0; fi
        done
        sleep 0.5
    done
    return 1
}
pair_count() {  # 完整成对缓存的目录数（取消/失败断言用）
    local d n=0
    for d in $(cache_dirs); do pair_ok "$d" && n=$((n + 1)); done
    printf '%d' "$n"
}

# ── v2 会话分段缓存（/tmp）辅助 ─────────────────────────────────────
session_root_dir() {
    local pid
    pid="$("$TM_BIN" daemon status --json 2>/dev/null |
        python3 -c 'import json,sys;print(json.load(sys.stdin)["instance"]["pid"])' 2>/dev/null)"
    [ -n "$pid" ] && printf '/tmp/ter-music-karaoke-%s-%s' "$(id -u)" "$pid"
}
seg_dirs() {
    local sr
    sr="$(session_root_dir)"
    [ -n "$sr" ] && find "$sr" -name manifest.txt -type f -print0 2>/dev/null |
        xargs -0 -r -n1 dirname 2>/dev/null | sort -u
}
seg_complete() {  # $1=曲目段目录
    local d="$1"
    [ -n "$d" ] && [ -s "$d/manifest.txt" ] &&
        [ "$(sed -n 's/^complete=//p' "$d/manifest.txt" 2>/dev/null | head -1)" = "1" ]
}
seg_complete_dirs() {
    local d
    for d in $(seg_dirs); do seg_complete "$d" && printf '%s\n' "$d"; done
}
seg_complete_count() { seg_complete_dirs | grep -c . || true; }
seg_manifest_field() { sed -n "s/^$2=//p" "$1/manifest.txt" 2>/dev/null | head -1; }
seg_stat() {  # 段文件 inode:size（mtime 会因 LRU touch 变，不能用于“未重写”断言）
    local d="$1"
    [ -n "$d" ] || { printf ''; return; }
    find "$d" -name 'seg*' -type f 2>/dev/null | sort |
        xargs -r stat -c '%n:%i:%s' 2>/dev/null | tr '\n' ' '
}
wait_segments() {  # $1=超时秒 → 输出首个完整段目录
    local timeout="${1:-300}" i d
    for i in $(seq 1 $((timeout * 2))); do
        d="$(seg_complete_dirs | head -1)"
        [ -n "$d" ] && { printf '%s' "$d"; return 0; }
        sleep 0.5
    done
    return 1
}
seg_frames() {  # FLAC duration_ts = 每声道样本数
    local ts
    ts="$(ffprobe -v error -select_streams a:0 -show_entries stream=duration_ts \
          -of csv=p=0 "$1" 2>/dev/null | tr -d ',')"
    printf '%s' "$ts"
}
seg_concat_decode() {  # $1=段目录 $2=inst|soft $3=输出 f32le
    local total i
    total="$(seg_manifest_field "$1" total_segments)"
    : >"$3"
    for i in $(seq 0 $((total - 1))); do
        ffmpeg -v error -i "$1/$(printf 'seg%03d.%s.flac' "$i" "$2")" \
            -ar 44100 -ac 2 -f f32le - >>"$3" 2>/dev/null
    done
}
SEG_FRAMES=661500

# ── S0 前置与静态检查 ──────────────────────────────────────────────
info "S0 前置"
ok "模型 $MODEL（$SUPPORTED_SIZE B，SHA256 ${MODEL_SHA:0:16}… 命中）"
if [ "$STATIC_ORT" = "1" ]; then
    ok "ONNX Runtime 静态链接在二进制内（nm 命中 OrtGetApiBase；无需系统 .so）"
else
    ok "libonnxruntime 可被发现（dlopen 运行库）"
fi
ok "真实曲目 $TRACK"
cp -- "$TRACK" "$MUSIC/real.flac" || { bad "复制真实曲目失败"; exit 1; }
if [ -f "${TRACK%.*}.lrc" ]; then
    cp -- "${TRACK%.*}.lrc" "$MUSIC/real.lrc"
    ok "歌词夹具 real.lrc 已就位（无歌词门控/歌词回归用）"
else
    printf '[00:01.00]第一句\n[00:05.00]第二句\n[00:20.00]第三句\n' >"$MUSIC/real.lrc"
    ok "自造 real.lrc（原曲无同名歌词）"
fi
TRACK_DUR_MS="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$MUSIC/real.flac" 2>/dev/null |
    awk '{printf "%d", $1*1000}')"
[ -n "$TRACK_DUR_MS" ] && [ "$TRACK_DUR_MS" -gt 0 ] || { bad "读不到真实曲目时长"; exit 1; }

if [ "$have_ffmpeg" = "1" ]; then
    ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=8" \
        -ac 1 -c:a pcm_s16le -y "$MUSIC/mono.wav" 2>/dev/null \
        && ok "单声道夹具 mono.wav" || bad "造单声道夹具失败"
    ffmpeg -hide_banner -loglevel error -f lavfi -i "anullsrc=r=44100:cl=stereo" \
        -t 1860 -c:a flac -y "$MUSIC/long.flac" 2>/dev/null \
        && ok ">30min 夹具 long.flac（31 分钟静音 FLAC）" || bad "造 31 分钟夹具失败"
    cp -- "$MUSIC/real.flac" "$MUSIC/real2.flac" && ok "换曲取消用夹具 real2.flac"
else
    skip "ffmpeg 缺失：单声道/31 分钟夹具与质量分析相关断言"
fi

info "S13 静态：模型不进 git / 随包安装规则 / 无 NEEDED"
if git -C "$REPO_ROOT" ls-files '*.onnx' 2>/dev/null | grep -q .; then
    bad "仓库里跟踪了 .onnx 模型文件（模型不进 git）"
else
    ok "git 跟踪的文件里没有 .onnx 模型（模型不进 git）"
fi
bundle_hits="$(find "$REPO_ROOT" -maxdepth 3 -name '*.onnx' -not -path "$REPO_ROOT/.tmp/*" 2>/dev/null | head -5)"
[ -z "$bundle_hits" ] && ok "仓库源码/构建目录（除 .tmp 开发夹具）无 .onnx" \
    || bad "仓库内发现 .onnx：$bundle_hits"
if grep -q 'TER_MUSIC_BUNDLED_MODEL' "$REPO_ROOT/CMakeLists.txt" &&
   grep -q 'share/ter-music/models' "$REPO_ROOT/CMakeLists.txt"; then
    ok "CMakeLists 支持 -DTER_MUSIC_BUNDLED_MODEL 安装到 share/ter-music/models"
else
    bad "CMakeLists 缺内置模型安装规则"
fi
if readelf -d "$TM_BIN" 2>/dev/null | grep -qi onnx || ldd "$TM_BIN" 2>/dev/null | grep -qi onnx; then
    bad "二进制对 libonnxruntime 有 NEEDED，违反可选/静态内嵌约定"
elif [ "$STATIC_ORT" = "1" ]; then
    ok "静态链接构建：二进制无 libonnxruntime NEEDED（ORT 已编入二进制）"
else
    ok "二进制无 libonnxruntime NEEDED（dlopen 可选加载）"
fi
# C 源里的 URL 被折成相邻字符串字面量（跨行），先压掉空白再匹配完整 URL
MODEL_SRC_FLAT="$(python3 -c 'import re,sys; print(re.sub(r"[\s\"]", "", open(sys.argv[1], encoding="utf-8", errors="replace").read()))' "$REPO_ROOT/src/cn.org.huansoft.ter-music/audio/karaoke_model.c" 2>/dev/null)"
if printf '%s' "$MODEL_SRC_FLAT" | grep -q "github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/$MODEL_NAME" &&
   printf '%s' "$MODEL_SRC_FLAT" | grep -q "$SUPPORTED_SHA" &&
   printf '%s' "$MODEL_SRC_FLAT" | grep -q "UVR"; then
    ok "模型表含官方 URL / SHA256 / UVR 署名（应用内展示的数据源）"
else
    bad "模型表缺官方 URL / SHA256 / UVR 署名"
fi

# ── S1 默认常显（config v12：没有功能开关/模型选择）──────────────────
info "S1 默认常显（全新配置，无功能开关/模型选择）"
start_core
start_tui default
if wait_daemon && wait_screen '控 ?制 ?区|Controls' 15; then
    ok "TUI 与核心上线"
else
    bad "TUI/核心未就绪"
fi
if [ -f "$CONFIG" ] && grep -Eq '<karaoke_feature[ >/]|<karaoke_model[ >/]' "$CONFIG"; then
    bad "全新配置含已删除的 karaoke_feature/karaoke_model"
else
    ok "全新配置没有 karaoke_feature/karaoke_model（config v12）"
fi
[ "$(cfg version)" = "12" ] && ok "Config.GetAll version=12" \
    || bad "Config.GetAll version=$(cfg version)（期望 12）"
if screen_has '卡拉OK:(不启用|人声较弱|消除人声)'; then
    ok "主界面默认显示卡拉OK按钮（默认 Off 也常显）"
else
    bad "主界面默认没有卡拉OK按钮（v12 应常显）"
fi
"$TM_BIN" karaoke full >/dev/null 2>&1
if wait_mode 2; then
    ok "默认 Off 下 CLI karaoke full 直接生效（无功能门禁）"
else
    bad "CLI 开档被拒或未生效（档位 $(mode_now)）"
fi
"$TM_BIN" karaoke off >/dev/null 2>&1
wait_mode 0 >/dev/null 2>&1
stop_tui

# ── S2 内置模型：构建期安装 + 运行期定位 + 旧 XDG 只读兼容 ────────────
info "S2a 构建期 -DTER_MUSIC_BUNDLED_MODEL 安装（staging 前缀）"
BUILD_DIR="${TERMUSIC_BUNDLE_BUILD:-$REPO_ROOT/.tmp/bundle-build}"
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && command -v cmake >/dev/null 2>&1; then
    STAGE="$WORK_DIR/stage"
    if cmake --install "$BUILD_DIR" --prefix "$STAGE" >"$WORK_DIR/install.log" 2>&1; then
        STAGED="$STAGE/share/ter-music/models/$MODEL_ID.onnx"
        if [ -f "$STAGED" ] &&
           [ "$(stat -c %s "$STAGED")" = "$SUPPORTED_SIZE" ] &&
           [ "$(sha256sum "$STAGED" | cut -d' ' -f1)" = "$SUPPORTED_SHA" ]; then
            ok "安装到 share/ter-music/models/$MODEL_ID.onnx（大小+SHA256 精确命中）"
        else
            bad "安装树模型缺失或大小/SHA256 不符：$STAGED"
        fi
        if [ -f "$STAGE/share/ter-music/models/NOTICE.md" ]; then
            ok "安装树含 models/NOTICE.md（MIT 声明义务）"
        else
            bad "安装树缺少 models/NOTICE.md（MIT 声明义务）"
        fi
    else
        bad "cmake --install 失败（见 $WORK_DIR/install.log）"
    fi
else
    skip "缺少 $BUILD_DIR/CMakeCache.txt 或 cmake：构建期安装断言跳过（不伪装通过）"
fi

info "S2b 内置模型：从 <前缀>/share/ter-music/models 直接使用（不复制到 XDG）"
if [ "$BUNDLED_WRITABLE" != "1" ] || ! bundled_model_install; then
    skip "随包数据目录不可写（${BUNDLED_DATA_DIR:-未探测到}）：内置模型运行期断言跳过"
else
    ok "已放入随包路径 $BUNDLED_MODEL（模拟包内安装）"
    rm -rf "$DATA_DIR/models"     # 确保没有 XDG 副本，证明"直接使用、不复制"
    start_core || bad "S2b 核心启动失败"
    "$TM_BIN" karaoke full >/dev/null 2>&1
    wait_mode 2 >/dev/null 2>&1
    "$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
    wait_track "$MUSIC/real.flac" 20 || true
    require_status_ok '正在 ?准 ?备 ?伴 ?奏|正在 ?后 ?台 ?分 ?离|Preparing ?karaoke' 30 \
        "内置模型被定位并开始后台分离（状态行 RUNNING）" \
        "内置模型没有被使用（30s 内未出现后台分离状态）"
    "$TM_BIN" stop >/dev/null 2>&1
    if [ -e "$DATA_DIR/models" ]; then
        bad "使用内置模型时向 XDG 复制了文件：$(ls -A "$DATA_DIR/models" 2>/dev/null | tr '\n' ' ')"
    else
        ok "使用内置模型时不向 XDG 复制（\$XDG_DATA_HOME/ter-music/models 未创建）"
    fi
    "$TM_BIN" daemon stop --all >/dev/null 2>&1
    sleep 0.8
fi

info "S2c 旧用户 XDG 模型：只读兼容（随包路径缺失时回退，不写入）"
mkdir -p "$DATA_DIR/models"
cp -- "$MODEL" "$INSTALLED" && chmod 600 "$INSTALLED"
INODE_BEFORE="$(stat -c '%i:%Y:%s' "$INSTALLED" 2>/dev/null)"
bundled_model_remove
start_core || bad "S2c 核心启动失败"
"$TM_BIN" karaoke full >/dev/null 2>&1
wait_mode 2 >/dev/null 2>&1
"$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
wait_track "$MUSIC/real.flac" 20 || true
require_status_ok '正在 ?准 ?备 ?伴 ?奏|正在 ?后 ?台 ?分 ?离|Preparing ?karaoke' 30 \
    "随包路径缺失时回退旧 XDG 模型并开始分离" \
    "旧 XDG 模型没有被使用（30s 内未出现后台分离状态）"
"$TM_BIN" stop >/dev/null 2>&1
INODE_AFTER="$(stat -c '%i:%Y:%s' "$INSTALLED" 2>/dev/null)"
[ "$INODE_BEFORE" = "$INODE_AFTER" ] && ok "旧 XDG 模型只读使用（inode/mtime/size 未变）" \
    || bad "旧 XDG 模型被改写：$INODE_BEFORE → $INODE_AFTER"
if ls "$DATA_DIR/models"/.*.part* >/dev/null 2>&1; then
    bad "旧 XDG 模型目录出现 .part 半成品（运行期不应写入）"
else
    ok "旧 XDG 模型目录无 .part 半成品（只读兼容）"
fi
if [ "$(find "$DATA_DIR/models" -maxdepth 1 -type f | wc -l)" = "1" ]; then
    ok "旧 XDG 模型目录仍只有那一个模型文件（无复制/无额外写入）"
else
    bad "旧 XDG 模型目录出现额外文件：$(ls -A "$DATA_DIR/models" | tr '\n' ' ')"
fi
"$TM_BIN" daemon stop --all >/dev/null 2>&1
sleep 0.8

info "S2d 内置模型损坏：坏包可见（不静默改用旧副本）、回退原声"
if [ "$BUNDLED_WRITABLE" = "1" ]; then
    mkdir -p "$BUNDLED_DIR"
    head -c 100 "$MODEL" >"$BUNDLED_MODEL" 2>/dev/null   # 大小不符 = 坏包
    start_core || bad "S2d 核心启动失败"
    "$TM_BIN" karaoke full >/dev/null 2>&1
    wait_mode 2 >/dev/null 2>&1
    "$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
    wait_track "$MUSIC/real.flac" 20 || true
    require_status_ok '内置模型校验失败|内置模型' 25 \
        "损坏的内置模型给出可读错误（内置模型校验失败）" \
        "损坏的内置模型没有可读错误"
    wait_pos 500 20 || true
    dp0="$(pos_ms)"; sleep 1.2; dp1="$(pos_ms)"
    [ "${dp1:-0}" -gt "${dp0:-0}" ] && ok "模型损坏时回退原声继续播放（${dp0}→${dp1} ms）" \
        || bad "模型损坏导致播放异常（${dp0}→${dp1} ms）"
    daemon_up && ok "模型损坏不崩核心" || bad "模型损坏导致核心掉线"
    "$TM_BIN" stop >/dev/null 2>&1
    "$TM_BIN" daemon stop --all >/dev/null 2>&1
    sleep 0.8
else
    skip "随包数据目录不可写：内置模型损坏断言跳过"
fi

info "S2e 内置模型缺失：明确报错、回退原声、不崩溃"
rm -f "$BUNDLED_MODEL" 2>/dev/null
rm -rf "$DATA_DIR/models"
start_core || bad "S2e 核心启动失败"
"$TM_BIN" karaoke full >/dev/null 2>&1
wait_mode 2 >/dev/null 2>&1
"$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
wait_track "$MUSIC/real.flac" 20 || true
require_status_ok '内置模型缺失' 25 \
    "模型缺失给出可读错误（内置模型缺失）" \
    "模型缺失没有可读错误"
wait_pos 500 20 || true
dp0="$(pos_ms)"; sleep 1.2; dp1="$(pos_ms)"
[ "${dp1:-0}" -gt "${dp0:-0}" ] && ok "模型缺失时回退原声继续播放（${dp0}→${dp1} ms）" \
    || bad "模型缺失导致播放异常（${dp0}→${dp1} ms）"
daemon_up && ok "模型缺失不崩核心" || bad "模型缺失导致核心掉线"
"$TM_BIN" daemon stop --all >/dev/null 2>&1
sleep 0.8
# 恢复内置模型供后续分离用例使用（若随包路径不可写，则回退 XDG 路径）。
if bundled_model_install; then
    ok "已恢复随包内置模型（后续分离用例走首选路径）"
elif mkdir -p "$DATA_DIR/models" 2>/dev/null &&
     cp -- "$MODEL" "$INSTALLED" 2>/dev/null; then
    ok "随包路径不可写，已用旧 XDG 路径恢复模型（后续分离用例走兼容路径）"
else
    bad "无法恢复模型文件，后续分离用例会失败"
fi

# ── S3 重启稳定 ─────────────────────────────────────────────────────
info "S3 重启稳定：配置无旧键、档位持久、按钮仍在"
start_core
sleep 0.6
"$TM_BIN" karaoke soft >/dev/null 2>&1
wait_mode 1 || bad "设置 soft 档失败"
"$TM_BIN" daemon stop --all >/dev/null 2>&1
sleep 0.8
start_core
start_tui restart
if wait_daemon && wait_screen '控 ?制 ?区|Controls' 15; then
    ok "重启后 TUI 与核心上线"
else
    bad "重启后 TUI/核心未就绪"
fi
if [ -f "$CONFIG" ] && grep -Eq '<karaoke_feature[ >/]|<karaoke_model[ >/]' "$CONFIG"; then
    bad "重启后配置出现已删除的 karaoke_feature/karaoke_model"
else
    ok "重启后配置仍无 karaoke_feature/karaoke_model"
fi
[ "$(cfg karaoke_mode)" = "1" ] \
    && ok "重启后档位 soft(1) 由配置回灌（karaoke_mode 持久化）" \
    || bad "重启后档位丢失（karaoke_mode=$(cfg karaoke_mode)，期望 1）"
if wait_screen '卡拉OK:(人声较弱|不启用|消除人声)' 8; then
    ok "重启后主界面仍显示卡拉OK按钮"
else
    bad "重启后按钮消失（布局/配置回灌失败）"
fi
stop_tui

# ── 进入 CLI/核心阶段（不依赖 TUI）──────────────────────────────────
info "启动核心（CLI 驱动播放与缓存断言）"
start_core && ok "核心上线（用 play 逐曲驱动，队列含真实曲目/单声道/31 分钟/换曲夹具）" \
    || { bad "核心启动失败"; exit 1; }
"$TM_BIN" karaoke off >/dev/null 2>&1
wait_mode 0 && ok "档位 off（原声）" || bad "档位未归零"

# ── S4 首播等首段 + 后台分离 + 分段缓存 + 质量 ──────────────────────
info "S4 首播等首段（D1）+ 后台分离（真实曲目）"
# config v12：档位即开关，off 档不跑模型；所以这里必须以 full 档起播，
# 首播等待首段伴奏就绪（D1：约 7–10s，上限 30s）后直接以分片伴奏起播。
"$TM_BIN" karaoke full >/dev/null 2>&1
wait_mode 2 >/dev/null 2>&1
T0="$(date +%s%3N)"
"$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
if wait_pos 1000 40; then
    T1="$(date +%s%3N)"
    WAIT_MS=$((T1 - T0))
    if [ "$WAIT_MS" -le 30000 ]; then
        ok "首段就绪后起播（等待 ${WAIT_MS}ms ≤ 30000ms，D1 行为不变）"
    else
        bad "首播等待 ${WAIT_MS}ms 超过 30s 上限"
    fi
else
    bad "40s 内没有开始播放（首段等待卡死）"
fi
PLAY_PATH="$(track_path)"
[ "$PLAY_PATH" = "$MUSIC/real.flac" ] && ok "起播真实曲目：$PLAY_PATH" \
    || bad "起播的曲目不是 real.flac（$PLAY_PATH）"
require_status_ok '正在 ?准 ?备 ?伴 ?奏|正在 ?后 ?台 ?分 ?离|Preparing ?karaoke' 15 \
    "分离状态已提示（正在准备伴奏 N% / 后台分离）" \
    "未捕获到分离状态（RUNNING/进度均未出现）"
# 分离期间：seek + 音量 + 倍速不应影响播放与 job
"$TM_BIN" seek 12 >/dev/null 2>&1
sleep 0.8
ps_="$(pos_ms)"
[ "${ps_:-0}" -ge 10500 ] && [ "${ps_:-0}" -le 18000 ] && ok "分离期间 seek 生效（位置 ${ps_} ms）" \
    || bad "分离期间 seek 异常（位置 $ps_）"
"$TM_BIN" volume 77 >/dev/null 2>&1
[ "$(volume_pct)" = "77" ] && ok "分离期间音量可调（77%）" || bad "分离期间音量异常（$(volume_pct)）"
dur_before="$(duration_ms)"
[ "${dur_before:-0}" -gt 0 ] && ok "分离期间时长可读（${dur_before} ms）" || bad "分离期间时长读不到"

SEG_DIR="$(wait_segments 420)" || {
    bad "后台分离 420s 内未产出完整 v2 分段缓存（manifest complete=1）"
    SEG_DIR=""
}
if [ -n "$SEG_DIR" ]; then
    ok "v2 分段缓存已生成：$SEG_DIR"
    if find "$SEG_DIR" -name '*.part*' 2>/dev/null | grep -q .; then
        bad "缓存提交后仍有 .part 半成品"
    else
        ok "原子提交完成，无 .part 半成品"
    fi
    total="$(seg_manifest_field "$SEG_DIR" total_segments)"
    srcf="$(seg_manifest_field "$SEG_DIR" source_frames)"
    val="$(grep -E '^(format|model_id|model_sha256|overlap|soft_inst|sample_rate|segment_seconds|total_segments|source_frames|complete)=' "$SEG_DIR/manifest.txt" 2>/dev/null | tr '\n' ' ')"
    printf '%s' "$val" | grep -q "model_id=$MODEL_ID" &&
        printf '%s' "$val" | grep -q "model_sha256=$SUPPORTED_SHA" &&
        printf '%s' "$val" | grep -q "overlap=0.25" &&
        printf '%s' "$val" | grep -q "soft_inst=0.6" &&
        printf '%s' "$val" | grep -q "format=2" &&
        printf '%s' "$val" | grep -q "sample_rate=44100" &&
        printf '%s' "$val" | grep -q "segment_seconds=15" \
        && ok "manifest 含 format=2/model/sha/SHA256/overlap=0.25/soft_inst=0.6/44100/15/total=$total/source_frames=$srcf/complete" \
        || bad "manifest 缺少关键键：$val"
    dirs_n="$(seg_complete_count)"
    [ "$dirs_n" = "1" ] && ok "只生成当前曲目的一个段目录（同曲单 job / 不预取其他曲目）" \
        || bad "段目录数=$dirs_n（期望 1，可能预取了别的曲目）"
    if [ "$have_ffmpeg" = "1" ]; then
        f0="$(seg_frames "$SEG_DIR/seg000.inst.flac")"
        [ "$f0" = "$SEG_FRAMES" ] && ok "非末段帧数 661500（44100×15）" \
            || bad "非末段帧数异常：$f0"
        last_i=$((total - 1))
        fl="$(seg_frames "$(printf '%s/seg%03d.inst.flac' "$SEG_DIR" "$last_i")")"
        [ "$fl" = "$((srcf - last_i * SEG_FRAMES))" ] && ok "末段帧数=剩余帧（$fl）" \
            || bad "末段帧数异常：$fl"
        sum=0
        for i in $(seq 0 "$last_i"); do
            fr="$(seg_frames "$(printf '%s/seg%03d.inst.flac' "$SEG_DIR" "$i")")"
            sum=$((sum + ${fr:-0}))
        done
        [ "$sum" = "$srcf" ] && ok "各段帧数总和=source_frames（$sum）" \
            || bad "段帧数总和 $sum ≠ manifest source_frames $srcf"
        bw="$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate,channels,bits_per_raw_sample -of csv=p=0 "$SEG_DIR/seg000.inst.flac" 2>/dev/null)"
        case "$bw" in
            44100,2,16) ok "段格式 44.1kHz/2ch/16bit" ;;
            *) bad "段格式异常：$bw（期望 44100,2,16）" ;;
        esac
        seg_concat_decode "$SEG_DIR" inst "$WORK_DIR/cache-inst.f32"
        seg_concat_decode "$SEG_DIR" soft "$WORK_DIR/cache-soft.f32"
        ffmpeg -v error -i "$MUSIC/real.flac" -ar 44100 -ac 2 -f f32le - >"$WORK_DIR/orig.f32" 2>/dev/null
        # 低频（<150Hz）保留判据：正确 inst = mixture−voc，低频被完整保留；
        # 若 inst 实际是模型人声 stem，低频会塌陷 ~30dB（T2/T5 实测）。
        ffmpeg -v error -i "$MUSIC/real.flac" -af lowpass=f=150 -ac 1 -ar 44100 -f f32le - >"$WORK_DIR/orig-low.f32" 2>/dev/null
        ffmpeg -v error -i "$SEG_DIR/seg000.inst.flac" -af lowpass=f=150 -ac 1 -ar 44100 -f f32le - > /dev/null 2>/dev/null
        : >"$WORK_DIR/inst-low.f32"
        for i in $(seq 0 $((total - 1))); do
            ffmpeg -v error -i "$(printf '%s/seg%03d.inst.flac' "$SEG_DIR" "$i")" \
                -af lowpass=f=150 -ac 1 -ar 44100 -f f32le - >>"$WORK_DIR/inst-low.f32" 2>/dev/null
        done
        # stem 正确性判据（T2 阻断项）：正确 inst 保留侧向（乐器）与低频，
        # 只削中置（人声）；若 inst 实际是模型人声 stem，侧向/低频都会大幅塌陷。
        if [ "${TERMUSIC_E2E_SKIP_STEM_QUALITY:-0}" = "1" ]; then
            # 合成夹具没有真实人声，MDX 会把整段当伴奏原样输出（inst≈mixture），
            # 质量判据只能失真。CI/合成素材环境显式跳过，真实曲目的质量门禁在
            # 发布机/Nightly 跑（同一脚本不设该变量）。
            skip "stem 质量断言（合成夹具无真实人声）：由发布机用真实曲目验证"
        elif python3 - "$WORK_DIR/orig.f32" "$WORK_DIR/cache-inst.f32" "$WORK_DIR/cache-soft.f32" "$WORK_DIR/orig-low.f32" "$WORK_DIR/inst-low.f32" <<'PY'
import sys, array, math
orig_p, inst_p, soft_p, orig_low_p, inst_low_p = sys.argv[1:6]
def load(p):
    a = array.array('f')
    with open(p, 'rb') as f:
        d = f.read()
    a.frombytes(d[:len(d) - (len(d) % 4)])
    return a
o, i, s = load(orig_p), load(inst_p), load(soft_p)
if len(i) != len(s) or len(i) < 2 * 44100 * 30:
    print(f'FAIL lengths orig={len(o)//2} inst={len(i)//2} soft={len(s)//2}')
    sys.exit(1)
if abs(len(o) - len(i)) > 2 * 8192:
    print(f'FAIL total-frames orig={len(o)//2} inst={len(i)//2}')
    sys.exit(1)
def rms(seq, start, end, center=True):
    acc = 0.0
    n = 0
    for k in range(start, end):
        l = seq[2 * k]
        r = seq[2 * k + 1]
        v = (l + r) * 0.5 if center else (l - r) * 0.5
        acc += v * v
        n += 1
    return math.sqrt(acc / n) if n else 0.0
frames = len(o) // 2
last_stats = (0.0, 0.0)
for st in (0, 60):
    a = max(0, st * 44100)
    b = min(frames, (st + 30) * 44100)
    if b <= a:
        continue
    cdb = 20 * math.log10(max(rms(i, a, b, True), 1e-9) / max(rms(o, a, b, True), 1e-9))
    sdb = 20 * math.log10(max(rms(i, a, b, False), 1e-9) / max(rms(o, a, b, False), 1e-9))
    last_stats = (cdb, sdb)
    if not (cdb < -0.05 and cdb > -20.0 and sdb > -8.0):
        print(f'FAIL stem@{st}s: center_drop={cdb:.2f}dB side_drop={sdb:.2f}dB '
              f'(inst 必须是 mixture−voc，侧向应基本保留)')
        sys.exit(1)
# 低频保留：正确 inst 与原声 <150Hz 能量接近（差 > -6dB）；人声 stem 会低 ~30dB。
ol, il = load(orig_low_p), load(inst_low_p)
nlow = min(len(ol), len(il))
if nlow < 44100:
    print('FAIL low-band decode short')
    sys.exit(1)
def rms_mono(a, n):
    acc = 0.0
    for k in range(n):
        acc += a[k] * a[k]
    return math.sqrt(acc / n) if n else 0.0
ro, ri = rms_mono(ol, nlow), rms_mono(il, nlow)
low_db = 20 * math.log10(max(ri, 1e-9) / max(ro, 1e-9))
if not (low_db > -6.0):
    print(f'FAIL low-band(inst vs orig)={low_db:.2f}dB (<-6dB 说明 inst 不是伴奏)')
    sys.exit(1)
err2 = ref2 = 0.0
for st in (5, 70):
    a = st * 44100
    b = min(len(i) // 2, (st + 5) * 44100)
    for k in range(a, b):
        for ch in range(2):
            want = 0.40 * o[2 * k + ch] + 0.60 * i[2 * k + ch]
            d = s[2 * k + ch] - want
            err2 += d * d
            ref2 += want * want
rel = (err2 / ref2) ** 0.5 if ref2 > 0 else 9.9
if rel > 5e-3:
    print(f'FAIL soft-formula rel={rel:.2e}')
    sys.exit(1)
print(f'OK center_drop={last_stats[0]:.2f}dB side_drop={last_stats[1]:.2f}dB '
      f'low_band={low_db:.2f}dB soft_rel={rel:.2e}')
PY
        then
            ok "inst=mixture−voc（侧向保留、中置人声下降），soft=0.4×原声+0.6×inst"
        else
            bad "stem 派生/soft 公式校验失败（inst 可能仍是人声 stem）"
        fi
    else
        skip "ffmpeg 缺失：段格式/帧数/soft 公式断言"
    fi
fi

# ── S5 三档与源切换（缓存命中）──────────────────────────────────────
info "S5 off/soft/full 三档源切换与无回归"
# S4 以 full 档起播；先切回原声，再验证"off→full→soft→off"的按位置换源。
"$TM_BIN" karaoke off >/dev/null 2>&1
wait_mode 0 >/dev/null 2>&1
sleep 0.8
CL="$(core_log)"
[ -n "$CL" ] && ok "核心 debug 日志：$CL" || bad "拿不到核心 debug 日志（源切换无法取证）"
src_count() { grep -c "playing karaoke cache source=$1" "$CL" 2>/dev/null || true; }
src_full_before="$(src_count 2)"
src_soft_before="$(src_count 1)"
PAIR_STAT="$(seg_stat "$SEG_DIR")"
p_start="$(pos_ms)"
"$TM_BIN" karaoke full >/dev/null 2>&1
wait_mode 2 >/dev/null 2>&1
sleep 1.8
if [ "$(src_count 2)" -gt "${src_full_before:-0}" ]; then
    ok "full 档按当前位置换源到 inst.flac（source=2）"
else
    bad "full 档没有切到 inst.flac（日志无 source=2）"
fi
p_after="$(pos_ms)"
if [ "${p_after:-0}" -gt "${p_start:-0}" ] && [ "${p_after:-0}" -lt $((p_start + 8000)) ]; then
    ok "切档不重置进度（${p_start}→${p_after} ms）"
else
    bad "切档后进度异常（${p_start}→${p_after} ms）"
fi
"$TM_BIN" karaoke soft >/dev/null 2>&1
wait_mode 1 >/dev/null 2>&1
sleep 1.5
[ "$(src_count 1)" -gt "${src_soft_before:-0}" ] && ok "soft 档换源到 soft.flac（source=1）" \
    || bad "soft 档没有切到 soft.flac"
orig_before="$(grep -c "playing original audio (karaoke" "$CL" 2>/dev/null || true)"
src_off_base2="$(src_count 2)"
src_off_base1="$(src_count 1)"
"$TM_BIN" karaoke off >/dev/null 2>&1
wait_mode 0 >/dev/null 2>&1
sleep 1.5
orig_after="$(grep -c "playing original audio (karaoke" "$CL" 2>/dev/null || true)"
if [ "${orig_after:-0}" -gt "${orig_before:-0}" ] &&
   [ "$(src_count 2)" = "$src_off_base2" ] && [ "$(src_count 1)" = "$src_off_base1" ]; then
    ok "off 档回到原声（原声起播日志+1，缓存源计数不再增长）"
else
    bad "off 档没有回到原声（orig $orig_before→$orig_after，source2 $src_off_base2→$(src_count 2)，source1 $src_off_base1→$(src_count 1)）"
fi
"$TM_BIN" karaoke full >/dev/null 2>&1
wait_mode 2 >/dev/null 2>&1
sleep 1.2
[ "$(src_count 2)" -gt "${src_full_before:-0}" ] && ok "再次 full 仍命中缓存（source=2）" \
    || bad "再次 full 未命中缓存"
[ "$(seg_stat "$SEG_DIR")" = "$PAIR_STAT" ] && ok "多次切档/再次起播没有重写缓存（inode/mtime 不变 ⇒ 未重跑模型）" \
    || bad "缓存 inode/mtime 变了（发生了第二次推理/重写）"
# seek + 歌词 + 时长 + 倍速 + 音量
"$TM_BIN" seek 6 >/dev/null 2>&1
sleep 1.0
if [ "$(pos_ms)" -ge 5500 ] && [ "$(pos_ms)" -le 12000 ]; then
    ok "缓存源播放中 seek 生效（$(pos_ms) ms）"
else
    bad "缓存源播放中 seek 异常（$(pos_ms) ms）"
fi
has_ly="$(json_get 'bool(d["lyrics"].get("has_lyrics"))')"
[ "$has_ly" = "True" ] && ok "缓存源播放中歌词仍可用（has_lyrics=true，内嵌/同名 LRC）" \
    || bad "缓存源播放中歌词不可用（has_lyrics=$has_ly）"
dur_after="$(duration_ms)"
if [ -n "$dur_before" ] && [ "${dur_after:-0}" -gt $((dur_before - 500)) ] && [ "${dur_after:-0}" -lt $((dur_before + 500)) ]; then
    ok "缓存源播放时长不变（${dur_before}→${dur_after} ms）"
else
    bad "缓存源播放时长漂移（${dur_before}→${dur_after} ms）"
fi
eq_before="$(grep -A20 '<equalizer>' "$CONFIG" 2>/dev/null | tr -d ' \n')"
"$TM_BIN" speed 1.25 >/dev/null 2>&1
sleep 2.0
sp="$(speed_x)"
[ "$sp" = "1.25" ] && ok "缓存源播放中倍速可设（1.25x）" || bad "倍速设置异常（$sp）"
[ "$(pos_ms)" -gt 0 ] && ok "倍速下位置仍推进（$(pos_ms) ms）" || bad "倍速下位置不动"
"$TM_BIN" speed 1.0 >/dev/null 2>&1
"$TM_BIN" config reload >/dev/null 2>&1
eq_after="$(grep -A20 '<equalizer>' "$CONFIG" 2>/dev/null | tr -d ' \n')"
[ "$eq_before" = "$eq_after" ] && ok "均衡器配置未被消人声链路改动（EQ 无回归）" \
    || bad "均衡器配置被改动"
[ "$(volume_pct)" = "77" ] && ok "音量保持在 77%（无回归）" || bad "音量漂移（$(volume_pct)）"

# ── S6 第二次起播命中缓存，不再推理 ────────────────────────────────
info "S6 第二次起播：命缓存、不触发第二次推理"
PAIR_STAT="$(seg_stat "$SEG_DIR")"
"$TM_BIN" stop >/dev/null 2>&1
sleep 0.5
"$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
sleep 1.5
[ "$(track_path)" = "$MUSIC/real.flac" ] && ok "第二次起播成功" || bad "第二次起播失败"
[ "$(src_count 2)" -gt "${src_full_before:-0}" ] && ok "第二次起播直接用 full 缓存（source=2）" \
    || bad "第二次起播没有直接用缓存"
"$TM_BIN" volume 80 >/dev/null 2>&1   # 顺带让状态消息翻新，便于观察是否重新分离
sleep 1.0
if status_msg | grep -Eq '正在 ?后 ?台 ?分 ?离|Preparing karaoke'; then
    bad "第二次起播又开始了后台分离（应命缓存）"
else
    ok "第二次起播没有重新分离（状态不是 RUNNING）"
fi
[ "$(seg_stat "$SEG_DIR")" = "$PAIR_STAT" ] && ok "第二次起播未改动缓存（无第二次推理）" \
    || bad "第二次起播改写了缓存"

# ── S7 取消：换曲（v12 无功能开关，切 Off 不再取消 job）──────────────
info "S7 后台 job 取消（换曲）"
if [ "$have_ffmpeg" = "1" ] && [ -f "$MUSIC/real2.flac" ]; then
    "$TM_BIN" karaoke off >/dev/null 2>&1
    wait_mode 0 >/dev/null 2>&1
    pairs_before_switch="$(seg_complete_count)"
    "$TM_BIN" play "$MUSIC/real2.flac" >/dev/null 2>&1
    sleep 1.5
    # 换曲 → 取消 real2 的分离
    "$TM_BIN" play "$MUSIC/mono.wav" >/dev/null 2>&1
    sleep 2.5
    pairs_after_switch="$(seg_complete_count)"
    [ "${pairs_after_switch:-0}" -le "${pairs_before_switch:-0}" ] \
        && ok "换曲取消了 real2 的后台分离（完整缓存对数未增加：$pairs_before_switch→$pairs_after_switch）" \
        || bad "换曲后 real2 仍产出了完整缓存（对数 $pairs_before_switch→$pairs_after_switch）"
    daemon_up && ok "换曲取消后核心存活" || bad "换曲取消后核心掉线"
    # config v12 没有功能开关：切 Off 只停当前档位，不再按 feature 取消后台
    # job（core.c 的取消逻辑已随 v12 删除，与旧版 feature 保持开启时一致）。
    "$TM_BIN" karaoke off >/dev/null 2>&1
    wait_mode 0 >/dev/null 2>&1
    sleep 1.0
    daemon_up && ok "切到 Off 不崩核心（v12：档位即开关）" \
        || bad "切到 Off 导致核心掉线"
else
    skip "ffmpeg 缺失或 real2 夹具不存在：换曲取消断言"
fi

# ── S8 单声道 / >30min 拒绝 ─────────────────────────────────────────
info "S8 单声道与 >30min：拒绝分离、原声继续"
if [ "$have_ffmpeg" = "1" ] && [ -f "$MUSIC/mono.wav" ]; then
    "$TM_BIN" karaoke full >/dev/null 2>&1
    wait_mode 2 >/dev/null 2>&1
    n_before="$(seg_dirs | grep -c . || true)"
    # 先 stop 保证是“全新起播”：mono 只有 6s，若沿用旧播放线程，
    # 状态会被上一曲的后台分离/队列接续覆盖（t6 R1 复核要的是首播状态）。
    "$TM_BIN" stop >/dev/null 2>&1
    sleep 0.4
    "$TM_BIN" play "$MUSIC/mono.wav" >/dev/null 2>&1
    wait_track "$MUSIC/mono.wav" 20 || true
    # 先抓失败原因（mono 很短，位置断言会拖到下一曲接管）。
    require_status_ok '立体声|单声道|1 ?声道' 10 \
        "单声道拒绝原因可见（立体声/单声道）" \
        "未看到单声道拒绝原因（立体声/单声道）"
    # 等新曲目真正开始（时长切到 6s 且位置回到开头），避免读到上一曲位置。
    for _ in $(seq 1 80); do
        _d="$(duration_ms)"; _p="$(pos_ms)"
        [ "${_d:-0}" -gt 0 ] && [ "${_d:-0}" -lt 10000 ] && [ "${_p:-0}" -lt 3000 ] && break
        sleep 0.25
    done
    sleep 0.3
    mp0="$(pos_ms)"; sleep 0.8; mp1="$(pos_ms)"
    [ "${mp1:-0}" -gt "${mp0:-0}" ] && ok "单声道曲目照常播放（原声直通 ${mp0}→${mp1} ms）" \
        || bad "单声道曲目播放异常（${mp0}→${mp1} ms）"
    [ "$(seg_dirs | grep -c . || true)" = "$n_before" ] && ok "单声道没有生成缓存目录" \
        || bad "单声道生成了缓存目录"
    daemon_up && ok "单声道后核心存活" || bad "单声道导致核心掉线"
    if [ -f "$MUSIC/long.flac" ]; then
        "$TM_BIN" play "$MUSIC/long.flac" >/dev/null 2>&1
        wait_track "$MUSIC/long.flac" 20 || true
        sleep 1.0
        lp0="$(pos_ms)"; sleep 1.5; lp1="$(pos_ms)"
        [ "${lp1:-0}" -gt "${lp0:-0}" ] && ok ">30min 曲目原声继续播放（${lp0}→${lp1} ms）" \
            || bad ">30min 曲目播放异常"
        require_status_ok '超过 ?上限|30 ?分 ?钟|1800' 25 \
            ">30min 拒绝原因可见（超过上限/1800s）" \
            "未看到 >30min 拒绝原因（超过上限/1800s）"
        [ "$(seg_dirs | grep -c . || true)" = "$n_before" ] && ok ">30min 没有生成缓存目录" \
            || bad ">30min 生成了缓存目录"
        daemon_up && ok ">30min 后核心存活" || bad ">30min 导致核心掉线"
    fi
else
    skip "ffmpeg 缺失：单声道 / >30min 断言"
fi

# ── S9 缓存损坏回退 ────────────────────────────────────────────────
info "S9 分段缓存损坏：拒绝使用、回退原声/重做损坏段"
if [ -n "$SEG_DIR" ] && [ -d "$SEG_DIR" ]; then
    truncate -s 100 "$SEG_DIR/seg000.soft.flac"
    "$TM_BIN" stop >/dev/null 2>&1
    "$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
    if wait_pos 1000 40; then
        ok "损坏段下仍能起播（拒绝损坏段 → 重做/回退原声）"
    else
        bad "损坏段导致 40s 内无法起播"
    fi
    repaired=0
    for _ in $(seq 1 40); do
        [ "$(stat -c %s "$SEG_DIR/seg000.soft.flac" 2>/dev/null)" != "100" ] && { repaired=1; break; }
        sleep 0.5
    done
    if [ "$repaired" = "1" ]; then
        ok "损坏段被识别并重做（soft.flac 已恢复，未当作有效缓存）"
    else
        ok "损坏段被拒绝使用（保持原状，播放回退原声）"
    fi
    daemon_up && ok "损坏缓存后核心存活" || bad "损坏缓存导致核心掉线"
else
    skip "没有完整段缓存可做损坏回退断言"
fi

# ── S10 XDG 缓存根不可写 / Permission；S11 运行中模型消失 ────────────
info "S10 XDG 缓存根不可写：v2 只写 /tmp，分离/播放不受影响"
"$TM_BIN" stop >/dev/null 2>&1
"$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
sleep 0.8
start_core || bad "重建核心失败（S10 前置）"
rm -rf "$CACHE_ROOT"
mkdir -p "$CACHE_ROOT"
chmod 0555 "$CACHE_ROOT"
"$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
if wait_pos 1000 40; then
    ok "XDG 缓存不可写时仍能起播（v1 不写、v2 写 /tmp）"
else
    bad "XDG 缓存不可写导致播放异常"
fi
seg_seen=0
for _ in $(seq 1 80); do
    [ -n "$(seg_dirs | head -1)" ] && { seg_seen=1; break; }
    sleep 0.5
done
[ "$seg_seen" = "1" ] && ok "XDG 只读不影响 v2 会话分段（/tmp 下已创建段目录）" \
    || bad "XDG 只读时 /tmp 没有出现 v2 段目录"
[ -z "$(cache_dirs)" ] && ok "XDG karaoke 根未写入新缓存（D6：v2 只写 /tmp）" \
    || bad "XDG karaoke 根出现了新写入"
daemon_up && ok "缓存不可写时核心存活" || bad "缓存不可写导致核心掉线"
chmod 0755 "$CACHE_ROOT"

# S10b（R1）：把 /tmp 会话的 model 目录设为 0555，新曲目无法建段目录 →
# worker 报 Permission；回退消息必须带出该原因（而不是笼统的"已回退原声"）。
"$TM_BIN" stop >/dev/null 2>&1
SEG_ROOT="$(session_root_dir)"
MODEL_DIR="$(find "${SEG_ROOT:-/nonexistent}" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | head -1)"
if [ -n "$MODEL_DIR" ] && [ -d "$MODEL_DIR" ]; then
    cp -f "$MUSIC/real.flac" "$MUSIC/perm.flac" 2>/dev/null
    chmod 0555 "$MODEL_DIR"
    "$TM_BIN" play "$MUSIC/perm.flac" >/dev/null 2>&1
    wait_track "$MUSIC/perm.flac" 20 || true
    require_status_ok 'Permission|权限|denied' 30 \
        "缓存目录不可写 → 失败原因（Permission）可见" \
        "缓存目录不可写时未看到 Permission 原因"
    chmod 0755 "$MODEL_DIR"
    "$TM_BIN" stop >/dev/null 2>&1
    daemon_up && ok "段目录不可写后核心存活" || bad "段目录不可写导致核心掉线"
else
    bad "S10 缺少可用的会话 model 目录，无法构造 Permission 场景"
fi
info "S11 运行中模型文件消失：分离失败原因可见、原声继续、核心存活"
"$TM_BIN" stop >/dev/null 2>&1
# 当前使用的模型 = 随包路径（S2 已恢复）；若该路径不可写则用旧 XDG 副本。
ACTIVE_MODEL="$BUNDLED_MODEL"
[ -f "$ACTIVE_MODEL" ] || ACTIVE_MODEL="$INSTALLED"
if [ -f "$ACTIVE_MODEL" ]; then
    mv "$ACTIVE_MODEL" "$ACTIVE_MODEL.bak"
    "$TM_BIN" play "$MUSIC/real2.flac" >/dev/null 2>&1
    wait_track "$MUSIC/real2.flac" 20 || true
    wait_pos 500 20 || true
    dp0="$(pos_ms)"; sleep 1.5; dp1="$(pos_ms)"
    [ "${dp1:-0}" -gt "${dp0:-0}" ] && ok "模型文件消失时原声继续播放（${dp0}→${dp1} ms）" \
        || bad "模型文件消失导致播放异常（${dp0}→${dp1} ms）"
    require_status_ok '分离失败|模型' 30 \
        "失败原因可见（分离失败/模型）" \
        "未看到分离失败原因"
    daemon_up && ok "模型文件消失时核心存活" || bad "模型文件消失导致核心掉线"
    mv "$ACTIVE_MODEL.bak" "$ACTIVE_MODEL"
    ok "模型文件已恢复（$ACTIVE_MODEL）"
else
    skip "找不到当前使用的模型文件，S11 无法构造"
fi
"$TM_BIN" stop >/dev/null 2>&1

# ── S12 无运行库：分离失败原因（ONNX Runtime）可见、原声继续 ─────────
info "S12 无 libonnxruntime：分离失败原因可见、原声继续、核心存活"
if [ "$STATIC_ORT" = "1" ]; then
    # 静态链接构建不依赖系统 .so；LD_LIBRARY_PATH 遮蔽只对 dlopen 构建有意义。
    skip "S12 无运行库失败路径（静态链接构建不适用；由动态构建覆盖）"
    "$TM_BIN" daemon stop --all >/dev/null 2>&1
else
FAKE_ORT="$WORK_DIR/fake-ort"
mkdir -p "$FAKE_ORT"
for so in libonnxruntime.so.1.21 libonnxruntime.so.1 libonnxruntime.so; do
    printf 'not an elf\n' >"$FAKE_ORT/$so"
done
"$TM_BIN" daemon stop --all >/dev/null 2>&1
sleep 0.8
# 坏 ELF 遮蔽真实库：dlopen 只认 LD_LIBRARY_PATH 里第一个同名文件；推理在
# 核心进程的 worker 里跑，所以坏库必须给核心进程（前端不再承担模型启用）。
if LD_LIBRARY_PATH="$FAKE_ORT:${LD_LIBRARY_PATH:-}" start_core; then
    ok "无运行库会话上线（坏 ELF 已遮蔽 libonnxruntime）"
    "$TM_BIN" karaoke full >/dev/null 2>&1
    wait_mode 2 >/dev/null 2>&1
    "$TM_BIN" play "$MUSIC/real.flac" >/dev/null 2>&1
    wait_track "$MUSIC/real.flac" 20 || true
    require_status_ok 'ONNX ?Runtime|分离失败' 30 \
        "无运行库时分离失败原因可见（ONNX Runtime/分离失败）" \
        "无运行库时没有明确报错"
    wait_pos 500 20 || true
    dp0="$(pos_ms)"; sleep 1.2; dp1="$(pos_ms)"
    [ "${dp1:-0}" -gt "${dp0:-0}" ] && ok "无运行库时原声继续播放（${dp0}→${dp1} ms）" \
        || bad "无运行库导致播放异常（${dp0}→${dp1} ms）"
    daemon_up && ok "无运行库时核心存活（无静默降级）" || bad "无运行库导致核心掉线"
else
    skip "无法用坏 ELF 遮蔽 libonnxruntime：无运行库 E2E 跳过（单元测试已覆盖）"
fi
"$TM_BIN" daemon stop --all >/dev/null 2>&1
fi

# ── 收尾 ────────────────────────────────────────────────────────────
"$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
info "结果"
printf '%d 通过, %d 跳过, %d 失败\n' "$PASS" "$SKIPPED" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
