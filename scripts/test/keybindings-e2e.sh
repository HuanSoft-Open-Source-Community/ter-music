#!/usr/bin/env bash
#
# 可配置键位端到端回归（config v9）
#
# 覆盖：
#   A 持久化与默认键失效：config.xml 覆盖 track.next=']'，并写入一条被锁定的
#     app.quit='ctrl+q'；播放中按 ']' 切下一曲、按默认 'n' 不再切曲；
#     Ctrl+Q 不退出（锁定覆盖被忽略），'q' 始终退出。
#   B 重启保持：同一临时配置再次启动，track.next 改绑仍然生效，q 可退出。
#   C 恢复默认：删除 <keybindings> 后 `keys check` 返回 0，`keys list --json`
#     中 track.next 回到默认 'n'。
#   D 终端兼容：同一配置分别以 TERM=xterm-256color 与 TERM=linux 启动，
#     发送各自 terminfo 的方向键（由 lifecycle-driver.py 按 --term 选取），
#     都能移动列表并播放第二首；q 在两种 TERM 下都能退出。
#   E 设置页编辑：锁定动作不再出现在列表，编辑首个可绑定动作并保存。
#   F Config.Set：合法覆盖生效；锁定动作新增覆盖被拒绝，default 允许清理。
#
# 隔离：临时 HOME/XDG_CONFIG_HOME/XDG_DATA_HOME/XDG_CACHE_HOME + 私有会话总线。
#
# 用法：scripts/test/keybindings-e2e.sh [--bin <ter-music>] [--keep]
# 环境：需要会话总线（未设置时用 dbus-run-session）、ffmpeg 造素材、
#       python3 与 infocmp（缺失时跳过 TERM=linux 兼容断言）。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""
DRIVER_PID=""

usage() { sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

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

# ── 独占会话总线（与既有 E2E 相同约定）────────────────────────────
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ "${TM_TEST_REUSE_SESSION_BUS:-0}" != "1" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：需要 dbus-run-session，或设 TM_TEST_REUSE_SESSION_BUS=1 复用当前会话总线。" >&2
        exit 2
    fi
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

# 子 shell / cd 之后相对 --bin 会失效，先规范成绝对路径
if command -v realpath >/dev/null 2>&1; then
    TM_BIN_ABS="$(realpath -- "$TM_BIN" 2>/dev/null || true)"
    [ -n "$TM_BIN_ABS" ] && TM_BIN="$TM_BIN_ABS"
fi
[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先用 --bin 指定）" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "错误：需要 python3" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo "错误：需要 ffmpeg 造测试音频" >&2; exit 2; }

DRIVER="$SCRIPT_DIR/lifecycle-driver.py"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-keys-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_DATA_HOME="$WORK_DIR/home/.local/share"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
MUSIC="$WORK_DIR/music"
CFG_DIR="$XDG_CONFIG_HOME/ter-music"
CFG="$CFG_DIR/config.xml"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$CFG_DIR" "$MUSIC"

cleanup() {
    [ -n "$DRIVER_PID" ] && kill -9 "$DRIVER_PID" 2>/dev/null || true
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

# 两首可区分的音频：列表按文件名排序，01-alpha 在前、02-bravo 在后
tone() {   # $1 = 频率, $2 = 路径
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "sine=frequency=$1:duration=30" -c:a pcm_s16le -y "$2" 2>/dev/null
}
tone 330 "$MUSIC/01-alpha.wav"
tone 550 "$MUSIC/02-bravo.wav"

# ── 观测助手 ──────────────────────────────────────────────────────
snap_field() {   # $1 = 对 show --json 文档求值的 Python 表达式
    "$TM_BIN" show --json 2>/dev/null | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
    print(eval(sys.argv[1], {"doc": doc}))
except Exception:
    print("?")' "$1" 2>/dev/null || echo "?"
}

wait_field() {   # $1 = 表达式, $2 = 期望值, $3 = 超时秒
    local expr="$1" want="$2" limit="${3:-15}" i
    for i in $(seq 1 $((limit * 2))); do
        [ "$(snap_field "$expr")" = "$want" ] && return 0
        sleep 0.5
    done
    return 1
}

daemon_up() { "$TM_BIN" daemon status 2>/dev/null | grep -q "pid"; }
wait_core_down() {
    local i
    for i in $(seq 1 80); do
        daemon_up || return 0
        sleep 0.25
    done
    return 1
}

# ── TUI 会话（lifecycle-driver.py drive）───────────────────────────
CTL=""
RC=""
PIDFILE=""

start_session() {   # $1 = TERM
    local term="$1"
    CTL="$WORK_DIR/keys-$term.cmd"
    RC="$WORK_DIR/keys-$term.rc"
    PIDFILE="$WORK_DIR/keys-$term.pid"
    : > "$CTL"
    rm -f "$RC" "$PIDFILE"
    python3 "$DRIVER" drive "$TM_BIN" "$MUSIC" "$WORK_DIR/tui-$term.log" \
        --ctl "$CTL" --rc-file "$RC" --pid-file "$PIDFILE" \
        --term "$term" --max-runtime 90 >/dev/null 2>&1 &
    DRIVER_PID=$!
}

send() { printf 'key:%s\n' "$1" >> "$CTL"; }
tick() { sleep "${1:-1}"; }

tui_alive() {
    local pid
    pid="$(cat "$PIDFILE" 2>/dev/null || true)"
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

wait_session_stop() {   # 驱动在 TUI 退出后会写 rc 并自行结束
    local i
    for i in $(seq 1 30); do
        [ -n "$DRIVER_PID" ] && kill -0 "$DRIVER_PID" 2>/dev/null || return 0
        sleep 0.5
    done
    return 1
}

finish_session() {   # 等驱动收尾并返回 TUI 退出码（写到 stdout）
    if ! wait_session_stop; then
        printf '?'
        return 1
    fi
    cat "$RC" 2>/dev/null || true
}

# ── 配置夹具 ──────────────────────────────────────────────────────
write_config_with_bindings() {
    cat > "$CFG" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <volume_percent>80</volume_percent>
  </preferences>
  <keybindings>
    <binding action="app.quit" key="ctrl+q"/>
    <binding action="track.next" key="]"/>
  </keybindings>
</ter-music-config>
EOF
}

write_default_config() {
    cat > "$CFG" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
</ter-music-config>
EOF
}

keys_json_field() {   # $1 = action id, $2 = 字段（keys/overridden/defaults）
    "$TM_BIN" keys list --json 2>/dev/null | python3 -c '
import json, sys
doc = json.load(sys.stdin)
for item in doc.get("actions", []):
    if item.get("id") == sys.argv[1]:
        value = item.get(sys.argv[2])
        if isinstance(value, list):
            print("/".join(value))
        else:
            print(str(value).lower())
        break
else:
    print("?")' "$1" "$2" 2>/dev/null || echo "?"
}

# ── A：持久化与默认键失效 ─────────────────────────────────────────
info "A 改绑生效：track.next=']'；app.quit='ctrl+q' 被锁定并忽略"
write_config_with_bindings

if [ "$(keys_json_field track.next keys)" != "]" ]; then
    bad "keys list 未读到 track.next 的覆盖键 ']'（实测 $(keys_json_field track.next keys)）"
else
    ok "keys list 读到 track.next=']'（覆盖）"
fi
if [ "$(keys_json_field app.quit keys)" != "q/Q/f9" ]; then
    bad "keys list 未回退 app.quit 默认键（实测 $(keys_json_field app.quit keys)）"
else
    ok "keys list 中 app.quit 保持默认 q/Q/f9（锁定覆盖被忽略）"
fi
if "$TM_BIN" keys check --json 2>/dev/null | grep -q '"locked_overrides":1'; then
    ok "keys check 报告 locked_overrides=1"
else
    bad "keys check 未报告锁定动作覆盖"
fi

start_session xterm-256color
if ! wait_field 'doc["track"]["queue_count"]' 2 30; then
    bad "TUI 启动后核心队列不是 2 首（实测 $(snap_field 'doc["track"]["queue_count"]')）"
    printf 'kill\n' >> "$CTL" || true
    wait_session_stop || true
    DRIVER_PID=""
else
    ok "TUI 把 2 首测试音频下发给核心"

    tick 0.5
    send "j"                 # 浏览器树首行是根目录节点，一次 j 落到第一个文件
    tick 2
    send "<Enter>"           # 播放 01-alpha
    if wait_field 'doc["track"]["path"].rsplit("/", 1)[-1]' "01-alpha.wav" 15; then
        ok "Enter 播放了第一首 01-alpha.wav"
    else
        bad "Enter 后未播放 01-alpha.wav（实测 $(snap_field 'doc["track"]["path"]')）"
    fi

    send "]"
    if wait_field 'doc["track"]["path"].rsplit("/", 1)[-1]' "02-bravo.wav" 15; then
        ok "按 ']' 切到下一曲 02-bravo.wav（track.next 改绑生效）"
    else
        bad "按 ']' 没有切到 02-bravo.wav（实测 $(snap_field 'doc["track"]["path"]')）"
    fi

    before_n="$(snap_field 'doc["track"]["path"]')"
    send "n"                 # 默认键已失效
    tick 2.5
    after_n="$(snap_field 'doc["track"]["path"]')"
    if [ "$after_n" = "$before_n" ] && [ -n "$before_n" ] && [ "$before_n" != "?" ]; then
        ok "按默认键 'n' 不再切曲（仍为 ${after_n##*/}）"
    else
        bad "按 'n' 仍然切曲：${before_n##*/} → ${after_n##*/}（默认键未失效）"
    fi

    send "\\x11"             # Ctrl+Q：锁定覆盖被忽略，不得退出
    tick 1.5
    if tui_alive; then
        ok "按 Ctrl+Q 后 TUI 仍存活（app.quit 锁定，旧覆盖被忽略）"
    else
        bad "按 Ctrl+Q 后 TUI 已退出（锁定覆盖意外生效）"
    fi

    send "q"                 # 系统保留退出键始终有效
    if code="$(finish_session)"; then
        if [ "$code" = "0" ]; then
            ok "按 q 正常退出（退出码 0）"
        else
            bad "按 q 退出码为 '$code'（期望 0）"
        fi
    else
        bad "q 之后驱动会话没有收尾（TUI 可能未退出）"
        kill -9 "$DRIVER_PID" 2>/dev/null || true
    fi
    DRIVER_PID=""
fi
wait_core_down || bad "A：TUI 退出后核心仍未离线，后续用例可能接上残留实例"

# ── B：重启保持 ───────────────────────────────────────────────────
info "B 重启保持：同一配置再次启动，改绑仍生效"
start_session xterm-256color
if ! wait_field 'doc["track"]["queue_count"]' 2 30; then
    bad "B：TUI 启动后核心队列不是 2 首"
    printf 'kill\n' >> "$CTL" || true
    wait_session_stop || true
    DRIVER_PID=""
else
    if [ "$(keys_json_field track.next keys)" = "]" ]; then
        ok "B：重启后 track.next 仍为 ']'（覆盖已持久化）"
    else
        bad "B：重启后 track.next 未保持 ']'（实测 $(keys_json_field track.next keys)）"
    fi
    send "q"                 # 退出键锁定，q 始终有效
    if code="$(finish_session)"; then
        if [ "$code" = "0" ]; then
            ok "B：重启后按 q 正常退出（退出码 0）"
        else
            bad "B：重启后按 q 退出码为 '$code'（期望 0）"
        fi
    else
        bad "B：q 之后驱动会话没有收尾"
        kill -9 "$DRIVER_PID" 2>/dev/null || true
    fi
    DRIVER_PID=""
fi
wait_core_down || bad "B：TUI 退出后核心仍未离线"

# ── C：恢复默认 ───────────────────────────────────────────────────
info "C 删除覆盖后恢复默认"
write_default_config

if check_out="$("$TM_BIN" keys check --json 2>/dev/null)"; then
    check_rc=0
else
    check_rc=$?
fi
if [ "$check_rc" -eq 0 ]; then
    ok "keys check 返回 0"
else
    bad "keys check 返回 $check_rc（期望 0）：$(printf '%s' "${check_out:-}" | head -c 300)"
fi
check_ok="$(printf '%s' "${check_out:-}" | python3 -c '
import json, sys
try:
    print(str(json.load(sys.stdin).get("ok")).lower())
except Exception:
    print("?")' 2>/dev/null || echo "?")"
if [ "$check_ok" = "true" ]; then
    ok "keys check --json 报告 ok=true"
else
    bad "keys check --json 未报告 ok=true（实测 '$check_ok'）"
fi

if [ "$(keys_json_field track.next keys)" = "n" ]; then
    ok "删除覆盖后 track.next 恢复默认键 'n'"
else
    bad "删除覆盖后 track.next 未恢复默认（实测 $(keys_json_field track.next keys)）"
fi
if [ "$(keys_json_field track.next overridden)" = "false" ]; then
    ok "track.next 的 overridden=false"
else
    bad "track.next 仍标记为覆盖（overridden=$(keys_json_field track.next overridden)）"
fi

# ── D：终端兼容（xterm-256color / linux）──────────────────────────
info "D 终端兼容：方向键与退出键在两种 TERM 下都生效"
write_config_with_bindings

if ! command -v infocmp >/dev/null 2>&1; then
    skip "infocmp 不可用，无法按 terminfo 生成 TERM=linux 的键序列"
else
    for term in xterm-256color linux; do
        start_session "$term"
        if ! wait_field 'doc["track"]["queue_count"]' 2 30; then
            bad "$term：TUI 启动后核心队列不是 2 首"
            printf 'kill\n' >> "$CTL" || true
            wait_session_stop || true
            DRIVER_PID=""
            wait_core_down || true
            continue
        fi
        # 初始选中根目录节点：两次下移到第二个文件，Enter 应播放 02-bravo
        tick 0.5
        send "<down>"
        tick 1
        send "<down>"
        tick 2
        send "<enter>"
        if wait_field 'doc["track"]["path"].rsplit("/", 1)[-1]' "02-bravo.wav" 15; then
            ok "$term：terminfo 方向键 <down> 生效（Enter 播放第二首 02-bravo.wav）"
        else
            bad "$term：<down> 未生效或 Enter 未播放第二首（实测 $(snap_field 'doc["track"]["path"]')）"
        fi

        send "q"             # 退出键锁定，q 始终有效
        if code="$(finish_session)"; then
            if [ "$code" = "0" ]; then
                ok "$term：q 正常退出（退出码 0）"
            else
                bad "$term：q 退出码为 '$code'（期望 0）"
            fi
        else
            bad "$term：q 之后驱动会话没有收尾"
            kill -9 "$DRIVER_PID" 2>/dev/null || true
        fi
        DRIVER_PID=""
        wait_core_down || bad "$term：TUI 退出后核心仍未离线"
    done
fi

# ── E：设置页编辑并保存（remote 前端）─────────────────────────────
# 回归：GetAll 回填镜像时若不清空 keybindings 覆盖，synced_config 会把
# 用户刚改的键误记为“核心基线”，保存时 diff 为空、配置丢失。
info "E 设置页编辑首个可绑定动作 library.toggle=x 并保存（remote 前端）"
write_default_config
start_session xterm-256color
if ! wait_field 'doc["track"]["queue_count"]' 2 30; then
    bad "E：TUI 启动后核心队列不是 2 首"
    printf 'kill\n' >> "$CTL" || true
    wait_session_stop || true
    DRIVER_PID=""
else
    send "<f2>"
    tick 1
    for _ in 1 2 3 4; do send "<down>"; tick 0.4; done
    send "<enter>"           # 进入快捷键内容区
    tick 1
    send "<enter>"           # 捕获首个可绑定动作 library.toggle（锁定动作已隐藏）
    tick 1
    send "x"                 # 绑定 x
    tick 1
    send "s"                 # 保存
    tick 2
    send "q"                 # 退出键锁定，q 始终有效
    if code="$(finish_session)"; then
        if [ "$code" = "0" ]; then
            ok "E：设置页保存后按 q 正常退出（退出码 0）"
        else
            bad "E：设置页保存后按 q 退出码为 '$code'（期望 0）"
        fi
    else
        bad "E：设置页保存后驱动会话没有收尾"
        kill -9 "$DRIVER_PID" 2>/dev/null || true
    fi
    DRIVER_PID=""
    if grep -q '<binding action="library.toggle" key="x"/>' "$CFG" 2>/dev/null; then
        ok "E：config.xml 已持久化 library.toggle=x"
    else
        bad "E：config.xml 未持久化 library.toggle=x（设置页保存丢失）"
    fi
    if [ "$(keys_json_field library.toggle keys)" = "x" ]; then
        ok "E：keys list 读到 library.toggle=x"
    else
        bad "E：keys list 未读到 library.toggle=x（实测 $(keys_json_field library.toggle keys)）"
    fi
fi
wait_core_down || bad "E：TUI 退出后核心仍未离线"

# ── F：D-Bus Config.Set 的原子性与 default 语义 ────────────────────
info "F Config.Set keybindings：合法生效 / 非法整体失败 / default 恢复"
write_default_config
"$TM_BIN" daemon start >/dev/null 2>&1 || true
for _ in $(seq 1 40); do daemon_up && break; sleep 0.25; done
if ! daemon_up; then
    bad "F：核心没有启动，无法验证 Config.Set"
else
    RPC=("$SCRIPT_DIR/rpc_client.py")
    if python3 "${RPC[@]}" call org.yxzl.ter_music.Config.Set \
        'json:{"keybindings":{"track.next":"]"}}' >/dev/null 2>&1; then
        ok "F：合法 keybindings Set 返回成功"
    else
        bad "F：合法 keybindings Set 被拒绝"
    fi
    if "$TM_BIN" config get 2>/dev/null | grep -q 'track.next'; then
        ok "F：GetAll/config get 读到 track.next 覆盖"
    else
        bad "F：GetAll/config get 未读到 track.next 覆盖"
    fi

    if python3 "${RPC[@]}" call org.yxzl.ter_music.Config.Set \
        'json:{"keybindings":{"no.such.action":"x"}}' >/dev/null 2>&1; then
        bad "F：未知 action 的 Set 未被拒绝"
    else
        ok "F：未知 action 的 Set 整体失败"
    fi
    if python3 "${RPC[@]}" call org.yxzl.ter_music.Config.Set \
        'json:{"keybindings":{"list.up":"j"}}' >/dev/null 2>&1; then
        bad "F：冲突键位的 Set 未被拒绝"
    else
        ok "F：冲突键位的 Set 整体失败"
    fi
    # 跨上下文遮蔽：父层覆盖绑子层默认键、子层覆盖抢父层默认键均须整体拒绝。
    if python3 "${RPC[@]}" call org.yxzl.ter_music.Config.Set \
        'json:{"keybindings":{"volume.down":"i"}}' >/dev/null 2>&1; then
        bad "F：遮蔽键位 volume.down→i（被 queue.insert_next 遮蔽）的 Set 未被拒绝"
    else
        ok "F：遮蔽键位 volume.down→i 的 Set 整体失败"
    fi
    if python3 "${RPC[@]}" call org.yxzl.ter_music.Config.Set \
        'json:{"keybindings":{"queue.append":"+"}}' >/dev/null 2>&1; then
        bad "F：子层抢父层键 queue.append→+（抢 volume.up 的 +）的 Set 未被拒绝"
    else
        ok "F：子层抢父层键 queue.append→+ 的 Set 整体失败"
    fi
    # 无冲突覆盖（默认键固有遮蔽不拦截）应被接受，随后清理。
    if python3 "${RPC[@]}" call org.yxzl.ter_music.Config.Set \
        'json:{"keybindings":{"volume.up":"t"}}' >/dev/null 2>&1; then
        ok "F：无冲突覆盖 volume.up→t 的 Set 生效"
        if python3 "${RPC[@]}" call org.yxzl.ter_music.Config.Set \
            'json:{"keybindings":{"volume.up":"default"}}' >/dev/null 2>&1; then
            ok "F：volume.up 的 default 清理成功"
        else
            bad "F：volume.up 的 default 清理被拒"
        fi
    else
        bad "F：无冲突覆盖 volume.up→t 的 Set 被误拒"
    fi
    if python3 "${RPC[@]}" call org.yxzl.ter_music.Config.Set \
        'json:{"keybindings":{"app.quit":"x"}}' >/dev/null 2>&1; then
        bad "F：锁定动作 app.quit 的 Set 未被拒绝"
    else
        ok "F：锁定动作 app.quit 的 Set 整体失败"
    fi
    if "$TM_BIN" config get 2>/dev/null | grep -q 'track.next'; then
        ok "F：失败的 Set 未破坏已有覆盖（原子性）"
    else
        bad "F：失败的 Set 破坏了已有覆盖"
    fi
    if python3 "${RPC[@]}" call org.yxzl.ter_music.Config.Set \
        'json:{"keybindings":{"app.quit":"default"}}' >/dev/null 2>&1; then
        ok "F：锁定动作 app.quit 的 default 清理被允许"
    else
        bad "F：锁定动作 app.quit 的 default 清理被拒绝"
    fi

    if python3 "${RPC[@]}" call org.yxzl.ter_music.Config.Set \
        'json:{"keybindings":{"track.next":"default"}}' >/dev/null 2>&1; then
        ok "F：default 删除覆盖返回成功"
    else
        bad "F：default 删除覆盖被拒绝"
    fi
    if "$TM_BIN" config get 2>/dev/null | grep -q 'track.next'; then
        bad "F：default 之后 track.next 覆盖仍存在"
    else
        ok "F：default 之后 track.next 覆盖已删除"
    fi
fi
"$TM_BIN" daemon stop >/dev/null 2>&1 || true
wait_core_down || true

info "结果"
printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -ne 0 ]; then
    KEEP=1
    printf '（工作目录保留在 %s）\n' "$WORK_DIR" >&2
fi
[ "$FAIL" -eq 0 ]
