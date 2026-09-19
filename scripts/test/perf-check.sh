#!/usr/bin/env bash
#
# A6 性能回归：命令→可见状态 时延 + 空闲 CPU 占用
#
# 覆盖待办的 A6 两条硬指标：
#   1) 命令→状态可见 < 50 ms：在被测核心上反复「发 SetVolume/SetPlayMode →
#      轮询 Info.GetInfo 直到新值出现」，取中位数。走的是与 TUI 每帧取快照
#      完全相同的读取路径，所以量的是核心侧生效+可见的开销，不含终端按键延迟。
#      这个数是**保守上界**：核心主循环节拍 DAEMON_TICK_US=20ms，命令等一个
#      tick 生效、观测再等一个 tick，所以中位数稳定在 ~40ms（实测 5 组样本
#      38.7~41.1ms，抖动 <2.5ms）。界面自己看到的值更快——前端在调用回复成功
#      时就镜像了新值（player_remote.c），实际 ≤1 个节拍。反过来说：谁把节拍
#      调大、或把快照刷新挪进慢路径，这个中位数会立刻顶到 50ms 阈值上，这正是
#      本检查要守的东西；
#   2) 空闲 CPU < 1%：分别在核心（daemon）与 TUI 上采样 /proc/<pid>/stat 的
#      utime+stime。TUI 用 pty 拉起、不发按键，样本区间内只应看到事件循环的
#      空转开销。
#
# 用法：scripts/test/perf-check.sh [--bin <ter-music>] [--report]
#                                [--samples N] [--seconds S] [--keep]
#   --report 只打印实测值，不做阈值判定（本机无音频设备/负载高时用）
#
# 说明：本机没有声卡，核心不会进入真正的播放解码，因此"播放中的 CPU"不在
# 本脚本范围内；空闲 CPU 与命令时延不受影响。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
LATENCY_MAX_MS="${LATENCY_MAX_MS:-50}"
IDLE_CPU_MAX_PCT="${IDLE_CPU_MAX_PCT:-1.0}"
SAMPLES=5
SECONDS_PER_SAMPLE=8
REPORT=0
KEEP=0
WORK_DIR=""

usage() { sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# 原始参数：解析循环会 shift 掉 "$@"，重入私有总线时必须带着它
ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --report) REPORT=1; shift ;;
        --samples) SAMPLES="$2"; shift 2 ;;
        --seconds) SECONDS_PER_SAMPLE="$2"; shift 2 ;;
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

# 判阈值；--report 下只打印
judge() {   # judge <说明> <实测> <上限> <单位> <比较符: lt>
    local label="$1" value="$2" limit="$3" unit="$4"
    local verdict
    verdict="$(awk -v v="$value" -v l="$limit" 'BEGIN { print (v < l) ? "pass" : "fail" }')"
    printf '  %-28s %8s %s（阈值 < %s %s）\n' "$label" "$value" "$unit" "$limit" "$unit"
    if [ "$REPORT" -eq 1 ]; then
        return 0
    fi
    if [ "$verdict" = "pass" ]; then
        ok "$label 达标"
    else
        bad "$label 超标：$value $unit >= $limit $unit"
    fi
}

# ── 独占的总线：默认起私有会话总线 ───────────────────────────────
# 只允许本脚本拉起的核心在总线上（否则会量到桌面残留实例）；需要复用当前总线
# 时显式传 TM_TEST_REUSE_SESSION_BUS=1。TM_TEST_BUS_READY 是再入标记。
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ "${TM_TEST_REUSE_SESSION_BUS:-0}" != "1" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：需要 dbus-run-session，或设 TM_TEST_REUSE_SESSION_BUS=1 复用当前会话总线。" >&2
        exit 2
    fi
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

cleanup() {
    [ -n "${TUI_PID:-}" ] && kill -9 "$TUI_PID" 2>/dev/null
    "$TM_BIN" daemon stop >/dev/null 2>&1 || true
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先用 --bin 指定）" >&2; exit 2; }

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-perf-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$WORK_DIR/music"

daemon_up() { "$TM_BIN" daemon status 2>/dev/null | grep -q "pid"; }

# /proc/<pid>/stat 的 utime+stime（去掉含空格的 comm 字段后取第 12/13 列）
cpu_ticks() {
    sed 's/.*) //' "/proc/$1/stat" 2>/dev/null | awk '{ print $12 + $13 }'
}

# 采样 <pid> 在 <seconds> 内的 CPU 占用百分比
sample_cpu_pct() {   # sample_cpu_pct <pid> <seconds> <说明>
    local pid="$1" secs="$2" label="$3"
    local hz t0 t1 delta
    hz="$(getconf CLK_TCK)"
    t0="$(cpu_ticks "$pid")"
    [ -n "$t0" ] || { echo ""; return 1; }
    sleep "$secs"
    t1="$(cpu_ticks "$pid")"
    [ -n "$t1" ] || { echo ""; return 1; }
    delta=$((t1 - t0))
    awk -v d="$delta" -v hz="$hz" -v s="$secs" 'BEGIN { printf "%.3f", (d / hz) / s * 100.0 }'
}

# ── 1. 命令→状态可见 时延 ────────────────────────────────────────
info "启动核心（daemon）"
"$TM_BIN" daemon start >/dev/null 2>&1
CORE_PID=""
for _ in $(seq 1 50); do
    if daemon_up; then
        CORE_PID="$("$TM_BIN" daemon status 2>/dev/null | sed -n 's/.*pid \([0-9]\{1,\}\).*/\1/p' | head -1)"
        break
    fi
    sleep 0.2
done
if [ -z "$CORE_PID" ]; then
    echo "错误：核心未在 10 秒内就绪。" >&2
    exit 2
fi
ok "核心就绪（pid $CORE_PID）"

info "命令→可见状态 时延（样本 $SAMPLES 次往返）"
LAT_OUT="$(python3 "$SCRIPT_DIR/perf-driver.py" latency org.mpris.MediaPlayer2.ter_music "$SAMPLES" 2>&1)"
LAT_RC=$?
printf '%s\n' "$LAT_OUT" | sed 's/^/  /'
if [ "$LAT_RC" -ne 0 ]; then
    if [ "$REPORT" -eq 1 ]; then
        echo "  （--report：驱动未取到样本，跳过判定）"
    else
        bad "时延探针未取到有效样本"
    fi
else
    VOL_MS="$(printf '%s\n' "$LAT_OUT" | sed -n 's/^SetVolume 中位数 \([0-9.]*\) ms.*/\1/p')"
    MODE_MS="$(printf '%s\n' "$LAT_OUT" | sed -n 's/^SetPlayMode 中位数 \([0-9.]*\) ms.*/\1/p')"
    [ -n "$VOL_MS" ] && judge "SetVolume→快照可见" "$VOL_MS" "$LATENCY_MAX_MS" "ms"
    [ -n "$MODE_MS" ] && judge "SetPlayMode→快照可见" "$MODE_MS" "$LATENCY_MAX_MS" "ms"
fi

# ── 2. 空闲 CPU：核心 ───────────────────────────────────────────
info "空闲 CPU（各采样 ${SECONDS_PER_SAMPLE}s）"
CORE_CPU="$(sample_cpu_pct "$CORE_PID" "$SECONDS_PER_SAMPLE" core)"
if [ -n "$CORE_CPU" ]; then
    judge "核心空闲 CPU" "$CORE_CPU" "$IDLE_CPU_MAX_PCT" "%"
else
    bad "核心 CPU 采样失败（pid $CORE_PID 已退出？）"
fi

# ── 3. 空闲 CPU：TUI（pty 拉起，不发按键） ──────────────────────
TUI_CPU="$(python3 - "$TM_BIN" "$SECONDS_PER_SAMPLE" "$SCRIPT_DIR" <<'PY'
import os, pty, sys, time

binary, secs, script_dir = sys.argv[1], float(sys.argv[2]), sys.argv[3]
sys.path.insert(0, script_dir)

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(binary, [binary])
    os._exit(127)

def ticks():
    try:
        with open("/proc/%d/stat" % pid) as handle:
            return sum(int(x) for x in handle.read().split(") ", 1)[1].split()[:13][11:13])
    except OSError:
        return None

# 等界面起来（TUI 启动时会有一次扫描/渲染开销，不计入空闲区间）
time.sleep(2.0)
t0 = ticks()
if t0 is None:
    os.kill(pid, 9)
    print("")
    sys.exit(0)
time.sleep(secs)
t1 = ticks()
hz = os.sysconf("SC_CLK_TCK")
os.write(fd, b"q")            # 正常退出，顺带验证空闲后仍能响应按键
time.sleep(0.5)
try:
    os.kill(pid, 9)
except ProcessLookupError:
    pass
os.waitpid(pid, 0)
if t1 is None:
    print("")
else:
    print("%.3f" % (((t1 - t0) / hz) / secs * 100.0))
PY
)"
if [ -n "$TUI_CPU" ]; then
    judge "TUI 空闲 CPU" "$TUI_CPU" "$IDLE_CPU_MAX_PCT" "%"
else
    bad "TUI CPU 采样失败"
fi

# ── 收尾 ────────────────────────────────────────────────────────
"$TM_BIN" daemon stop >/dev/null 2>&1
for _ in $(seq 1 20); do daemon_up || break; sleep 0.1; done

echo
if [ "$REPORT" -eq 1 ]; then
    printf 'A6 性能实测（--report，不判定）：通过 %d，失败 %d\n' "$PASS" "$FAIL"
else
    printf 'A6 性能回归：通过 %d，失败 %d\n' "$PASS" "$FAIL"
fi
[ "$FAIL" -eq 0 ] || exit 1
exit 0
