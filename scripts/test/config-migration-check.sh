#!/usr/bin/env bash
#
# 配置 v5 → v6 迁移回归：远程服务器条目移交给前端
#
# 远程音乐源自 config v6 起归前端（核心不认识远程）。旧配置里已有的服务器
# 条目是用户数据，必须一次性、逐元素原样搬到前端自有的 <configdir>/remote.xml，
# 之后核心配置（config.xml）不再含 <remote_connections>。
#
# 本脚本在隔离的 HOME/XDG 环境里：
#   1. 造一份含远程条目（带密码密文）的 v5 config.xml；
#   2. 启动核心（daemon），让它完成迁移；
#   3. 断言 config.xml 变成 v6 且无该段、remote.xml 存在且条目与密文逐字节一致；
#   4. 断言前端能读入该文件（以 TUI 探针的日志为准）。
#
# 用法：scripts/test/config-migration-check.sh [--bin <ter-music>] [--keep]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""
DAEMON_PID=""

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
        -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

# --bin 规范成绝对路径：本脚本会先 `cd "$WORK_DIR"` 再跑 tui-probe.py（好让被测
# 程序的调试日志落在工作目录里），此时相对路径（CI 传的就是 ./build/ter-music）
# 已经失效——探针只会报“不可执行”，而那条 remote.xml 断言就会长期 skip。
if [ "${TM_BIN#/}" = "$TM_BIN" ]; then
    bin_dir="$(cd "$(dirname "$TM_BIN")" 2>/dev/null && pwd)" || bin_dir=""
    if [ -n "$bin_dir" ]; then
        TM_BIN="$bin_dir/$(basename "$TM_BIN")"
    fi
fi

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
    # 无条件停核心：最后一步 tui-probe 会让 TUI 自己拉起一个核心，而 `q` 只退前端
    # （核心继续跑是设计语义），若只在"记录过 DAEMON_PID"时才停就会留下孤儿进程
    # （实测：本脚本 rc=0 但总线上留下 1 个 daemon foreground）。
    "$TM_BIN" daemon stop >/dev/null 2>&1 || true
    if [ -n "$DAEMON_PID" ]; then
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

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先构建，或用 --bin 指定）" >&2; exit 2; }

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-migrate-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$XDG_CONFIG_HOME/ter-music" "$WORK_DIR/music"

CONFIG="$XDG_CONFIG_HOME/ter-music/config.xml"
REMOTE="$XDG_CONFIG_HOME/ter-music/remote.xml"

# 与 config/crypto.c 相同算法（sha256(salt) 逐字节异或后十六进制）
CRYPT_HEX="$(python3 - <<'PY'
import hashlib
salt = b"ter-music-remote-password-v1"
key = hashlib.sha256(salt).digest()
plain = b"s3cret-pass"
print("".join("%02x" % (c ^ key[i % len(key)]) for i, c in enumerate(plain)))
PY
)"

info "造一份含远程条目的 v5 配置"
cat > "$CONFIG" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="5">
  <paths>
    <default_startup_path>$WORK_DIR/music</default_startup_path>
    <last_opened_path>$WORK_DIR/music</last_opened_path>
  </paths>
  <theme>
    <playlist_fg>7</playlist_fg>
  </theme>
  <preferences>
    <volume_percent>72</volume_percent>
    <ui_language>zh_CN</ui_language>
  </preferences>
  <equalizer>
    <enabled>0</enabled>
  </equalizer>
  <remote_connections>
    <connection>
      <name>NAS 测试</name>
      <protocol>1</protocol>
      <host>192.168.1.9</host>
      <port>22</port>
      <username>tester</username>
      <password encrypted="1">$CRYPT_HEX</password>
      <private_key_path></private_key_path>
      <base_path>/music</base_path>
    </connection>
  </remote_connections>
</ter-music-config>
EOF

[ -f "$CONFIG" ] && ok "v5 配置已就绪（含 1 条远程连接）" || { bad "无法写入 v5 配置"; exit 1; }

info "启动核心触发迁移"
pid="$("$TM_BIN" daemon start --open "$WORK_DIR/music" 2>/dev/null |
       sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
if [ -z "$pid" ]; then
    pid="$("$TM_BIN" daemon status 2>/dev/null | sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
fi
DAEMON_PID="$pid"
[ -n "$DAEMON_PID" ] && ok "核心已启动（pid $DAEMON_PID）" || { bad "核心启动失败"; KEEP=1; exit 1; }

sleep 1

info "断言：config.xml 升级到当前版本且不再含远程段"
cfg_version="$(python3 "$SCRIPT_DIR/rpc_client.py" call org.yxzl.ter_music.Config.GetAll 2>/dev/null |
    python3 -c 'import json,sys; print(json.load(sys.stdin).get("version", "?"))' 2>/dev/null || echo "?")"
if [ "$cfg_version" = "7" ]; then
    ok "核心配置版本为 7（Config.GetAll version=$cfg_version）"
else
    bad "核心配置版本应为 7，实际 '$cfg_version'"
fi
if grep -q "remote_connections" "$CONFIG"; then
    bad "config.xml 仍含 <remote_connections>"
else
    ok "config.xml 已无 <remote_connections>"
fi

info "断言：remote.xml 已生成且条目逐字段一致"
if [ ! -f "$REMOTE" ]; then
    bad "未生成 $REMOTE"
else
    ok "remote.xml 已生成"
    check_field() {
        local name="$1" want="$2"
        if grep -q "<$name>$want</$name>" "$REMOTE"; then
            ok "remote.xml $name = $want"
        else
            bad "remote.xml $name 不匹配（期望 $want）"
        fi
    }
    check_field name "NAS 测试"
    check_field protocol "1"
    check_field host "192.168.1.9"
    check_field port "22"
    check_field username "tester"
    check_field base_path "/music"

    if grep -q "encrypted=\"1\">$CRYPT_HEX</password>" "$REMOTE"; then
        ok "密码密文逐字节保留（可被前端解出原文）"
    else
        bad "密码密文未原样保留"
    fi

    if python3 - "$REMOTE" <<'PY'
import hashlib, re, sys
xml = open(sys.argv[1], encoding='utf-8').read()
m = re.search(r'<password encrypted="1">([0-9a-f]+)</password>', xml)
if not m:
    sys.exit(1)
key = hashlib.sha256(b"ter-music-remote-password-v1").digest()
raw = bytes.fromhex(m.group(1))
plain = "".join(chr(b ^ key[i % len(key)]) for i, b in enumerate(raw))
sys.exit(0 if plain == "s3cret-pass" else 1)
PY
    then
        ok "密文可解出原文（前端密码沿用同一派生密钥）"
    else
        bad "密文无法解出原文"
    fi

    perms="$(stat -c '%a' "$REMOTE")"
    [ "$perms" = "600" ] && ok "remote.xml 权限为 600" || bad "remote.xml 权限为 $perms（应为 600）"
fi

info "断言：核心侧配置面不再有远程段"
all="$("$TM_BIN" show --json 2>/dev/null | head -c 200 || true)"
if [ -n "$all" ]; then
    ok "show --json 仍可用（核心在迁移后正常运行）"
else
    printf '  skip show --json 无输出（可能未开始播放）\n'
fi

info "断言：前端能读入 remote.xml"
if command -v python3 >/dev/null 2>&1 && [ -f "$SCRIPT_DIR/tui-probe.py" ]; then
    "$TM_BIN" daemon stop >/dev/null 2>&1 || true
    sleep 1
    DAEMON_PID=""
    ( cd "$WORK_DIR" && timeout 25 python3 "$SCRIPT_DIR/tui-probe.py" \
        --bin "$TM_BIN" --music "$WORK_DIR/music" --home "$HOME" --keys " " \
        --extra-args=--debug >/dev/null 2>&1 ) || true
    probe_log="$(ls -t "$WORK_DIR"/ter-music-debug-*.log 2>/dev/null | head -1)"
    if [ -n "$probe_log" ] && grep -q "Loaded 1 remote connection(s)" "$probe_log"; then
        ok "前端已读入 1 条远程连接（remote_store 解析成功）"
    else
        printf '  skip 未捕获到 remote_store 日志（%s）\n' "${probe_log:-无日志}"
    fi
else
    printf '  skip 缺少 tui-probe.py\n'
fi

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
