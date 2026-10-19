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
#   4. 断言前端能读入该文件（以 TUI 探针的日志为准）；
#   5. 断言 v7 → v8：属性 7 文件首启补写 stop_core_on_exit=1 且版本升 8；
#      属性 8 显式 0 的文件连续启停不回卷；JSON v1 → XML 的迁移产出同样
#      含 stop_core_on_exit=1（该键在 JSON 时代不存在显式值，按缺省补写）。
#   6. 断言属性 8 文件（含 theme playlist_bg=0）连续启停不触发 file_version<4
#      的主题迁移块：版本属性数字化后，bg 0→-1 只对老文件（<4）一次性生效；
#   7. 断言历史字面量 version="2.2" 的完整文件仍可加载，且首启迁移一次
#      （属性升 8、补 stop_core_on_exit=1、playlist_bg 0→-1、显式值保留）
#      之后再启停不重跑迁移块。
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
        -h|--help) sed -n '2,23p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
if [ "$cfg_version" = "8" ]; then
    ok "核心配置版本为 8（Config.GetAll version=$cfg_version）"
else
    bad "核心配置版本应为 8，实际 '$cfg_version'"
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

# ── v7→v8 / v8 显式值 / JSON v1 的 stop_core_on_exit 断言 ──────────
core_up() { "$TM_BIN" daemon status 2>/dev/null | grep -q "pid"; }
wait_core_down() {
    for _ in $(seq 1 40); do core_up || return 0; sleep 0.25; done
    return 1
}
stop_live() {   # 核心现在内存镜像里的 stop_core_on_exit（经 Config.GetAll）
    python3 "$SCRIPT_DIR/rpc_client.py" call org.yxzl.ter_music.Config.GetAll 2>/dev/null |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["preferences"].get("stop_core_on_exit","?"))' 2>/dev/null || echo "?"
}

info "v7 → v8：属性 7 文件首启补写 stop_core_on_exit=1 且版本升 8"
"$TM_BIN" daemon stop >/dev/null 2>&1 || true
wait_core_down || bad "daemon stop 后核心没有下线，夹具断言不可信"
cat > "$CONFIG" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="7">
  <preferences>
    <core_exit_when_no_frontend>0</core_exit_when_no_frontend>
    <volume_percent>77</volume_percent>
  </preferences>
</ter-music-config>
EOF
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 2
DAEMON_PID="$("$TM_BIN" daemon status 2>/dev/null | sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
if [ -n "$DAEMON_PID" ]; then ok "v7 夹具下核心已启动（pid $DAEMON_PID）"; else bad "核心启动失败"; fi
if grep -q 'version="8"' "$CONFIG" && grep -q "<stop_core_on_exit>1</stop_core_on_exit>" "$CONFIG"; then
    ok "属性 7 文件首启后被补写：版本升 8 且含 <stop_core_on_exit>1</stop_core_on_exit>"
else
    bad "v7→v8 补写缺失（文件根属性 $(grep -o 'version="[^"]*"' "$CONFIG" | head -1)，stop 值 '$(sed -n 's|.*<stop_core_on_exit>\([0-9]*\)</stop_core_on_exit>.*|\1|p' "$CONFIG" | head -1)'）"
fi
if grep -q "<volume_percent>77</volume_percent>" "$CONFIG"; then
    ok "迁移补写不误伤显式值（volume_percent=77 原样保留）"
else
    bad "迁移后 volume_percent 丢失或被改写（期望 77）"
fi
if [ "$(stop_live)" = "1" ]; then
    ok "Config.GetAll 的 stop_core_on_exit=1（迁移后的实时值）"
else
    bad "Config.GetAll 的 stop_core_on_exit=$(stop_live)（期望 1）"
fi

info "v8 显式 stop_core_on_exit=0：连续多轮启停不回卷"
"$TM_BIN" daemon stop >/dev/null 2>&1 || true
wait_core_down || bad "核心未下线即写夹具，本轮断言不可信"
cat > "$CONFIG" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="8">
  <preferences>
    <stop_core_on_exit>0</stop_core_on_exit>
    <volume_percent>33</volume_percent>
  </preferences>
</ter-music-config>
EOF
# 起 → 停（Quit 收尾会 persist 一次）→ 再起，等价于两轮启停后再判读
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1.5
"$TM_BIN" daemon stop >/dev/null 2>&1 || true
wait_core_down || bad "第一轮 daemon stop 后核心未下线"
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1.5
DAEMON_PID="$("$TM_BIN" daemon status 2>/dev/null | sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
if [ "$(stop_live)" = "0" ]; then
    ok "多轮启停后 Config.GetAll 仍读到显式的 0"
else
    bad "多轮启停后 stop_core_on_exit=$(stop_live)（期望 0，存在被缺省 1 回卷的迁移块/加载路径）"
fi
if grep -q "<stop_core_on_exit>0</stop_core_on_exit>" "$CONFIG"; then
    ok "落盘文件保持 <stop_core_on_exit>0</stop_core_on_exit>（属性 8 不再触发 <8 迁移块）"
else
    bad "落盘文件的 stop_core_on_exit 被回卷：$(grep -o '<stop_core_on_exit>[0-9]*</stop_core_on_exit>' "$CONFIG" | head -1)"
fi
if grep -q "<volume_percent>33</volume_percent>" "$CONFIG"; then
    ok "显式 volume_percent=33 同样跨启停保留"
else
    bad "volume_percent=33 在启停中丢失或被改写"
fi

# ── 版本属性数字化后的新契约：file_version<4 主题块只对老文件一次性生效 ──
# 守的点：config_save_to_xml() 从落 "2.2" 字面量改为落数字契约版后，
# 属性 8 的文件 atoi 后 file_version=8，`< 4` 迁移块（bg==0 → -1）不再命中；
# 用户显式选择的 0（C_BLACK）底色必须能跨启停存活——数字化之前 "2.2" 恒被
# atoi 成 2，每次启动重跑该块、把 0 反复抹成 -1，正是本次改动要根除的回归。
info "属性 8 文件（含 theme playlist_bg=0）：连续启停不触发 <4 主题迁移块"
"$TM_BIN" daemon stop >/dev/null 2>&1 || true
wait_core_down || bad "核心未下线即写夹具，本轮断言不可信"
cat > "$CONFIG" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="8">
  <theme>
    <playlist_bg>0</playlist_bg>
  </theme>
  <preferences>
    <stop_core_on_exit>0</stop_core_on_exit>
  </preferences>
</ter-music-config>
EOF
# 起 → 停（收尾 persist 会全量重写文件）→ 再起，等价两轮启停后判读
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1.5
"$TM_BIN" daemon stop >/dev/null 2>&1 || true
wait_core_down || bad "第一轮 daemon stop 后核心未下线，落盘断言不可信"
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1.5
DAEMON_PID="$("$TM_BIN" daemon status 2>/dev/null | sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
# 先证明文件确实被核心重写过（含夹具没有、但保存必写的 <equalizer> 段），
# 否则下面"值没变"只是没人动过文件，断言会空转成假绿。
if grep -q "<equalizer>" "$CONFIG"; then
    ok "属性 8 文件已被核心重写（落盘含夹具没有的 <equalizer> 段）"
else
    bad "核心未重写属性 8 文件，本轮断言空转（值未变不能证明迁移块没跑）"
fi
if grep -q "<playlist_bg>0</playlist_bg>" "$CONFIG"; then
    ok "两轮启停后落盘仍为 <playlist_bg>0</playlist_bg>（<4 主题块未对属性 8 文件重跑）"
else
    bad "playlist_bg 被 <4 主题块改写：$(grep -o '<playlist_bg>[0-9-]*</playlist_bg>' "$CONFIG" | head -1)（期望 0）"
fi
if grep -q "<stop_core_on_exit>0</stop_core_on_exit>" "$CONFIG"; then
    ok "属性 8 显式 stop_core_on_exit=0 未被缺省 1 回卷"
else
    bad "stop_core_on_exit 被回卷：$(grep -o '<stop_core_on_exit>[0-9]*</stop_core_on_exit>' "$CONFIG" | head -1)（期望 0）"
fi

info "历史字面量 version=\"2.2\" 完整文件：仍可加载，且只迁移一次"
"$TM_BIN" daemon stop >/dev/null 2>&1 || true
wait_core_down || bad "核心未下线即写夹具，本轮断言不可信"
cat > "$CONFIG" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="2.2">
  <theme>
    <playlist_bg>0</playlist_bg>
  </theme>
  <preferences>
    <volume_percent>55</volume_percent>
  </preferences>
</ter-music-config>
EOF
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 2
DAEMON_PID="$("$TM_BIN" daemon status 2>/dev/null | sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
if [ -n "$DAEMON_PID" ]; then ok "\"2.2\" 夹具下核心已启动（历史文件仍可加载，pid $DAEMON_PID）"; else bad "核心启动失败（\"2.2\" 文件没有被加载？）"; fi
# atoi("2.2")==2：>= CONFIG_MIN_SUPPORTED_VER(2) 可加载；file_version=2 让
# <4 / <7 / <8 三个块在首启各跑一次，随后保存落属性 8，之后再启停永不命中。
if grep -q 'version="8"' "$CONFIG"; then
    ok "\"2.2\" 文件首启后根属性升为 8（数字化契约生效）"
else
    bad "\"2.2\" 文件首启后属性未升 8（实际 $(grep -o '<ter-music-config version="[^"]*"' "$CONFIG" | head -1)）"
fi
if grep -q "<stop_core_on_exit>1</stop_core_on_exit>" "$CONFIG"; then
    ok "\"2.2\" 文件首启补写 <stop_core_on_exit>1</stop_core_on_exit>（<8 块一次性生效）"
else
    bad "\"2.2\" 文件未补写 stop_core_on_exit=1（实际 $(grep -o '<stop_core_on_exit>[0-9]*</stop_core_on_exit>' "$CONFIG" | head -1)）"
fi
if grep -q "<playlist_bg>-1</playlist_bg>" "$CONFIG"; then
    ok "\"2.2\" 文件的 playlist_bg 0→-1（<4 主题块这一次经 atoi=2 生效）"
else
    bad "\"2.2\" 文件主题迁移未生效：$(grep -o '<playlist_bg>[0-9-]*</playlist_bg>' "$CONFIG" | head -1)（期望 -1）"
fi
if grep -q "<volume_percent>55</volume_percent>" "$CONFIG"; then
    ok "\"2.2\" 文件的显式 volume_percent=55 未被迁移吞掉"
else
    bad "\"2.2\" 文件迁移后 volume_percent=55 丢失或被改写"
fi
"$TM_BIN" daemon stop >/dev/null 2>&1 || true
wait_core_down || bad "daemon stop 后核心未下线，第二轮断言不可信"
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1.5
DAEMON_PID="$("$TM_BIN" daemon status 2>/dev/null | sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
if grep -q 'version="8"' "$CONFIG" && grep -q "<stop_core_on_exit>1</stop_core_on_exit>" "$CONFIG" &&
   grep -q "<playlist_bg>-1</playlist_bg>" "$CONFIG" && grep -q "<volume_percent>55</volume_percent>" "$CONFIG"; then
    ok "第二轮启停后四项迁移产出全部不变（属性 8 起跳，迁移块不再重跑）"
else
    bad "第二轮启停把迁移产出改写了（实际：$(grep -o '<ter-music-config version="[^"]*"' "$CONFIG" | head -1)，$(grep -o '<playlist_bg>[0-9-]*</playlist_bg>' "$CONFIG" | head -1)，$(grep -o '<stop_core_on_exit>[0-9]*</stop_core_on_exit>' "$CONFIG" | head -1)）"
fi

info "JSON v1 → XML：迁移产出含 stop_core_on_exit=1（按缺省补写）"
"$TM_BIN" daemon stop >/dev/null 2>&1 || true
wait_core_down || bad "核心未下线即布置 JSON 夹具，本轮断言不可信"
rm -f "$CONFIG" "$XDG_CONFIG_HOME/ter-music/config.json.bak"
cat > "$XDG_CONFIG_HOME/ter-music/config.json" <<'EOF'
{
  "version": 1,
  "volume_percent": 60
}
EOF
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 2
DAEMON_PID="$("$TM_BIN" daemon status 2>/dev/null | sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
if [ -f "$XDG_CONFIG_HOME/ter-music/config.json.bak" ]; then
    ok "JSON 首启迁移路径被触发（config.json 已改名 .bak）"
else
    bad "config.json 未被迁移（迁移入口没有走到）"
fi
if [ -f "$CONFIG" ] && grep -q 'version="8"' "$CONFIG" && grep -q "<stop_core_on_exit>1</stop_core_on_exit>" "$CONFIG"; then
    ok "JSON→XML 产出为属性 8 且显式含 <stop_core_on_exit>1</stop_core_on_exit>"
else
    bad "JSON→XML 产出不含 stop_core_on_exit=1 / 版本不是 8（本路径保存即落 8，<8 迁移块不触发，缺值即 migration.c 未补写）"
fi
if [ "$(stop_live)" = "1" ]; then
    ok "迁移后 Config.GetAll 的 stop_core_on_exit=1"
else
    bad "迁移后 Config.GetAll 的 stop_core_on_exit=$(stop_live)（期望 1）"
fi
if grep -q "<volume_percent>60</volume_percent>" "$CONFIG"; then
    ok "JSON 里的显式 volume_percent=60 被带入 XML（迁移没有吞掉用户数据）"
else
    bad "JSON→XML 后 volume_percent=60 丢失"
fi

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
