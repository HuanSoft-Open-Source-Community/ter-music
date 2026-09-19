#!/usr/bin/env bash
#
# 前端远程端到端：核心只拿到本地文件
#
# 终态：远程音乐源（SMB/SFTP/FTP/WebDAV/HTTP）由**前端**负责——列出目录、
# 逐曲下载到本地缓存，再把本地缓存路径交给核心播放；核心不认识远程。
#
# 本脚本用本地 HTTP 服务器 + pty 驱动的 TUI 验证整条链路：
#   1. 起一个本地 HTTP 服务，目录里放一首音频；
#   2. 全新 HOME/缓存，TUI 以 `-o http://127.0.0.1:PORT/` 启动（前端远程会话）；
#   3. 断言：缓存目录出现该音频（且无 .part 残留）；
#   4. 断言：核心侧看到的曲目路径就是缓存里的本地路径（快照 path 落在缓存目录）；
#   5. 断言：核心不发布 Remote 接口、core.methods 不含 Remote.*。
#
# 用法：scripts/test/remote-frontend-e2e.sh [--bin <ter-music>] [--keep]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""
HTTP_PID=""

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

# 原始参数：解析循环会 shift 掉 "$@"，重入私有总线时必须带着它
ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

# ── 独占的总线：默认起私有会话总线 ───────────────────────────────
# 这两个脚本必须只有一个核心在总线上，否则前端会接到上一次运行残留的实例上
# （实测：远端 e2e 曾接上迁移测试留下的核心）。需要复用当前桌面总线时显式传
# TM_TEST_REUSE_SESSION_BUS=1；TM_TEST_BUS_READY 是再入标记，防止无限重入。
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ "${TM_TEST_REUSE_SESSION_BUS:-0}" != "1" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：需要 dbus-run-session，或设 TM_TEST_REUSE_SESSION_BUS=1 复用当前会话总线。" >&2
        exit 2
    fi
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

cleanup() {
    if [ -n "$HTTP_PID" ]; then
        kill "$HTTP_PID" 2>/dev/null || true
    fi
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN" >&2; exit 2; }
command -v ffmpeg >/dev/null || { echo "错误：需要 ffmpeg 生成测试音频" >&2; exit 2; }

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-remote-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$WORK_DIR/serve" "$WORK_DIR/home"

info "准备本地 HTTP 服务"
ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=2" \
    -c:a pcm_s16le -y "$WORK_DIR/serve/remote-tone.wav" 2>/dev/null

PORT="$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)"
( cd "$WORK_DIR/serve" && python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1 ) &
HTTP_PID=$!
sleep 1

if curl -sf "http://127.0.0.1:$PORT/remote-tone.wav" -o /dev/null; then
    ok "HTTP 服务可用（127.0.0.1:$PORT）"
else
    bad "HTTP 服务不可用"
    KEEP=1
    exit 1
fi

info "以远程 URL 启动 TUI（前端远程会话）"
if ! python3 "$SCRIPT_DIR/tui-probe.py" --bin "$TM_BIN" \
        --music "http://127.0.0.1:$PORT/" --home "$HOME" \
        --keys " " --state-out "$WORK_DIR/state.json" --wait 6 >"$WORK_DIR/probe.out" 2>&1; then
    bad "TUI 探针失败：$(tail -2 "$WORK_DIR/probe.out" | tr '\n' ' ')"
    KEEP=1
else
    ok "TUI 以远程 URL 启动并保持存活"
fi

info "断言：前端已把曲目下载到本地缓存"
CACHE_ROOT="$XDG_CACHE_HOME/ter-music/remote"
cached="$(find "$CACHE_ROOT" -name 'remote-tone.wav' -type f 2>/dev/null | head -1)"
if [ -n "$cached" ]; then
    ok "缓存命中：${cached#"$WORK_DIR"/}"
else
    bad "缓存目录未出现下载的音频（$CACHE_ROOT）"
fi
if find "$CACHE_ROOT" -name '*.part' 2>/dev/null | grep -q .; then
    bad "缓存里存在未完成的 .part 文件"
else
    ok "无 .part 残留（下载为原子落盘）"
fi

info "断言：核心只看到本地缓存路径"
if [ -f "$WORK_DIR/state.json" ]; then
    core_path="$(python3 - "$WORK_DIR/state.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1], encoding='utf-8'))
track = doc.get('track') or {}
print(track.get('path') or '')
PY
)"
    if [ -n "$cached" ] && [ "$core_path" = "$cached" ]; then
        ok "核心侧曲目路径＝缓存本地路径（$core_path）"
    elif [ -n "$core_path" ]; then
        bad "核心侧曲目路径不是缓存文件：$core_path"
    else
        printf '  skip 快照里还没有曲目（下载/交付可能尚未完成）\n'
    fi

    if python3 - "$WORK_DIR/state.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1], encoding='utf-8'))
track = doc.get('track') or {}
sys.exit(1 if 'is_remote' in track else 0)
PY
    then
        ok "Info 快照已无 is_remote 字段"
    else
        bad "Info 快照仍含 is_remote"
    fi

    if python3 - "$WORK_DIR/state.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1], encoding='utf-8'))
core = doc.get('core') or {}
methods = core.get('methods') or []
sys.exit(1 if any(m.startswith('Remote.') for m in methods) else 0)
PY
    then
        ok "core.methods 不含 Remote.*"
    else
        bad "core.methods 仍声明 Remote.*"
    fi
else
    bad "缺少状态快照"
fi

info "断言：前端设置改动经门面到达核心（配置归属）"
core_last_path="$(python3 "$SCRIPT_DIR/rpc_client.py" call org.yxzl.ter_music.Config.GetAll 2>/dev/null |
    python3 -c '
import json, sys
raw = sys.stdin.read().strip()
try:
    doc = json.loads(raw)
except Exception:
    print("")
    raise SystemExit
paths = doc.get("paths") or {}
print(paths.get("last_opened_path") or "")
' 2>/dev/null || true)"
if [ -n "$core_last_path" ] && [ "$core_last_path" = "http://127.0.0.1:$PORT/" ]; then
    ok "核心配置收到了前端经门面下发的 last_opened_path（$core_last_path）"
elif [ -n "$core_last_path" ]; then
    bad "核心配置里的 last_opened_path 不是本次远程 URL：$core_last_path"
else
    bad "核心配置里没有 last_opened_path（前端 persist 未到达核心）"
fi

# 收尾：停掉前端拉起的核心，避免在真实会话总线上留下残留进程
"$TM_BIN" daemon stop >/dev/null 2>&1 || true

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || { KEEP=1; echo "（工作目录保留在 $WORK_DIR）" >&2; }
[ "$FAIL" -eq 0 ]
