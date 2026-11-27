#!/usr/bin/env bash
#
# 前端远程音乐源端到端（真实运行，容器/Linyaps 可跑）
#
# 终态约定：远程音乐源（SMB/SFTP/FTP/WebDAV/HTTP）由**前端**负责——列出目录、
# 逐曲下载到本地缓存，再把本地缓存路径交给核心播放；核心不认识远程。
#
# 本脚本不依赖 ffmpeg、python-gi、tui-probe：
#   - 音频用 python3 wave 生成；D-Bus 断言用 gdbus（scripts/test/remote_test_client.py）；
#   - 在私有 dbus-run-session 内**先起 foreground 核心**，再用 pty 驱动 TUI；
#   - 协议夹具：HTTP/WebDAV/FTP/TLS/慢速/401，全部本地回环；
#   - 负例：401 / 404 / 连接拒绝 / 空目录 / 非音频 / FTP 空目录 / 取消；
#   - 安全：SMB 假客户端断言 argv 不被 shell 解释、密码不进 argv/日志/配置；
#   - 覆盖 remote 与 local 两种后端的队列条数（local 曾重复入队，F-15）。
#
# 用法：scripts/test/remote-frontend-e2e.sh [--bin <ter-music>] [--keep]
#
# 退出码：0 = 全部断言通过；1 = 有失败；2 = 环境不满足（缺工具/构建产物）。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PY="${PYTHON:-python3}"
CLIENT="$SCRIPT_DIR/remote_test_client.py"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""
CORE_PID=""
declare -a SERVER_PIDS=()

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) sed -n '2,29p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

# ── 私有会话总线（只允许本脚本拉起的核心在线） ─────────────────────
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ "${TM_TEST_REUSE_SESSION_BUS:-0}" != "1" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：需要 dbus-run-session，或设 TM_TEST_REUSE_SESSION_BUS=1 复用当前会话总线。" >&2
        exit 2
    fi
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

cleanup() {
    for pid in "${SERVER_PIDS[@]:-}"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
    if [ -n "$CORE_PID" ]; then
        kill "$CORE_PID" 2>/dev/null || true
        wait "$CORE_PID" 2>/dev/null || true
    fi
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

# ── 环境预检（缺工具明确退出 2，不伪装成产品失败） ─────────────────
[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN" >&2; exit 2; }
for tool in "$PY" gdbus dbus-run-session openssl curl; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "错误：环境缺少 $tool（远程 E2E 需要它）" >&2
        exit 2
    }
done
if ! "$PY" -c 'import pyftpdlib' >/dev/null 2>&1; then
    echo "错误：环境缺少 pyftpdlib（FTP 夹具需要；可通过 pip install pyftpdlib 安装）" >&2
    exit 2
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-remote-e2e-XXXXXX")"
CORE_HOME="$WORK_DIR/core-home"
mkdir -p "$WORK_DIR/bin" "$WORK_DIR/results" "$WORK_DIR/logs" \
         "$CORE_HOME/.config/ter-music" "$CORE_HOME/.cache"

# 核心退出时不要被前端顺手关掉：后续 Config/Queue 断言需要它活着。
cat > "$CORE_HOME/.config/ter-music/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <stop_core_on_exit>0</stop_core_on_exit>
  </preferences>
</ter-music-config>
EOF

# ── 夹具素材（python wave 生成，不需要 ffmpeg） ────────────────────
info "生成音频与协议夹具"
if ! "$PY" - "$CLIENT" "$WORK_DIR" <<'PY'
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location("rtc", sys.argv[1])
rtc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rtc)
work = sys.argv[2]

def w(*parts):
    path = os.path.join(work, *parts)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path

# HTTP 根：多曲 + 带 HTML 实体/中文/空格的名字 + 子目录 + 空目录 + 非音频
for i, name in enumerate(["tone1.wav", "tone2.wav", "tone3.wav", "a&b.wav", "中文歌.wav"]):
    rtc.write_wave(w("http", name), frequency=440 + i * 90)
rtc.write_wave(w("http", "sub dir", "inner.wav"), frequency=900)
rtc.write_wave(w("http", "only-text", "readme.txt"), frequency=300)
os.makedirs(w("http", "emptydir"), exist_ok=True)
# 防止 readme.txt 被 wave 头误导：它本来就只是非音频文件
# WebDAV 根
for i, name in enumerate(["wd1.wav", "wd2.wav"]):
    rtc.write_wave(w("webdav", name), frequency=500 + i * 100)
rtc.write_wave(w("webdav", "sub", "wd-inner.wav"), frequency=700)
# FTP 根（含空目录，回归 F-01）
for i, name in enumerate(["ftp1.wav", "ftp2.wav", "ftp3.wav", "semi;name.wav"]):
    rtc.write_wave(w("ftp", name), frequency=600 + i * 100)
rtc.write_wave(w("ftp", "sub", "ftp-inner.wav"), frequency=800)
os.makedirs(w("ftp", "emptydir"), exist_ok=True)
# TLS 根
for i, name in enumerate(["tls1.wav", "tls2.wav"]):
    rtc.write_wave(w("tls", name), frequency=520 + i * 100)
# 慢速下载根（每个 8 KiB，夹具按 512 B / 0.5 s 送 → 约 8 秒）
rtc.write_wave(w("trickle", "slow1.wav"), frequency=300, seconds=0.5, rate=8000)
rtc.write_wave(w("trickle", "slow2.wav"), frequency=310, seconds=0.5, rate=8000)
# 截断夹具：>200 首（验证主界面“仅加载 200/205”与 truncated）
for _i in range(205):
    rtc.write_wave(w("http", "bulk", "t%03d.wav" % _i), frequency=300, seconds=0.005)
# 本地模式重复打开用的空音乐目录
os.makedirs(w("empty-music"), exist_ok=True)
# 重试夹具根：第一次列目录返回 404，之后成功（验证错误态 → 重试）
rtc.write_wave(w("retry", "retry1.wav"), frequency=700)
# 下载重试夹具：flaky 首次下载失败，第二次成功（验证“重试失败曲目”）
rtc.write_wave(w("retry-dl", "ok.wav"), frequency=720)
rtc.write_wave(w("retry-dl", "flaky.wav"), frequency=730)
# SMB 根：正常曲目 + 恶意文件名
# 旧实现 system() 会把 ';touch PWN-SMB;echo ' 当命令执行；R-01 的真实
# Samba 注入则是 ';put ...' —— 两者都必须被拒绝（smbclient -c 命令模式）。
rtc.write_wave(w("smb", "smb tone.wav"), frequency=650)
rtc.write_wave(w("smb", "smb two.wav"), frequency=655)
rtc.write_wave(w("smb", "x';touch PWN-SMB;echo '.mp3"), frequency=660)
rtc.write_wave(w("smb", "x;put secret.txt stolen.txt;z.mp3"), frequency=665)
# 超大下载夹具：列表 size 正常，get 时生成 1GiB 稀疏文件（验证下载后硬上限）
rtc.write_wave(w("smb-oversize", "huge.wav"), frequency=670)
PY
then
    echo "错误：夹具生成失败" >&2
    exit 2
fi

# TLS 自签名证书（CA:TRUE + SAN IP，让 libcurl 用 CURL_CA_BUNDLE 正常校验通过）
openssl req -x509 -newkey rsa:2048 -sha256 -days 2 -nodes \
    -keyout "$WORK_DIR/tls-key.pem" -out "$WORK_DIR/tls-cert.pem" \
    -subj "/CN=127.0.0.1" \
    -addext "subjectAltName=IP:127.0.0.1" \
    -addext "basicConstraints=critical,CA:TRUE" >/dev/null 2>&1 || {
    echo "错误：生成 TLS 测试证书失败" >&2
    exit 2
}

# ── 协议夹具服务 ─────────────────────────────────────────────────
start_server() {   # $1=名字 $2=kind $3=root [额外参数...]；stdout 输出端口
    local name="$1" kind="$2" root="$3"
    shift 3
    local port_file="$WORK_DIR/port-$name.txt"
    rm -f "$port_file"
    "$PY" "$CLIENT" serve --kind "$kind" --root "$root" --port-file "$port_file" "$@" \
        >"$WORK_DIR/server-$name.log" 2>&1 &
    local pid=$!
    SERVER_PIDS+=("$pid")
    for _ in $(seq 1 200); do
        [ -s "$port_file" ] && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.05
    done
    if [ ! -s "$port_file" ]; then
        printf 'ERROR: 夹具 %s 启动失败：%s\n' "$name" \
            "$(tail -3 "$WORK_DIR/server-$name.log" 2>/dev/null | tr '\n' ' ')" >&2
        return 1
    fi
    sed -n 's/^PORT=//p' "$port_file"
}

# 注意：start_server 必须在**当前 shell**里执行（它要往 SERVER_PIDS 里记 PID），
# 因此不能用命令替换 `$(...)`（那会进子 shell，cleanup 就杀不到夹具进程）。
port_of() {
    sed -n 's/^PORT=//p' "$WORK_DIR/port-$1.txt"
}

info "启动本地协议夹具"
start_server http http "$WORK_DIR/http" >/dev/null || exit 2
start_server auth http "$WORK_DIR/http" --auth e2e:secret >/dev/null || exit 2
start_server tls http "$WORK_DIR/tls" --cert "$WORK_DIR/tls-cert.pem" \
    --key "$WORK_DIR/tls-key.pem" >/dev/null || exit 2
start_server trickle http "$WORK_DIR/trickle" --trickle >/dev/null || exit 2
start_server webdav webdav "$WORK_DIR/webdav" >/dev/null || exit 2
start_server ftp ftp "$WORK_DIR/ftp" --user e2e --password e2e-pass >/dev/null || exit 2
P_HTTP="$(port_of http)"
P_AUTH="$(port_of auth)"
P_TLS="$(port_of tls)"
P_TRICKLE="$(port_of trickle)"
P_WEBDAV="$(port_of webdav)"
P_FTP="$(port_of ftp)"

if curl -sf "http://127.0.0.1:$P_HTTP/tone1.wav" -o /dev/null; then
    ok "HTTP 夹具可用（127.0.0.1:$P_HTTP）"
else
    bad "HTTP 夹具不可用"; KEEP=1; exit 1
fi
if curl -sf "http://127.0.0.1:$P_AUTH/" -o /dev/null; then
    bad "401 夹具没有要求认证"
else
    ok "401 夹具按预期拒绝匿名请求"
fi

# ── 同一私有总线内先起 foreground 核心（Linyaps 内 TUI 不能自行 fork） ──
info "启动核心（daemon foreground）"
HOME="$CORE_HOME" XDG_CONFIG_HOME="$CORE_HOME/.config" XDG_CACHE_HOME="$CORE_HOME/.cache" \
TER_MUSIC_LOG_DIR="$WORK_DIR/logs" \
    "$TM_BIN" daemon foreground --debug >"$WORK_DIR/daemon.out" 2>&1 &
CORE_PID=$!
if "$PY" "$CLIENT" wait-core --timeout 15 >/dev/null; then
    ok "核心已上线（foreground，私有总线）"
else
    bad "核心未能上线：$(tail -5 "$WORK_DIR/daemon.out" | tr '\n' ' ')"
    KEEP=1
    exit 1
fi

# ── 断言辅助 ──────────────────────────────────────────────────────
reset_front() {   # $1 = frontend home
    rm -rf "$1"
    mkdir -p "$1"
}
cache_audio_count() {
    local root="$1/ter-music/remote"
    find "$root" -type f \( -name '*.wav' -o -name '*.mp3' -o -name '*.flac' \
        -o -name '*.ogg' -o -name '*.m4a' \) ! -name '*.lrc' 2>/dev/null | wc -l | tr -d ' '
}
assert_audio_count() {   # root want label
    local count
    count="$(cache_audio_count "$1")"
    if [ "$count" = "$2" ]; then ok "$3（$count 首）"; else bad "$3：缓存 $count 首，期望 $2"; fi
}
assert_cache_glob() {    # root glob label
    if find "$1/ter-music/remote" -name "$2" -type f 2>/dev/null | grep -q .; then
        ok "$3"
    else
        bad "$3（缓存里没有 $2）"
    fi
}
assert_no_part() {       # root label
    local left
    left="$(find "$1/ter-music/remote" -name '*.part' -type f 2>/dev/null | wc -l | tr -d ' ')"
    if [ "$left" = "0" ]; then ok "$2（无 .part 残留）"; else bad "$2：还有 $left 个 .part"; fi
}
assert_core_queue() {    # queue-json cache-root want label
    if "$PY" - "$1" "$2" "$3" <<'PY'
import json, os, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
cache = os.path.realpath(sys.argv[2])
want = int(sys.argv[3])
errors = []
if doc.get("count") != want:
    errors.append("count=%r, want %d" % (doc.get("count"), want))
rows = doc.get("rows") or []
if len(rows) != want:
    errors.append("rows=%d, want %d" % (len(rows), want))
for row in rows:
    path = row.get("path") or ""
    if "://" in path:
        errors.append("核心收到远程 URL: " + path)
    elif not os.path.realpath(path).startswith(cache + os.sep):
        errors.append("核心路径不在缓存目录: " + path)
print("; ".join(errors))
sys.exit(1 if errors else 0)
PY
    then
        ok "$4"
    else
        bad "$4"
    fi
}
assert_info_has_no_remote() {   # info-json cache-root label
    if "$PY" - "$1" "$2" <<'PY'
import json, os, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
cache = os.path.realpath(sys.argv[2])
errors = []
methods = (doc.get("core") or {}).get("methods") or []
if not methods:
    errors.append("core.methods 为空（断言会假绿）")
for name in methods:
    if str(name).startswith("Remote."):
        errors.append("核心仍发布 " + str(name))
track = doc.get("track") or {}
path = track.get("path") or ""
if path:
    if "://" in path:
        errors.append("核心当前曲目是远程 URL: " + path)
    elif not os.path.realpath(path).startswith(cache + os.sep):
        errors.append("核心当前曲目不在缓存目录: " + path)
print("; ".join(errors))
sys.exit(1 if errors else 0)
PY
    then
        ok "$3"
    else
        bad "$3"
    fi
}
assert_last_opened() {   # config-json want label
    if "$PY" - "$1" "$2" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
got = (doc.get("paths") or {}).get("last_opened_path") or ""
want = sys.argv[2]
errors = []
if got != want:
    errors.append("last_opened_path=%r, want %r" % (got, want))
if "@" in got:
    errors.append("last_opened_path 仍含 userinfo")
print("; ".join(errors))
sys.exit(1 if errors else 0)
PY
    then
        ok "$3"
    else
        bad "$3"
    fi
}
assert_no_secret() {   # dir secret label
    if grep -rIq -- "$2" "$1" 2>/dev/null; then
        bad "$3（在 $1 下发现明文凭据）"
    else
        ok "$3"
    fi
}
assert_log_matches() {   # log-dir pattern label
    if grep -riq -- "$2" "$1" 2>/dev/null; then
        ok "$3"
    else
        bad "$3（日志里没有匹配 '$2'）"
    fi
}
run_queue_snapshot() {   # outfile
    "$PY" "$CLIENT" queue 0 100 >"$1" 2>"$1.err" || {
        echo "{}" >"$1"
        return 1
    }
}

# ── 1) HTTP：列目录 + 逐曲下载 + 核心只拿本地路径 ─────────────────
info "1. HTTP 主链路（多曲 + HTML 实体 + 中文名）"
F_HOME="$WORK_DIR/front-http"
reset_front "$F_HOME"
R="$WORK_DIR/results/http.json"
if "$PY" "$CLIENT" pty-session --bin "$TM_BIN" --url "http://127.0.0.1:$P_HTTP/" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 7 --action quit \
        --result "$R" >/dev/null; then
    ok "TUI 以 HTTP URL 启动、下载完成后正常退出"
else
    bad "TUI 探针失败：$(tail -c 400 "$R" 2>/dev/null | tr '\n' ' ')"
fi
assert_audio_count "$F_HOME/.cache" 5 "HTTP 根目录 5 首音频全部落盘"
assert_cache_glob "$F_HOME/.cache" 'a_b-*.wav' "HTML 实体/百分号编码文件名解析正确（a&b.wav）"
assert_cache_glob "$F_HOME/.cache" '*中文歌.wav' "非 ASCII 文件名落盘"
assert_no_part "$F_HOME/.cache" "HTTP 下载原子落盘"
run_queue_snapshot "$WORK_DIR/results/queue-http.json"
assert_core_queue "$WORK_DIR/results/queue-http.json" "$F_HOME/.cache/ter-music/remote" 5 \
    "核心队列 5 条且全部是缓存本地路径（非远程 URL）"
"$PY" "$CLIENT" info >"$WORK_DIR/results/info-http.json" 2>/dev/null || echo '{}' >"$WORK_DIR/results/info-http.json"
assert_info_has_no_remote "$WORK_DIR/results/info-http.json" "$F_HOME/.cache/ter-music/remote" \
    "核心 methods 非空且不含 Remote.*，当前曲目是本地缓存路径"
"$PY" "$CLIENT" config >"$WORK_DIR/results/config-http.json" 2>/dev/null || echo '{}' >"$WORK_DIR/results/config-http.json"
assert_last_opened "$WORK_DIR/results/config-http.json" "http://127.0.0.1:$P_HTTP/" \
    "核心 last_opened_path 为本次远程 URL"

# ── 2) WebDAV：标准命名空间 PROPFIND ─────────────────────────────
info "2. WebDAV 列目录/下载（标准 <D:href> 命名空间）"
F_HOME="$WORK_DIR/front-webdav"
reset_front "$F_HOME"
R="$WORK_DIR/results/webdav.json"
if "$PY" "$CLIENT" pty-session --bin "$TM_BIN" --url "webdav://127.0.0.1:$P_WEBDAV/" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 6 --action quit \
        --result "$R" >/dev/null; then
    ok "TUI 以 webdav:// URL 启动并正常退出"
else
    bad "WebDAV TUI 探针失败：$(tail -c 400 "$R" 2>/dev/null | tr '\n' ' ')"
fi
assert_audio_count "$F_HOME/.cache" 2 "WebDAV 根目录 2 首文件全部下载（目录不下载）"
assert_no_part "$F_HOME/.cache" "WebDAV 下载无 .part 残留"
run_queue_snapshot "$WORK_DIR/results/queue-webdav.json"
assert_core_queue "$WORK_DIR/results/queue-webdav.json" "$F_HOME/.cache/ter-music/remote" 2 \
    "WebDAV 队列 2 条本地缓存路径"
"$PY" "$CLIENT" config >"$WORK_DIR/results/config-webdav.json" 2>/dev/null || echo '{}' >"$WORK_DIR/results/config-webdav.json"
assert_last_opened "$WORK_DIR/results/config-webdav.json" "webdav://127.0.0.1:$P_WEBDAV/" \
    "WebDAV 会话的 last_opened_path 保留 webdav:// scheme"

# ── 3) FTP：MLSD 下载 + 空目录不崩 + 凭据不落盘 ───────────────────
info "3. FTP 列目录/下载（含空目录回归）"
F_HOME="$WORK_DIR/front-ftp"
reset_front "$F_HOME"
R="$WORK_DIR/results/ftp.json"
if "$PY" "$CLIENT" pty-session --bin "$TM_BIN" \
        --url "ftp://e2e:e2e-pass@127.0.0.1:$P_FTP/" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 7 --action quit \
        --result "$R" >/dev/null; then
    ok "TUI 以带凭据的 FTP URL 启动并正常退出"
else
    bad "FTP TUI 探针失败：$(tail -c 400 "$R" 2>/dev/null | tr '\n' ' ')"
fi
assert_audio_count "$F_HOME/.cache" 4 "FTP 根目录 4 首音频全部下载（含文件名带 ';'）"
assert_no_part "$F_HOME/.cache" "FTP 下载无 .part 残留"
run_queue_snapshot "$WORK_DIR/results/queue-ftp.json"
assert_core_queue "$WORK_DIR/results/queue-ftp.json" "$F_HOME/.cache/ter-music/remote" 4 \
    "FTP 队列 4 条本地缓存路径"
"$PY" "$CLIENT" config >"$WORK_DIR/results/config-ftp.json" 2>/dev/null || echo '{}' >"$WORK_DIR/results/config-ftp.json"
assert_last_opened "$WORK_DIR/results/config-ftp.json" "ftp://127.0.0.1:$P_FTP/" \
    "FTP 的 last_opened_path 已脱敏（不含 userinfo）"
assert_no_secret "$F_HOME" "e2e-pass" "前端 HOME（含 debug 日志/配置/曲库）没有明文密码"
assert_no_secret "$CORE_HOME" "e2e-pass" "核心 HOME（config.xml/library.db）没有明文密码"

info "3b. FTP 空目录不崩溃（F-01 回归）"
F_HOME="$WORK_DIR/front-ftp-empty"
reset_front "$F_HOME"
R="$WORK_DIR/results/ftp-empty.json"
if "$PY" "$CLIENT" pty-session --bin "$TM_BIN" \
        --url "ftp://e2e:e2e-pass@127.0.0.1:$P_FTP/emptydir/" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 4 --action quit \
        --result "$R" >/dev/null; then
    ok "FTP 空目录：TUI 存活并正常退出（无 SIGSEGV）"
else
    bad "FTP 空目录导致 TUI 异常退出：$(tail -c 300 "$R" 2>/dev/null | tr '\n' ' ')"
fi
if [ "$(cache_audio_count "$F_HOME/.cache")" = "0" ]; then
    ok "FTP 空目录没有下载任何文件"
else
    bad "FTP 空目录意外产生了缓存文件"
fi
assert_log_matches "$F_HOME/logs" "no audio files" "FTP 空目录给出明确错误"

info "3c. FTP 子目录 URL 不带尾斜杠也能列目录（F-04）"
F_HOME="$WORK_DIR/front-ftp-subdir"
reset_front "$F_HOME"
R="$WORK_DIR/results/ftp-subdir.json"
if "$PY" "$CLIENT" pty-session --bin "$TM_BIN" \
        --url "ftp://e2e:e2e-pass@127.0.0.1:$P_FTP/sub" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 5 --action quit \
        --result "$R" >/dev/null; then
    ok "FTP 子目录（URL 无尾斜杠）：TUI 正常退出"
else
    bad "FTP 子目录（URL 无尾斜杠）失败：$(tail -c 300 "$R" 2>/dev/null | tr '\n' ' ')"
fi
assert_audio_count "$F_HOME/.cache" 1 "FTP 子目录 /sub 的 1 首音频下载成功（列目录 URL 已补尾斜杠）"

# ── 4) HTTPS 非 443 端口仍走 TLS（F-08） ─────────────────────────
info "4. HTTPS 非 443 端口（TLS 不被降级）"
F_HOME="$WORK_DIR/front-tls"
reset_front "$F_HOME"
R="$WORK_DIR/results/tls.json"
if CURL_CA_BUNDLE="$WORK_DIR/tls-cert.pem" \
   "$PY" "$CLIENT" pty-session --bin "$TM_BIN" --url "https://127.0.0.1:$P_TLS/" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 6 --action quit \
        --env "CURL_CA_BUNDLE=$WORK_DIR/tls-cert.pem" \
        --result "$R" >/dev/null; then
    ok "TUI 通过 TLS 列目录并正常退出（https://host:非443）"
else
    bad "HTTPS 非 443 失败（可能退化为明文 http）：$(tail -c 400 "$R" 2>/dev/null | tr '\n' ' ')"
fi
assert_audio_count "$F_HOME/.cache" 2 "HTTPS 根目录 2 首音频通过 TLS 下载"
run_queue_snapshot "$WORK_DIR/results/queue-tls.json"
assert_core_queue "$WORK_DIR/results/queue-tls.json" "$F_HOME/.cache/ter-music/remote" 2 \
    "HTTPS 队列 2 条本地缓存路径"

# ── 5) SMB：argv 执行（无 shell 注入）+ 假客户端 ─────────────────
info "5. SMB 安全回归（远端文件名注入 + 密码不进 argv/日志）"
SMB_ROOT="$WORK_DIR/smb"
SMB_ARGV_LOG="$WORK_DIR/results/smb-argv.log"
: > "$SMB_ARGV_LOG"
cat > "$WORK_DIR/bin/smbclient" <<'SMBEOF'
#!/usr/bin/env python3
"""E2E 假 smbclient：按 Samba process_command_string 语义解析 -c。

要点（R-02 / t11 R2-01）：
  - 与 Samba 一样，**先按 ';' 切分**（引号解析之前），再分派 cd/ls/get/put；
  - 每个分号段独立 try/except：get 失败只记录该段 failed，继续执行后续段
    （Samba 的真实行为；旧实现首段 get 失败就退出，导致注入段不被执行）；
  - argv/ops 在**分派前**就写日志（并在 finally 再写一次），即使随后被
    SIGTERM 或异常中止，调用记录与解析出的 put/get 也不会丢。
  - 可选：CI 有真实 smbd 时可设置 TM_TEST_REAL_SMB=1 改用真实客户端
    （本容器无 smbd，默认仍用本假客户端做语义回归）。
"""
import json, os, shutil, sys, time

argv = sys.argv[1:]
argv_log = os.environ.get("SMB_ARGV_LOG", "")
auth_file = ""
if "-A" in argv:
    auth_file = argv[argv.index("-A") + 1]
authed = False
if auth_file and os.path.exists(auth_file):
    try:
        with open(auth_file, encoding="utf-8") as handle:
            content = handle.read()
        authed = ("password = " + os.environ.get("SMB_EXPECT_PASS", "")) in content
    except OSError:
        authed = False

command = argv[argv.index("-c") + 1] if "-c" in argv else ""
root = os.environ.get("SMB_ROOT", ".")

def tokenize(text):
    out, cur, quoted, escaped = [], "", False, False
    for ch in text:
        if escaped:
            cur += ch
            escaped = False
        elif ch == "\\":
            escaped = True
        elif ch == '"':
            quoted = not quoted
        elif ch.isspace() and not quoted:
            if cur:
                out.append(cur)
                cur = ""
        else:
            cur += ch
    if cur:
        out.append(cur)
    return out

def write_log(ops_list):
    if not argv_log:
        return
    try:
        with open(argv_log, "a", encoding="utf-8") as handle:
            handle.write(json.dumps({"argv": argv, "authed": authed,
                                     "ops": ops_list}) + "\n")
    except OSError:
        pass

# Samba process_command_string(): 先 strchr(';') 切分（引号解析之前）。
ops = []
for segment in command.split(";"):
    tokens = tokenize(segment)
    if not tokens:
        continue
    ops.append({"cmd": tokens[0].lower(), "args": tokens[1:], "segment": segment})

# 分派前先落盘：即使后续段崩溃/被 kill，注入命令也可被断言检出。
write_log(ops)

rel = "."
exit_status = 0
try:
    for op in ops:
        cmd, args = op["cmd"], op["args"]
        try:
            if cmd == "cd" and args:
                rel = args[0]
            elif cmd == "put":
                marker = os.environ.get("SMB_FAKE_PUT_MARKER", "")
                if marker:
                    with open(marker, "w", encoding="utf-8") as handle:
                        handle.write("PUT-EXECUTED:" + " ".join(args))
                op["executed"] = True
            elif cmd in ("ls", "dir"):
                target = os.path.join(root, rel) if rel != "." else root
                for name in sorted(os.listdir(target)):
                    full = os.path.join(target, name)
                    attrs = "D" if os.path.isdir(full) else "A"
                    size = 0 if os.path.isdir(full) else os.path.getsize(full)
                    sys.stdout.write("  %-40s%7.7s %8.0f  %s\n"
                                     % (name, attrs, size, "Thu Nov 20 12:00:00 2025"))
                op["executed"] = True
            elif cmd == "get" and args:
                src = args[0]
                dst = args[1] if len(args) > 1 else os.path.basename(src)
                source = (os.path.join(root, rel, src) if rel != "."
                          else os.path.join(root, src))
                delay = float(os.environ.get("SMB_FAKE_SLOW", "0") or "0")
                if os.environ.get("SMB_FAKE_OVERSIZE") == "1":
                    with open(dst, "wb") as handle:
                        handle.seek(1024 * 1024 * 1024)   # 1 GiB 稀疏文件
                        handle.write(b"x")
                elif delay > 0:
                    with open(dst, "wb") as handle:
                        handle.write(b"partial")
                        handle.flush()
                        time.sleep(delay)
                    shutil.copyfile(source, dst)
                else:
                    shutil.copyfile(source, dst)
                op["executed"] = True
            elif cmd == "quit":
                op["executed"] = True
            else:
                op["failed"] = "unknown command: " + cmd
                exit_status = 1
        except Exception as exc:                  # noqa: BLE001
            # 该段失败只影响本段；后续 put/get 仍继续执行（Samba 行为）。
            op["failed"] = "%s: %s" % (type(exc).__name__, exc)
            exit_status = 1
finally:
    write_log(ops)

sys.exit(exit_status)
SMBEOF
chmod +x "$WORK_DIR/bin/smbclient"

# ── 夹具自检（t11 R2-01）：旧式 -c 注入必须在假客户端上被检出 ─────
# 直接以旧代码的构造调用假客户端：首段 get 的源不存在（旧实现会立即退出），
# 第二段 put 必须仍执行并写 marker、ops 必须记录 get/put。
SMB_SELF_LOG="$WORK_DIR/results/smb-selfcheck-argv.log"
SMB_SELF_MARKER="$WORK_DIR/results/smb-selfcheck-put-marker"
rm -f "$SMB_SELF_LOG" "$SMB_SELF_MARKER"
SMB_SELF_RC=0
SMB_ARGV_LOG="$SMB_SELF_LOG" SMB_ROOT="$SMB_ROOT" \
SMB_FAKE_PUT_MARKER="$SMB_SELF_MARKER" \
    "$WORK_DIR/bin/smbclient" //self/share -A /dev/null -c \
    'get "x;put secret.txt stolen.txt;z.mp3" "/tmp/smb-selfcheck-out"' \
    >/dev/null 2>&1 || SMB_SELF_RC=$?
# 自检命令预期返回非 0（首段 get 失败），退出码不参与断言；真正断言在 marker/ops。
if [ -f "$SMB_SELF_MARKER" ] && "$PY" - "$SMB_SELF_LOG" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
ops = [op for row in rows for op in (row.get("ops") or [])]
has_put = any(op.get("cmd") == "put" and op.get("executed") for op in ops)
failed_get = any(op.get("cmd") == "get" and op.get("failed") for op in ops)
has_later = any(op.get("cmd") == "get" and (op.get("args") or [""])[0] == "x" for op in ops)
print("has_put=%r failed_get=%r has_x=%r ops=%r" % (has_put, failed_get, has_later, ops))
sys.exit(0 if (has_put and failed_get and has_later) else 1)
PY
then
    ok "假 smbclient 夹具自检：旧式注入的 put 被执行、失败的 get 有记录（夹具敏感）"
else
    bad "假 smbclient 夹具对旧式注入不敏感（R-02 回归仍会假绿）"
fi
rm -f "$SMB_SELF_LOG" "$SMB_SELF_MARKER" /tmp/smb-selfcheck-out

F_HOME="$WORK_DIR/front-smb"
reset_front "$F_HOME"
R="$WORK_DIR/results/smb.json"
# 产品运行前清空注入 marker：断言必须在产品调用之后检查它（vulnerable 树上
# put 会写 marker；若运行后才 rm 再检查，会让注入断言假绿）。
SMB_PUT_MARKER="$WORK_DIR/results/smb-put-executed"
rm -f "$SMB_PUT_MARKER"
# 用独立变量保存“要传给子进程的值”：同一 if 里先赋值再展开同名变量，
# 展开读到的是旧值（SC2097/SC2098），独立变量语义明确、不会误传。
SMB_ROOT_CHILD="$SMB_ROOT"
SMB_ARGV_LOG_CHILD="$SMB_ARGV_LOG"
PATH_CHILD="$WORK_DIR/bin:$PATH"
if SMB_ROOT="$SMB_ROOT_CHILD" SMB_ARGV_LOG="$SMB_ARGV_LOG_CHILD" SMB_EXPECT_PASS="e2e-pass" \
   PATH="$PATH_CHILD" \
   "$PY" "$CLIENT" pty-session --bin "$TM_BIN" \
        --url "smb://e2e-user:e2e-pass@127.0.0.1/share" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 6 --action quit \
        --env "PATH=$PATH_CHILD" \
        --env "SMB_ROOT=$SMB_ROOT_CHILD" --env "SMB_ARGV_LOG=$SMB_ARGV_LOG_CHILD" \
        --env "SMB_EXPECT_PASS=e2e-pass" \
        --env "SMB_FAKE_PUT_MARKER=$WORK_DIR/results/smb-put-executed" \
        --result "$R" >/dev/null; then
    ok "TUI 以 SMB URL 经假 smbclient 完成列目录/下载并正常退出"
else
    bad "SMB TUI 探针失败：$(tail -c 500 "$R" 2>/dev/null | tr '\n' ' ')"
fi
assert_audio_count "$F_HOME/.cache" 2 "SMB 只下载 2 首安全文件名（含 ';' 的条目被拒绝）"
if find "$WORK_DIR" -name 'PWN-SMB' -print -quit 2>/dev/null | grep -q .; then
    bad "SMB 命令注入被执行（发现 PWN-SMB 标记）"
else
    ok "SMB 文件名注入载荷没有执行（无 PWN-SMB 标记）"
fi
if [ -f "$SMB_PUT_MARKER" ] || [ -e "$SMB_ROOT/stolen.txt" ]; then
    bad "SMB 注入的 put 命令被执行（marker/stolen.txt 出现）"
else
    ok "SMB 注入命令没有执行（无 put marker/stolen.txt）"
fi
if grep -q "e2e-pass" "$SMB_ARGV_LOG" 2>/dev/null; then
    bad "smbclient argv 里出现明文密码"
else
    ok "smbclient argv 不含密码（-A 认证文件）"
fi
assert_log_matches "$F_HOME/logs" "unsafe" "含 ';' 的 SMB 文件名被拒绝并记录可操作原因"
if "$PY" - "$SMB_ARGV_LOG" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
errors = []
if not rows:
    errors.append("没有调用 smbclient")
if any(not row.get("authed") for row in rows):
    errors.append("认证文件未被正确读取")
for row in rows:
    joined = " ".join(row.get("argv") or [])
    if ";" in joined:
        errors.append("argv 含 ';'（远端可控命令分隔符进入 -c）")
    for op in row.get("ops") or []:
        if op.get("cmd") == "put":
            errors.append("smbclient 解析出 put 命令")
        if op.get("cmd") == "get":
            source = (op.get("args") or [""])[0]
            if ";" in source or "secret" in source:
                errors.append("get 源含危险文本: " + source)
print("; ".join(errors))
sys.exit(1 if errors else 0)
PY
then
    ok "假 smbclient 按 Samba 语义解析：无 put、无 ';' 进入 -c（R-01/R-02）"
else
    bad "smbclient 命令语义回归失败（注入仍可能执行）"
fi
run_queue_snapshot "$WORK_DIR/results/queue-smb.json"
assert_core_queue "$WORK_DIR/results/queue-smb.json" "$F_HOME/.cache/ter-music/remote" 2 \
    "SMB 队列 2 条本地缓存路径"

info "5b. 缺少 smbclient 时给出可操作错误（F-06）"
F_HOME="$WORK_DIR/front-smb-missing"
reset_front "$F_HOME"
R="$WORK_DIR/results/smb-missing.json"
if "$PY" "$CLIENT" pty-session --bin "$TM_BIN" \
        --url "smb://e2e-user:e2e-pass@127.0.0.1/share" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 4 --action quit \
        --env "PATH=/usr/bin:/bin" \
        --result "$R" >/dev/null; then
    ok "缺 smbclient 时 TUI 存活并正常退出"
else
    bad "缺 smbclient 时 TUI 异常：$(tail -c 300 "$R" 2>/dev/null | tr '\n' ' ')"
fi
assert_audio_count "$F_HOME/.cache" 0 "缺 smbclient 时没有下载任何文件"
assert_log_matches "$F_HOME/logs" "smbclient" "错误信息提到 smbclient"
assert_log_matches "$F_HOME/logs" "install" "错误信息给出安装提示"

info "5c. SMB 取消在途下载：kill 子进程并快速退出（R-03）"
F_HOME="$WORK_DIR/front-smb-cancel"
reset_front "$F_HOME"
SMB_CANCEL_LOG="$WORK_DIR/results/smb-cancel-argv.log"
: > "$SMB_CANCEL_LOG"
R="$WORK_DIR/results/smb-cancel.json"
if SMB_ROOT="$SMB_ROOT" SMB_ARGV_LOG="$SMB_CANCEL_LOG" SMB_EXPECT_PASS="e2e-pass" \
   SMB_FAKE_SLOW=8 PATH="$WORK_DIR/bin:$PATH" \
   "$PY" "$CLIENT" pty-session --bin "$TM_BIN" \
        --url "smb://e2e-user:e2e-pass@127.0.0.1/share" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 2 \
        --action cancel-on-part --part-timeout 20 --result "$R" >/dev/null; then
    ok "SMB 慢速下载：取消后 TUI 正常退出"
else
    bad "SMB 慢速下载取消场景异常：$(tail -c 400 "$R" 2>/dev/null | tr '\n' ' ')"
fi
if "$PY" - "$R" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
errors = []
if not doc.get("alive_before"):
    errors.append("退出前 TUI 已死")
if doc.get("part_seen_ms") is None:
    errors.append("未观察到 SMB 在途 .part")
if (doc.get("exit_ms") or 999999) > 3000:
    errors.append("退出耗时 %r ms > 3000ms（SMB 取消未中断 smbclient）" % doc.get("exit_ms"))
if doc.get("parts_after") != 0:
    errors.append("退出后仍有 %r 个 .part" % doc.get("parts_after"))
print("; ".join(errors))
sys.exit(1 if errors else 0)
PY
then
    ok "SMB 在途下载可取消：3 秒内退出、无 .part（R-03）"
else
    bad "SMB 取消没有及时中断子进程或留下残留"
fi
if pgrep -af "bin/smbclient" | grep -v pgrep | grep -q .; then
    bad "取消后仍残留 smbclient 子进程"
else
    ok "取消后没有残留 smbclient 子进程"
fi

info "5d. SMB 下载硬上限：谎报 size 的 1GiB 文件被删除（R-03）"
F_HOME="$WORK_DIR/front-smb-oversize"
reset_front "$F_HOME"
SMB_OVERSIZE_LOG="$WORK_DIR/results/smb-oversize-argv.log"
: > "$SMB_OVERSIZE_LOG"
R="$WORK_DIR/results/smb-oversize.json"
if SMB_ROOT="$WORK_DIR/smb-oversize" SMB_ARGV_LOG="$SMB_OVERSIZE_LOG" \
   SMB_EXPECT_PASS="e2e-pass" SMB_FAKE_OVERSIZE=1 PATH="$WORK_DIR/bin:$PATH" \
   "$PY" "$CLIENT" pty-session --bin "$TM_BIN" \
        --url "smb://e2e-user:e2e-pass@127.0.0.1/share" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 6 --action quit \
        --result "$R" >/dev/null; then
    ok "SMB 超大下载场景 TUI 存活并正常退出"
else
    bad "SMB 超大下载场景异常"
fi
assert_audio_count "$F_HOME/.cache" 0 "超过 1GiB 的 SMB 下载被删除、不进入缓存"
assert_log_matches "$F_HOME/logs" "1 GiB" "日志明确说明超过 1GiB 上限"

# ── 6) 负例：401 / 404 / 连接拒绝 / 空目录 / 非音频 ───────────────
info "6. 负例（不得崩溃、不得产生缓存、错误可读）"
REFUSED_PORT="$("$PY" - <<'PY'
import socket
sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
)"
negative_case() {   # name url log-pattern label
    local name="$1" url="$2" pattern="$3"
    local home="$WORK_DIR/front-neg-$name"
    local result="$WORK_DIR/results/neg-$name.json"
    reset_front "$home"
    if "$PY" "$CLIENT" pty-session --bin "$TM_BIN" --url "$url" \
            --home "$home" --cache "$home/.cache" --wait 4 --action quit \
            --result "$result" >/dev/null; then
        ok "$name：TUI 存活并正常退出"
    else
        bad "$name：TUI 异常退出（$(tail -c 300 "$result" 2>/dev/null | tr '\n' ' ')）"
    fi
    if [ "$(cache_audio_count "$home/.cache")" = "0" ]; then
        ok "$name：没有生成缓存文件"
    else
        bad "$name：失败时仍产生了缓存文件"
    fi
    assert_log_matches "$home/logs" "$pattern" "$name：错误信息可读（$pattern）"
}
negative_case 401 "http://127.0.0.1:$P_AUTH/" "Authentication failed" "HTTP 401"
negative_case 404 "http://127.0.0.1:$P_HTTP/missing/" "Not found" "HTTP 404"
negative_case refused "http://127.0.0.1:$REFUSED_PORT/" "Cannot connect" "连接拒绝"
negative_case empty "http://127.0.0.1:$P_HTTP/emptydir/" "no audio files" "HTTP 空目录"
negative_case nonaudio "http://127.0.0.1:$P_HTTP/only-text/" "no audio files" "非音频目录"

# ── 7) 取消在途下载（F-09） ──────────────────────────────────────
info "7. 取消在途下载：快速停止、不再交付、无 .part"
F_HOME="$WORK_DIR/front-cancel"
reset_front "$F_HOME"
R="$WORK_DIR/results/cancel.json"
if "$PY" "$CLIENT" pty-session --bin "$TM_BIN" \
        --url "http://127.0.0.1:$P_TRICKLE/" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 3 \
        --action cancel-on-part --part-timeout 15 --result "$R" >/dev/null; then
    ok "取消后 TUI 正常退出"
else
    bad "取消场景 TUI 异常：$(tail -c 400 "$R" 2>/dev/null | tr '\n' ' ')"
fi
if "$PY" - "$R" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
errors = []
if not doc.get("alive_before"):
    errors.append("退出前 TUI 已死")
if doc.get("part_seen_ms") is None:
    errors.append("未观察到在途 .part（无法证明取消的是进行中的传输）")
if doc.get("exit_code") != 0:
    errors.append("退出码=%r" % doc.get("exit_code"))
if (doc.get("exit_ms") or 999999) > 3000:
    errors.append("退出耗时 %r ms > 3000ms（取消未中断传输）" % doc.get("exit_ms"))
if doc.get("parts_after") != 0:
    errors.append("退出后仍有 %r 个 .part" % doc.get("parts_after"))
print("; ".join(errors))
sys.exit(1 if errors else 0)
PY
then
    ok "慢速下载中取消：3 秒内退出、无 .part 残留（F-09）"
else
    bad "取消没有及时中断传输或留下 .part"
fi
run_queue_snapshot "$WORK_DIR/results/queue-cancel.json"
assert_core_queue "$WORK_DIR/results/queue-cancel.json" "$F_HOME/.cache/ter-music/remote" 0 \
    "取消后没有曲目交付到核心队列"

# ── 8) local 后端 F-15：队列条数 == 成功下载数 ────────────────────
info "8. local 后端：远程缓存交付不重复入队（F-15）"
F_HOME="$WORK_DIR/front-local"
reset_front "$F_HOME"
R="$WORK_DIR/results/local.json"
if dbus-run-session -- "$PY" "$CLIENT" pty-session --bin "$TM_BIN" \
        --url "http://127.0.0.1:$P_HTTP/" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --wait 8 \
        --frontend local --query-method queue --action quit \
        --result "$R" >/dev/null; then
    ok "local 后端 TUI 完成远程下载并正常退出"
else
    bad "local 后端 TUI 失败：$(tail -c 400 "$R" 2>/dev/null | tr '\n' ' ')"
fi
if "$PY" - "$R" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
query = doc.get("query") or {}
errors = []
if not query:
    errors.append("没有取到 local 后端的 Queue.Get 结果")
if query.get("count") != 5:
    errors.append("local 队列 count=%r，期望 5（重复入队会变成 6）" % query.get("count"))
rows = query.get("rows") or []
if len(rows) != 5:
    errors.append("local 队列 rows=%d，期望 5" % len(rows))
paths = [row.get("path") or "" for row in rows]
if len(paths) != len(set(paths)):
    errors.append("local 队列出现重复路径")
for path in paths:
    if "://" in path:
        errors.append("local 队列含远程 URL: " + path)
print("; ".join(errors))
sys.exit(1 if errors else 0)
PY
then
    ok "local 后端 5 首 → 队列恰好 5 条无重复"
else
    bad "local 后端队列条数/内容不符合预期（F-15 未修复）"
fi

# ── UX 断言辅助（pty 捕获真实屏幕文本 + 按键导航） ────────────────
UX_PTY="$WORK_DIR/ux_pty.py"
cat > "$UX_PTY" <<'UXEOF'
#!/usr/bin/env python3
"""按步骤驱动 TUI 并捕获屏幕输出（用于断言用户可见的 UX 文本）。"""
import argparse, ast, fcntl, json, os, pty, select, signal, struct, subprocess, sys, termios, time

parser = argparse.ArgumentParser()
parser.add_argument("--bin", required=True)
parser.add_argument("--home", required=True)
parser.add_argument("--cache", default="")
parser.add_argument("--log-dir", default="")
parser.add_argument("--result", required=True)
parser.add_argument("--arg", action="append", default=[])
parser.add_argument("--step", action="append", default=[])
args = parser.parse_args()

home = os.path.realpath(args.home)
cache = os.path.realpath(args.cache or os.path.join(home, ".cache"))
log_dir = os.path.realpath(args.log_dir or os.path.join(home, "logs"))
os.makedirs(home, exist_ok=True)
os.makedirs(cache, exist_ok=True)
os.makedirs(log_dir, exist_ok=True)

env = os.environ.copy()
env.update({
    "HOME": home,
    "XDG_CONFIG_HOME": os.path.join(home, ".config"),
    "XDG_CACHE_HOME": cache,
    "TERM": "xterm-256color",
    "LANG": "C.UTF-8",
    "LC_ALL": "C.UTF-8",
    "TER_MUSIC_LOG_DIR": log_dir,
})
argv = [args.bin] + args.arg
pid, fd = pty.fork()
if pid == 0:  # pragma: no cover
    try:
        os.execvpe(argv[0], argv, env)
    finally:
        os._exit(127)

try:
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 45, 140, 0, 0))
    os.kill(pid, signal.SIGWINCH)
except OSError:
    pass
os.set_blocking(fd, False)
output = bytearray()

def drain(seconds):
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            return
        if not chunk:
            return
        output.extend(chunk)

alive = True
queue_doc = None
queue_error = None
for step in args.step:
    kind, _, value = step.partition(":")
    if kind == "query-queue":
        try:
            completed = subprocess.run([
                "gdbus", "call", "--session",
                "--dest", "org.mpris.MediaPlayer2.ter_music",
                "--object-path", "/org/mpris/MediaPlayer2",
                "--method", "cn.org.huansoft.ter_music.Queue.Get", "0", "100",
            ], capture_output=True, text=True, timeout=15)
            if completed.returncode != 0:
                raise RuntimeError(completed.stderr.strip())
            value_obj = ast.literal_eval(completed.stdout.strip())
            if isinstance(value_obj, tuple):
                value_obj = value_obj[0] if value_obj else ""
            queue_doc = json.loads(value_obj)
        except Exception as exc:                  # noqa: BLE001
            queue_error = str(exc)
    elif kind == "sleep":
        drain(float(value))
    elif kind == "send":
        try:
            os.write(fd, value.encode("utf-8"))
        except OSError:
            pass
        drain(0.12)
    elif kind == "alive":
        wpid, _status = os.waitpid(pid, os.WNOHANG)
        if wpid != 0:
            alive = False
            break

if alive:
    wpid, _status = os.waitpid(pid, os.WNOHANG)
    if wpid != 0:
        alive = False

try:
    os.write(fd, b"q")
except OSError:
    pass

code = None
deadline = time.time() + 8
while time.time() < deadline:
    wpid, status = os.waitpid(pid, os.WNOHANG)
    if wpid != 0:
        code = os.waitstatus_to_exitcode(status) if hasattr(os, "waitstatus_to_exitcode") else 0
        break
    drain(0.1)
if code is None:
    try:
        os.kill(pid, 9)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass
drain(0.2)

doc = {
    "alive": alive,
    "exit_code": code,
    "output": output.decode("utf-8", "replace"),
    "queue": queue_doc,
    "queue_error": queue_error,
}
with open(args.result, "w", encoding="utf-8") as handle:
    json.dump(doc, handle, ensure_ascii=False)
sys.exit(0 if alive and code == 0 else 1)
UXEOF

K_UP=$'\eOA'; K_DOWN=$'\eOB'; K_RIGHT=$'\eOC'; K_LEFT=$'\eOD'
K_ESC=$'\e'; K_ENTER=$'\n'; K_F2=$'\eOQ'; K_BS=$'\x7f'

write_remote_xml() {   # home protocol host port use_tls [insecure_tls]
    mkdir -p "$1/.config/ter-music"
    cat > "$1/.config/ter-music/remote.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-remote version="1">
  <connections>
    <connection>
      <name>ux</name>
      <protocol>$2</protocol>
      <host>$3</host>
      <port>$4</port>
      <username></username>
      <password></password>
      <private_key_path></private_key_path>
      <base_path>/</base_path>
      <known_hosts_path></known_hosts_path>
      <insecure_tls>${6:-0}</insecure_tls>
      <use_tls>$5</use_tls>
    </connection>
  </connections>
  <cache limit_mb="2048"/>
</ter-music-remote>
EOF
}

assert_output_contains() {   # result-json needle label
    if "$PY" - "$1" "$2" <<'PYEOF'
import json, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
sys.exit(0 if sys.argv[2] in (doc.get("output") or "") else 1)
PYEOF
    then
        ok "$3"
    else
        bad "$3（屏幕输出里没有“$2”）"
    fi
}

# 重试夹具：第一次列目录 404，之后成功
RETRY_ROOT="$WORK_DIR/retry"
RETRY_PORT_FILE="$WORK_DIR/port-retry.txt"
"$PY" - "$RETRY_ROOT" "$RETRY_PORT_FILE" <<'RETRYEOF' &
import http.server, os, sys, urllib.parse
root, port_file = sys.argv[1], sys.argv[2]
state = {"requests": 0}
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a):
        pass
    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/":
            state["requests"] += 1
            if state["requests"] == 1:
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            body = (b"<html><body><a href=\"retry1.wav\">retry1.wav</a>"
                    b"</body></html>")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/retry1.wav":
            data = open(os.path.join(root, "retry1.wav"), "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        self.send_error(404)
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
with open(port_file, "w", encoding="utf-8") as handle:
    handle.write("PORT=%d\n" % server.server_address[1])
server.serve_forever()
RETRYEOF
SERVER_PIDS+=("$!")
for _ in $(seq 1 200); do
    [ -s "$RETRY_PORT_FILE" ] && break
    sleep 0.05
done
P_RETRY="$(sed -n 's/^PORT=//p' "$RETRY_PORT_FILE")"
[ -n "$P_RETRY" ] || { bad "重试夹具启动失败"; KEEP=1; exit 1; }

info "10. UX：本地化错误提示（401）"
F_HOME="$WORK_DIR/front-ux-401"
reset_front "$F_HOME"
write_remote_xml "$F_HOME" 4 127.0.0.1 "$P_AUTH" 0
R="$WORK_DIR/results/ux-401.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "$WORK_DIR/http" \
        --step "sleep:2.6" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.4" \
        --step "send:$K_ENTER" --step "sleep:1.8" \
        --step "alive" >/dev/null; then
    ok "401 会话：TUI 存活并正常退出"
else
    bad "401 会话：TUI 异常退出"
fi
assert_output_contains "$R" "认证失败" "401 在界面上给出本地化、可操作提示"

info "11. UX：可见进度 + 缓存入口 + 取消当前任务 + 清理确认"
F_HOME="$WORK_DIR/front-ux-cancel"
reset_front "$F_HOME"
write_remote_xml "$F_HOME" 4 127.0.0.1 8080 0
R="$WORK_DIR/results/ux-cancel.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "http://127.0.0.1:$P_TRICKLE/" \
        --step "sleep:2.6" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.8" \
        --step "send:$K_ENTER" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_ENTER" --step "sleep:1.5" \
        --step "send:$K_LEFT" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.3" \
        --step "send:$K_ENTER" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.8" \
        --step "alive" >/dev/null; then
    ok "下载中可进入远程页、取消任务并清理缓存，TUI 存活到退出"
else
    bad "进度/取消/清理流程异常（TUI 提前退出）"
fi
assert_output_contains "$R" "下载中" "下载期间界面显示可判定的进度"
assert_output_contains "$R" "缓存" "远程页显示缓存管理入口"
assert_output_contains "$R" "清理缓存" "缓存清理入口可见"
assert_output_contains "$R" "缓存上限" "缓存上限设置入口可见"
assert_output_contains "$R" "取消当前任务" "进行中的会话有“取消当前任务”动作"
assert_output_contains "$R" "已取消" "取消后界面明确显示已取消"
assert_output_contains "$R" "确认清理" "清理缓存需要二次确认"
assert_output_contains "$R" "已清理" "确认后执行清理并显示结果"

info "12. UX：列目录失败进入错误态并可重试"
F_HOME="$WORK_DIR/front-ux-retry"
reset_front "$F_HOME"
write_remote_xml "$F_HOME" 4 127.0.0.1 "$P_RETRY" 0
R="$WORK_DIR/results/ux-retry.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "$WORK_DIR/http" \
        --step "sleep:2.6" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.4" \
        --step "send:$K_ENTER" --step "sleep:1.6" \
        --step "send:$K_ENTER" --step "sleep:1.6" \
        --step "send:$K_ENTER" --step "sleep:2.4" \
        --step "alive" >/dev/null; then
    ok "列目录失败后可重试并继续下载，TUI 存活到退出"
else
    bad "错误态/重试流程异常（TUI 提前退出）"
fi
assert_output_contains "$R" "重试列目录" "列目录失败后显示错误态与“重试列目录”"
assert_output_contains "$R" "远程路径不存在" "404 失败原因已本地化（不是永久“正在加载”）"
assert_output_contains "$R" "retry1.wav" "重试后目录内容正常显示"
assert_audio_count "$F_HOME/.cache" 1 "重试成功后目录里的音频被下载"

# 下载失败后重试夹具：flaky.wav 第一次 503，第二次成功
RETRY_DL_ROOT="$WORK_DIR/retry-dl"
RETRY_DL_PORT_FILE="$WORK_DIR/port-retry-dl.txt"
"$PY" - "$RETRY_DL_ROOT" "$RETRY_DL_PORT_FILE" <<'RETRYDLEOF' &
import http.server, os, sys, urllib.parse
root, port_file = sys.argv[1], sys.argv[2]
state = {"flaky": 0}
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a):
        pass
    def send_file(self, name, content_type="audio/wav"):
        data = open(os.path.join(root, name), "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/":
            body = (b"<html><body>"
                    b"<a href=\"ok.wav\">ok.wav</a>"
                    b"<a href=\"flaky.wav\">flaky.wav</a>"
                    b"</body></html>")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/ok.wav":
            self.send_file("ok.wav")
            return
        if path == "/flaky.wav":
            state["flaky"] += 1
            if state["flaky"] == 1:
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_file("flaky.wav")
            return
        self.send_error(404)
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
with open(port_file, "w", encoding="utf-8") as handle:
    handle.write("PORT=%d\n" % server.server_address[1])
server.serve_forever()
RETRYDLEOF
SERVER_PIDS+=("$!")
for _ in $(seq 1 200); do
    [ -s "$RETRY_DL_PORT_FILE" ] && break
    sleep 0.05
done
P_RETRY_DL="$(sed -n 's/^PORT=//p' "$RETRY_DL_PORT_FILE")"
[ -n "$P_RETRY_DL" ] || { bad "下载重试夹具启动失败"; KEEP=1; exit 1; }

info "12b. UX：失败曲目可重试（下载失败后重试成功）"
F_HOME="$WORK_DIR/front-ux-retry-dl"
reset_front "$F_HOME"
write_remote_xml "$F_HOME" 4 127.0.0.1 "$P_RETRY_DL" 0
R="$WORK_DIR/results/ux-retry-dl.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "$WORK_DIR/http" \
        --step "sleep:2.6" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.4" \
        --step "send:$K_ENTER" --step "sleep:1.2" \
        --step "send:$K_ENTER" --step "sleep:3.2" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.3" \
        --step "send:$K_ENTER" --step "sleep:2.6" \
        --step "alive" >/dev/null; then
    ok "下载失败后进入远程页重试，TUI 存活到退出"
else
    bad "失败曲目重试流程异常"
fi
assert_output_contains "$R" "重试失败曲目" "下载失败后提供“重试失败曲目”动作"
assert_audio_count "$F_HOME/.cache" 2 "重试失败曲目后两首缓存齐全"

info "13. UX：TLS 开关持久化（重启后保持）+ 删除连接二次确认"
F_HOME="$WORK_DIR/front-ux-tls"
reset_front "$F_HOME"
write_remote_xml "$F_HOME" 4 127.0.0.1 8443 0
REMOTE_XML="$F_HOME/.config/ter-music/remote.xml"

ux_tls_navigate() {   # result, extra-steps...
    local result="$1"; shift
    "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$result" \
        --arg "$WORK_DIR/http" \
        --step "sleep:2.6" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.4" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "sleep:0.3" \
        --step "send:$K_ENTER" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "sleep:0.3" \
        "$@" --step "alive" >/dev/null 2>&1
}

# 第一次：打开 TLS 开关并保存
if ux_tls_navigate "$WORK_DIR/results/ux-tls-1.json" \
        --step "send:+" --step "sleep:0.3" --step "send:s" --step "sleep:0.9"; then
    ok "TLS 表单保存流程：TUI 存活到正常退出"
else
    bad "TLS 表单保存流程异常（TUI 提前退出/崩溃）"
fi
if grep -q '<use_tls>1</use_tls>' "$REMOTE_XML" 2>/dev/null; then
    ok "TLS 开关经表单保存到 remote.xml"
else
    bad "TLS 开关没有写入 remote.xml"
fi

# 第二次（重启后）：进入编辑表单直接保存，use_tls 必须仍为 1（load→save 往返）
if ux_tls_navigate "$WORK_DIR/results/ux-tls-2.json" \
        --step "send:s" --step "sleep:0.9"; then
    ok "重启后保存流程：TUI 存活到正常退出"
else
    bad "重启后保存流程异常"
fi
if grep -q '<use_tls>1</use_tls>' "$REMOTE_XML" 2>/dev/null; then
    ok "重启后 TLS 开关保持（load→save 往返仍为 1）"
else
    bad "重启后 TLS 开关丢失"
fi

# 删除连接需要二次确认；ESC 取消不删除
R="$WORK_DIR/results/ux-delete.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "$WORK_DIR/http" \
        --step "sleep:2.6" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.4" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "sleep:0.3" \
        --step "send:$K_ENTER" --step "sleep:0.6" \
        --step "send:$K_ESC" --step "sleep:0.4" \
        --step "alive" >/dev/null; then
    ok "删除确认流程中 TUI 存活到退出"
else
    bad "删除确认流程异常"
fi
assert_output_contains "$R" "确认删除连接" "删除连接需要二次确认"
if grep -q '>ux<' "$REMOTE_XML" 2>/dev/null; then
    ok "ESC 取消删除后连接仍在"
else
    bad "取消删除后连接丢失"
fi

# ── 收尾与残留检查 ────────────────────────────────────────────────
info "15. 主视图远程状态行（t5 V-03：进度/失败在主视图可见）"
F_HOME="$WORK_DIR/front-main-progress"
reset_front "$F_HOME"
R="$WORK_DIR/results/main-progress.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "http://127.0.0.1:$P_TRICKLE/" \
        --step "sleep:3.0" --step "alive" >/dev/null; then
    ok "主视图进度场景 TUI 存活到退出"
else
    bad "主视图进度场景异常"
fi
assert_output_contains "$R" "下载中" "主视图状态行显示下载进度（无需进入设置页）"

F_HOME="$WORK_DIR/front-main-failure"
reset_front "$F_HOME"
R="$WORK_DIR/results/main-failure.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "http://127.0.0.1:$P_AUTH/" \
        --step "sleep:3.0" --step "alive" >/dev/null; then
    ok "主视图失败场景 TUI 存活到退出"
else
    bad "主视图失败场景异常"
fi
assert_output_contains "$R" "认证失败" "主视图状态行显示本地化失败原因"

F_HOME="$WORK_DIR/front-bulk"
reset_front "$F_HOME"
R="$WORK_DIR/results/main-bulk.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "http://127.0.0.1:$P_HTTP/bulk/" \
        --step "sleep:15.0" --step "alive" >/dev/null; then
    ok ">200 首目录场景 TUI 存活到退出"
else
    bad ">200 首目录场景异常"
fi
assert_output_contains "$R" "仅加载 200/205" "主视图显示截断提示（仅加载 200/205，t5 V-02）"
assert_audio_count "$F_HOME/.cache" 200 "截断后实际缓存 200 首"

info "16. SFTP 安全：known_hosts 缺失必须 fail-closed（t5 F-13）"
SFTP_PORT_FILE="$WORK_DIR/port-sftp-blackhole.txt"
SFTP_COUNT_FILE="$WORK_DIR/sftp-blackhole-count.txt"
rm -f "$SFTP_PORT_FILE" "$SFTP_COUNT_FILE"
"$PY" - "$SFTP_PORT_FILE" "$SFTP_COUNT_FILE" <<'BLACKHOLEEOF' &
import socket, sys
port_file, count_file = sys.argv[1], sys.argv[2]
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", 0))
listener.listen(8)
with open(port_file, "w", encoding="utf-8") as handle:
    handle.write("PORT=%d\n" % listener.getsockname()[1])
count = 0
while True:
    conn, _addr = listener.accept()
    count += 1
    with open(count_file, "w", encoding="utf-8") as handle:
        handle.write(str(count))
    conn.close()
BLACKHOLEEOF
SERVER_PIDS+=("$!")
for _ in $(seq 1 200); do
    [ -s "$SFTP_PORT_FILE" ] && break
    sleep 0.05
done
P_SFTP="$(sed -n 's/^PORT=//p' "$SFTP_PORT_FILE")"
F_HOME="$WORK_DIR/front-sftp-knownhosts"
reset_front "$F_HOME"
R="$WORK_DIR/results/sftp-knownhosts.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg=--debug --arg "sftp://e2e@127.0.0.1:$P_SFTP/" \
        --step "sleep:3.0" --step "alive" >/dev/null; then
    ok "SFTP known_hosts 缺失场景 TUI 存活到退出"
else
    bad "SFTP known_hosts 缺失场景异常"
fi
assert_output_contains "$R" "known_hosts" "缺少 known_hosts 时给出可操作提示（known_hosts/ssh-keyscan）"
assert_log_matches "$F_HOME/logs" "SFTP rejected" "引擎在连接前明确拒绝（SFTP rejected）"
if [ -f "$SFTP_COUNT_FILE" ] && [ "$(cat "$SFTP_COUNT_FILE")" != "0" ]; then
    bad "known_hosts 缺失时仍向服务器发起了连接（count=$(cat "$SFTP_COUNT_FILE")）"
else
    ok "known_hosts 缺失时没有发起任何 TCP 连接（fail-closed）"
fi

info "17. 同进程重复打开同一目录（t5 V-01：队列=成功下载数、无重复行）"
F_HOME="$WORK_DIR/front-repeat-open"
reset_front "$F_HOME"
write_remote_xml "$F_HOME" 4 127.0.0.1 "$P_HTTP" 0
R="$WORK_DIR/results/repeat-open.json"
if dbus-run-session -- "$PY" "$UX_PTY" --bin "$TM_BIN" \
        --home "$F_HOME" --cache "$F_HOME/.cache" --log-dir "$F_HOME/logs" \
        --result "$R" \
        --arg=--frontend=local --arg "$WORK_DIR/empty-music" \
        --step "sleep:2.6" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.4" \
        --step "send:$K_ENTER" --step "sleep:1.2" \
        --step "send:$K_ENTER" --step "sleep:3.2" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.4" \
        --step "send:$K_ENTER" --step "sleep:1.2" \
        --step "send:$K_ENTER" --step "sleep:3.2" \
        --step "query-queue" --step "sleep:0.3" --step "alive" >/dev/null; then
    ok "同进程重复打开同一目录：TUI 存活到退出"
else
    bad "同进程重复打开同一目录流程异常"
fi
if "$PY" - "$R" <<'REPEATPY'
import json, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
queue = doc.get("queue") or {}
rows = queue.get("rows") or []
errors = []
if doc.get("queue_error"):
    errors.append("Queue.Get 失败: " + str(doc.get("queue_error")))
if queue.get("count") != 5:
    errors.append("队列 count=%r，期望 5（重复打开会变成 10）" % queue.get("count"))
if len(rows) != 5:
    errors.append("队列 rows=%d，期望 5" % len(rows))
paths = [row.get("path") or "" for row in rows]
if len(paths) != len(set(paths)):
    errors.append("队列出现重复行")
for path in paths:
    if "://" in path:
        errors.append("队列含远程 URL: " + path)
print("; ".join(errors))
sys.exit(1 if errors else 0)
REPEATPY
then
    ok "重复打开后队列=5（成功下载数）且无重复行"
else
    bad "重复打开后队列条数/内容不符合预期（V-01 未修复）"
fi

info "13b. 表单校验/保存失败：真实可失败断言（R-09）"
F_HOME="$WORK_DIR/front-form-invalid"
reset_front "$F_HOME"
write_remote_xml "$F_HOME" 4 127.0.0.1 8080 0
REMOTE_XML="$F_HOME/.config/ter-music/remote.xml"
R="$WORK_DIR/results/form-invalid-port.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "$WORK_DIR/http" \
        --step "sleep:2.6" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.4" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "sleep:0.3" --step "send:$K_ENTER" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "sleep:0.3" --step "send:$K_ENTER" --step "sleep:0.5" \
        --step "send:$K_BS" --step "send:$K_BS" --step "send:$K_BS" --step "send:$K_BS" \
        --step "send:99999" --step "sleep:0.3" --step "send:$K_ENTER" --step "sleep:0.3" \
        --step "send:s" --step "sleep:0.8" --step "alive" >/dev/null; then
    ok "非法端口表单：TUI 存活到正常退出"
else
    bad "非法端口表单流程异常"
fi
assert_output_contains "$R" "端口必须" "非法端口保存被拒绝并给出提示（不再静默保存）"
if grep -q '<port>8080</port>' "$REMOTE_XML" 2>/dev/null; then
    ok "非法端口没有被写入 remote.xml"
else
    bad "非法端口被写入 remote.xml（校验未生效）"
fi

F_HOME="$WORK_DIR/front-form-savefail"
reset_front "$F_HOME"
write_remote_xml "$F_HOME" 4 127.0.0.1 8080 0
REMOTE_XML="$F_HOME/.config/ter-music/remote.xml"
chmod 0555 "$F_HOME/.config/ter-music"
R="$WORK_DIR/results/form-save-fail.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "$WORK_DIR/http" \
        --step "sleep:2.6" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.4" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "sleep:0.3" --step "send:$K_ENTER" --step "sleep:0.5" \
        --step "send:s" --step "sleep:0.8" --step "alive" >/dev/null; then
    ok "只读配置目录保存失败：TUI 存活到正常退出"
else
    bad "保存失败流程异常"
fi
assert_output_contains "$R" "保存远程配置失败" "保存失败显示可操作错误（不再静默失败）"
chmod 0755 "$F_HOME/.config/ter-music"

info "19. FTPS TLS 选项接线（R-04：CA/insecure_tls 对 FTP 生效）"
start_ftps_probe() {   # result_file -> sets P_FTPS
    local result="$1"
    local port_file
    port_file="$WORK_DIR/port-ftps-$(date +%s%N).txt"
    rm -f "$result" "$port_file"
    "$PY" - "$WORK_DIR/tls-cert.pem" "$WORK_DIR/tls-key.pem" "$port_file" "$result" <<'FTPSPY' &
import socket, ssl, sys
cert, key, port_file, result_file = sys.argv[1:]
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert, key)
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", 0))
listener.listen(1)
with open(port_file, "w", encoding="utf-8") as handle:
    handle.write("PORT=%d\n" % listener.getsockname()[1])
conn, _addr = listener.accept()
try:
    tls = context.wrap_socket(conn, server_side=True)
    with open(result_file, "w", encoding="utf-8") as handle:
        handle.write("tls=1\n")
    try:
        tls.settimeout(2)
        tls.recv(128)
    except Exception:
        pass
    tls.close()
except Exception as exc:                          # noqa: BLE001
    with open(result_file, "w", encoding="utf-8") as handle:
        handle.write("tls=0 error=%s\n" % exc)
FTPSPY
    SERVER_PIDS+=("$!")
    for _ in $(seq 1 200); do
        [ -s "$port_file" ] && break
        sleep 0.05
    done
    P_FTPS="$(sed -n 's/^PORT=//p' "$port_file")"
}

# A) 显式 CURL_CA_BUNDLE 必须应用到 ftps：握手成功 = 自签名 CA 被采纳
FTPS_A_RESULT="$WORK_DIR/results/ftps-ca.txt"
start_ftps_probe "$FTPS_A_RESULT"
F_HOME="$WORK_DIR/front-ftps-ca"
reset_front "$F_HOME"
R="$WORK_DIR/results/ftps-ca.json"
if CURL_CA_BUNDLE="$WORK_DIR/tls-cert.pem" \
   "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "ftps://127.0.0.1:$P_FTPS/" \
        --step "sleep:3.0" --step "alive" >/dev/null; then
    ok "FTPS+CA 场景 TUI 存活到退出"
else
    bad "FTPS+CA 场景异常"
fi
if grep -q '^tls=1' "$FTPS_A_RESULT" 2>/dev/null; then
    ok "FTPS 使用 CURL_CA_BUNDLE 完成 TLS 握手（CAINFO 已接线）"
else
    bad "FTPS 没有使用 CA（$(cat "$FTPS_A_RESULT" 2>/dev/null)）"
fi
if grep -Rqi "certificate verify" "$F_HOME/logs" 2>/dev/null; then
    bad "FTPS+正确 CA 仍报证书校验失败"
else
    ok "FTPS+正确 CA 没有证书校验错误"
fi

# B) insecure_tls=1 时自签名无 CA 也握手成功（设置页 FTP 连接）
FTPS_B_RESULT="$WORK_DIR/results/ftps-insecure.txt"
start_ftps_probe "$FTPS_B_RESULT"
F_HOME="$WORK_DIR/front-ftps-insecure"
reset_front "$F_HOME"
write_remote_xml "$F_HOME" 2 127.0.0.1 "$P_FTPS" 1 1
R="$WORK_DIR/results/ftps-insecure.json"
if "$PY" "$UX_PTY" --bin "$TM_BIN" --home "$F_HOME" --cache "$F_HOME/.cache" \
        --log-dir "$F_HOME/logs" --result "$R" \
        --arg "$WORK_DIR/http" \
        --step "sleep:2.6" \
        --step "send:$K_F2" --step "sleep:0.5" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "send:$K_DOWN" \
        --step "send:$K_DOWN" --step "send:$K_DOWN" --step "sleep:0.4" \
        --step "send:$K_RIGHT" --step "sleep:0.5" \
        --step "send:$K_ENTER" --step "sleep:0.4" \
        --step "send:$K_ENTER" --step "sleep:3.0" \
        --step "alive" >/dev/null; then
    ok "FTPS insecure_tls 场景 TUI 存活到退出"
else
    bad "FTPS insecure_tls 场景异常"
fi
if grep -q '^tls=1' "$FTPS_B_RESULT" 2>/dev/null; then
    ok "FTPS insecure_tls=1 接受自签名证书（校验开关已接线）"
else
    bad "FTPS insecure_tls 未生效（$(cat "$FTPS_B_RESULT" 2>/dev/null)）"
fi

info "20. 收尾：停止核心/夹具，检查无残留进程与临时缓存"
for pid in "${SERVER_PIDS[@]:-}"; do
    [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
done
if [ -n "$CORE_PID" ]; then
    kill "$CORE_PID" 2>/dev/null || true
    wait "$CORE_PID" 2>/dev/null || true
fi
SERVER_PIDS=()
CORE_PID=""
sleep 0.3

leftover_pids=()
for pid_path in /proc/[0-9]*; do
    pid="${pid_path#/proc/}"
    [ "$pid" = "$$" ] && continue
    [ "$pid" = "$PPID" ] && continue
    [ -r "$pid_path/environ" ] || continue
    if tr '\0' '\n' < "$pid_path/environ" 2>/dev/null | grep -qF "$WORK_DIR"; then
        leftover_pids+=("$pid")
    fi
done
# 夹具进程的命令行里也带工作目录（它们的 env 不一定带），一并检查
while IFS= read -r pid; do
    [ -n "$pid" ] || continue
    [ "$pid" = "$$" ] && continue
    [ "$pid" = "$PPID" ] && continue
    skip=0
    for existing in "${leftover_pids[@]:-}"; do
        [ "$existing" = "$pid" ] && skip=1 && break
    done
    [ "$skip" -eq 1 ] || leftover_pids+=("$pid")
done < <(pgrep -f "$WORK_DIR" 2>/dev/null || true)
if [ "${#leftover_pids[@]}" -eq 0 ]; then
    ok "没有残留进程持有本次工作目录"
else
    bad "仍有残留进程：${leftover_pids[*]}（$(ps -o pid=,comm= -p "${leftover_pids[@]}" 2>/dev/null | tr '\n' ' ')）"
fi

if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ]; then
    rm -rf "$WORK_DIR"
    ok "临时工作目录已清理"
elif [ "$KEEP" -eq 1 ]; then
    echo "（工作目录保留在 $WORK_DIR）"
fi
WORK_DIR=""

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
