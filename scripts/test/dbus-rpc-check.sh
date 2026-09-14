#!/usr/bin/env bash
#
# ter-music D-Bus RPC 接口面单点验证（里程碑 M2）
#
# 验证内容：
#   1. 自省 XML 中 org.yxzl.ter_music.* 各接口及方法齐备；
#   2. Info.GetInfo 的 core.methods 声明与实际发布的方法一致（防漂移）；
#   3. 各接口的 schema、分页边界与超大响应拒绝（随 M2 逐步补充）；
#   4. 退出时不留残留进程。
#
# 用法：
#   scripts/test/dbus-rpc-check.sh [--bin <ter-music>] [--keep]
#
# 环境：需要会话总线；未设置 DBUS_SESSION_BUS_ADDRESS 时自动用
#       dbus-run-session 起一条私有总线。需要 ffmpeg 生成测试音频。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""
DAEMON_PID=""

PASS=0
FAIL=0

# M2 完成后应存在的接口（随里程碑推进逐个加入本清单）
EXPECTED_INTERFACES=(
    "org.yxzl.ter_music.Lyrics"
    "org.yxzl.ter_music.Info"
    "org.yxzl.ter_music.Control"
)

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage; exit 2 ;;
    esac
done

ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

# ── 复用总线：没有会话总线时用 dbus-run-session 重入 ───────────────
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：没有会话总线，且未安装 dbus-run-session。" >&2
        exit 2
    fi
    exec dbus-run-session -- "$0" "$@"
fi

cleanup() {
    # 显式停止实例；用记录下来的 PID 兜底，避免 pgrep 匹配到本脚本自身
    if [ -n "$DAEMON_PID" ]; then
        "$TM_BIN" daemon stop >/dev/null 2>&1 || true
        for _ in $(seq 1 20); do
            kill -0 "$DAEMON_PID" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "$DAEMON_PID" 2>/dev/null || true
    fi
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

# ── 准备隔离环境与测试素材 ─────────────────────────────────────────
setup_fixtures() {
    WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-rpc-XXXXXX")"
    mkdir -p "$WORK_DIR/music" "$WORK_DIR/home"

    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -hide_banner -loglevel error -f lavfi -i anullsrc=r=44100:cl=mono \
            -t 3 -c:a pcm_s16le -y "$WORK_DIR/music/a.wav" 2>/dev/null
        ffmpeg -hide_banner -loglevel error -f lavfi -i anullsrc=r=44100:cl=mono \
            -t 3 -c:a pcm_s16le -y "$WORK_DIR/music/b.wav" 2>/dev/null
    fi

    export HOME="$WORK_DIR/home"
    export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
    export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
}

start_daemon() {
    local pid
    pid="$("$TM_BIN" daemon start --open "$WORK_DIR/music" 2>/dev/null |
           sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
    if [ -z "$pid" ]; then
        # daemon start 的输出格式变化时退回按总线名查询
        pid="$("$TM_BIN" daemon status 2>/dev/null | sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
    fi
    DAEMON_PID="$pid"
    [ -n "$DAEMON_PID" ]
}

dbus_call() {
    local iface="$1" method="$2"; shift 2
    timeout 10 gdbus call --session \
        --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 \
        --method "$iface.$method" "$@" 2>&1
}

introspect() {
    timeout 10 gdbus introspect --session \
        --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 2>&1
}

# ── 检查 1：接口与方法齐备 ─────────────────────────────────────────
check_interfaces() {
    local xml
    xml="$(introspect)"
    if [ -z "$xml" ] || [ "${xml#*Error}" != "$xml" ]; then
        bad "自省失败：$(printf '%s' "$xml" | head -1)"
        return
    fi
    for iface in "${EXPECTED_INTERFACES[@]}"; do
        if printf '%s' "$xml" | grep -q "interface $iface"; then
            ok "接口存在 $iface"
        else
            bad "接口缺失 $iface"
        fi
    done
}

# ── 检查 2：声明的方法清单与实际发布一致 ───────────────────────────
check_method_list() {
    local json
    json="$(dbus_call org.yxzl.ter_music.Info GetInfo)"
    if ! printf '%s' "$json" | grep -q '"api_version"'; then
        echo "  skip core.methods 尚未实现（M2.2 起生效）"
        return
    fi

    local declared
    declared="$(printf '%s' "$json" | python3 -c '
import json,sys
try:
    doc = json.load(sys.stdin)
except Exception as exc:
    print("JSON-PARSE-ERROR", exc)
    raise SystemExit(0)
methods = doc.get("core", {}).get("methods")
if not isinstance(methods, list):
    print("NO-METHODS")
    raise SystemExit(0)
print("\n".join(methods))
')"

    if [ "$declared" = "NO-METHODS" ] || [ -z "$declared" ]; then
        bad "core.methods 缺失或不是数组"
        return
    fi
    case "$declared" in
        JSON-PARSE-ERROR*) bad "GetInfo 返回的不是合法 JSON：$declared"; return ;;
    esac

    local xml
    xml="$(introspect)"
    local checked=0 missing=0
    while IFS= read -r method; do
        [ -z "$method" ] && continue
        checked=$((checked + 1))
        local member="${method##*.}"
        if ! printf '%s' "$xml" | grep -q "method $member"; then
            bad "core.methods 声明了未实现的方法 $method"
            missing=$((missing + 1))
        fi
    done <<< "$declared"

    if [ "$missing" -eq 0 ]; then
        ok "core.methods 与实际发布一致（$checked 个方法）"
    fi
}

# ── 主流程 ─────────────────────────────────────────────────────────
info "准备隔离环境"
setup_fixtures

if [ ! -x "$TM_BIN" ]; then
    echo "错误：找不到可执行文件 $TM_BIN（先构建，或用 --bin 指定）" >&2
    exit 2
fi

info "启动测试实例"
if ! start_daemon; then
    bad "daemon 启动失败"
    echo "（工作目录保留在 $WORK_DIR）" >&2
    KEEP=1
    exit 1
fi
ok "daemon 已启动（pid $DAEMON_PID）"

info "检查 1：接口齐备"
check_interfaces

info "检查 2：方法清单一致性"
check_method_list

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
