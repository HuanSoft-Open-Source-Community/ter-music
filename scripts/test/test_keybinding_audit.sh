#!/usr/bin/env bash
#
# Task 7「严格审计 CLI」测试：ter-music keys list|check [--json]
#
# 覆盖：
#   - 含合法覆盖 / 非法键名 / 冲突项 / 未知 action 的配置：check 非 0 且
#     JSON 的 invalid / conflicts 计数正确
#   - 全合法配置：check 返回 0
#   - keys list --json 是合法 JSON（python3 -m json.tool 校验）
#   - 无配置文件：按默认键位 check 返回 0
#   - 未知子命令/选项返回 CLI_EXIT_USAGE(1)
#
# 用法：scripts/test/test_keybinding_audit.sh [--bin <ter-music>] [--keep]
# 环境：TM_BIN 可指定构建产物；缺省用 build/ter-music，再回退 PATH。
#       本测试不要求 D-Bus / 音频后端 / 终端，只读临时 XDG_CONFIG_HOME。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-}"
KEEP=0

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$TM_BIN" ]; then
    if [ -x "$REPO_ROOT/build/ter-music" ]; then
        TM_BIN="$REPO_ROOT/build/ter-music"
    else
        TM_BIN="$(command -v ter-music 2>/dev/null || true)"
    fi
fi
[ -n "$TM_BIN" ] && [ -x "$TM_BIN" ] || {
    echo "错误：找不到可执行文件（用 --bin 或 TM_BIN 指定）" >&2
    exit 2
}
if command -v realpath >/dev/null 2>&1; then
    TM_BIN="$(realpath -- "$TM_BIN")"
fi

PASS=0
FAIL=0
SKIP=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
skip() { SKIP=$((SKIP + 1)); printf '  SKIP %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-keys-audit-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
export XDG_DATA_HOME="$WORK_DIR/home/.local/share"
mkdir -p "$XDG_CONFIG_HOME/ter-music" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

CONFIG_FILE="$XDG_CONFIG_HOME/ter-music/config.xml"
OUT_JSON="$WORK_DIR/out.json"
ERR_LOG="$WORK_DIR/err.log"

cleanup() {
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

HAVE_PYTHON=0
if command -v python3 >/dev/null 2>&1; then
    HAVE_PYTHON=1
fi

jget() {
    python3 - "$1" "$2" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    data = json.load(fh)
print(data[sys.argv[2]])
PY
}

json_is_valid() {
    python3 -m json.tool "$1" >/dev/null 2>&1
}

action_keys() {   # $1 = action id；输出 “键1/键2”
    "$TM_BIN" keys list --json 2>/dev/null | python3 -c '
import json, sys
doc = json.load(sys.stdin)
for item in doc.get("actions", []):
    if item.get("id") == sys.argv[1]:
        print("/".join(item.get("keys", [])))
        break
else:
    print("?")' "$1" 2>/dev/null || echo "?"
}

write_config_invalid() {
    cat > "$CONFIG_FILE" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <keybindings>
    <binding action="track.next" key="]"/>
    <binding action="list.up" key="bogus"/>
    <binding action="no.such.action" key="x"/>
    <binding action="list.down" key="k"/>
    <binding action="app.quit" key="ctrl+q"/>
  </keybindings>
</ter-music-config>
EOF
}

write_config_valid() {
    cat > "$CONFIG_FILE" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <keybindings>
    <binding action="track.next" key="]"/>
    <binding action="settings.save" key="w"/>
  </keybindings>
</ter-music-config>
EOF
}

# ── 含非法项的配置：check 必须失败且计数正确 ─────────────────────
info "非法配置：keys check --json"
write_config_invalid
"$TM_BIN" keys check --json >"$OUT_JSON" 2>"$ERR_LOG"
rc=$?
[ "$rc" -ne 0 ] && ok "含非法项时退出码非 0（rc=$rc）" || bad "含非法项时退出码为 0"

if [ "$HAVE_PYTHON" -eq 1 ]; then
    if json_is_valid "$OUT_JSON"; then
        ok "keys check --json 输出是合法 JSON"
    else
        bad "keys check --json 不是合法 JSON：$(cat "$ERR_LOG")"
    fi

    [ "$(jget "$OUT_JSON" ok)" = "False" ] && ok "JSON ok=false" || bad "JSON ok 不是 false"
    [ "$(jget "$OUT_JSON" invalid)" = "2" ] && ok "JSON invalid=2（非法键 + 未知 action）" || bad "JSON invalid=$(jget "$OUT_JSON" invalid)（期望 2）"
    [ "$(jget "$OUT_JSON" conflicts)" = "1" ] && ok "JSON conflicts=1" || bad "JSON conflicts=$(jget "$OUT_JSON" conflicts)（期望 1）"
    [ "$(jget "$OUT_JSON" missing_defaults)" = "0" ] && ok "JSON missing_defaults=0" || bad "JSON missing_defaults 非 0"
    [ "$(jget "$OUT_JSON" locked_overrides)" = "1" ] && ok "JSON locked_overrides=1（app.quit 已被锁定）" \
        || bad "JSON locked_overrides=$(jget "$OUT_JSON" locked_overrides)（期望 1）"

    # 运行时加载的逐项降级：合法覆盖保留，非法键/未知 action/冲突项跳过
    [ "$(action_keys track.next)" = "]" ] && ok "运行时保留合法覆盖 track.next=]" \
        || bad "运行时丢失合法覆盖 track.next（实测 $(action_keys track.next)）"
    [ "$(action_keys app.quit)" = "q/Q/f9" ] && ok "锁定动作 app.quit 运行时保持默认 q/Q/f9" \
        || bad "锁定动作 app.quit 未回退默认（实测 $(action_keys app.quit)）"
    [ "$(action_keys list.up)" = "up/k" ] && ok "非法键 list.up=bogus 被跳过并保留默认" \
        || bad "非法键 list.up 未正确降级（实测 $(action_keys list.up)）"
    [ "$(action_keys list.down)" = "down/j" ] && ok "冲突项 list.down=k 被跳过并保留默认" \
        || bad "冲突项 list.down 未正确降级（实测 $(action_keys list.down)）"
else
    skip "JSON 断言（未找到 python3）"
fi

# ── 全合法配置：check 返回 0；list --json 合法 ───────────────────
info "合法配置：keys check / keys list --json"
write_config_valid

"$TM_BIN" keys check >/dev/null 2>"$ERR_LOG"
rc=$?
[ "$rc" -eq 0 ] && ok "全合法配置 keys check 返回 0" || bad "全合法配置 keys check 返回 $rc（期望 0）"

"$TM_BIN" keys check --json >"$OUT_JSON" 2>"$ERR_LOG"
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "全合法配置 keys check --json 返回 0"
else
    bad "全合法配置 keys check --json 返回 $rc（期望 0）"
fi

if [ "$HAVE_PYTHON" -eq 1 ]; then
    if json_is_valid "$OUT_JSON"; then
        ok "合法配置的 check --json 是合法 JSON"
    else
        bad "合法配置的 check --json 不是合法 JSON"
    fi
    [ "$(jget "$OUT_JSON" ok)" = "True" ] && ok "JSON ok=true" || bad "JSON ok 不是 true"
    [ "$(jget "$OUT_JSON" invalid)" = "0" ] && ok "JSON invalid=0" || bad "JSON invalid 非 0"
    [ "$(jget "$OUT_JSON" conflicts)" = "0" ] && ok "JSON conflicts=0" || bad "JSON conflicts 非 0"
    [ "$(jget "$OUT_JSON" overrides)" = "2" ] && ok "JSON overrides=2（文件中的覆盖条目）" || bad "JSON overrides=$(jget "$OUT_JSON" overrides)（期望 2）"
    [ "$(jget "$OUT_JSON" actions)" = "128" ] && ok "JSON actions=128" || bad "JSON actions=$(jget "$OUT_JSON" actions)（期望 128）"
fi

"$TM_BIN" keys list --json >"$OUT_JSON" 2>"$ERR_LOG"
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "keys list --json 返回 0"
else
    bad "keys list --json 返回 $rc"
fi

if [ "$HAVE_PYTHON" -eq 1 ]; then
    if json_is_valid "$OUT_JSON"; then
        ok "keys list --json 输出可被 python3 -m json.tool 解析"
    else
        bad "keys list --json 不是合法 JSON"
    fi
    [ "$(jget "$OUT_JSON" actions | head -c 1)" != "" ] && ok "keys list --json 含 actions 数组" || bad "keys list --json 缺少 actions"
    [ "$(jget "$OUT_JSON" shadows | head -c 1)" != "" ] && ok "keys list --json 含 shadows 数组" || bad "keys list --json 缺少 shadows"
    locked_bindable="$(python3 - "$OUT_JSON" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
for item in doc.get("actions", []):
    if item.get("id") == "app.quit":
        print(str(item.get("bindable")).lower())
        break
else:
    print("?")
PY
)"
    [ "$locked_bindable" = "false" ] && ok "keys list 将 app.quit 标记为 bindable=false" \
        || bad "keys list 的 app.quit bindable=$locked_bindable（期望 false）"
fi

# 人读输出应标出覆盖项
"$TM_BIN" keys list >"$WORK_DIR/list.txt" 2>"$ERR_LOG"
if grep -q '\[\*覆盖\]' "$WORK_DIR/list.txt" && grep -q 'track.next' "$WORK_DIR/list.txt"; then
    ok "keys list 人读输出包含覆盖标记与动作 ID"
else
    bad "keys list 人读输出缺少覆盖标记/动作 ID"
fi

# ── 无配置文件：默认键位应通过 ───────────────────────────────────
info "无配置文件：默认键位 keys check"
rm -f "$CONFIG_FILE"
"$TM_BIN" keys check >/dev/null 2>"$ERR_LOG"
rc=$?
[ "$rc" -eq 0 ] && ok "无配置文件时 keys check 返回 0（默认键位完整）" || bad "无配置文件时 keys check 返回 $rc（期望 0）"

"$TM_BIN" keys check --json >"$OUT_JSON" 2>"$ERR_LOG"
rc=$?
if [ "$rc" -eq 0 ] && [ "$HAVE_PYTHON" -eq 1 ]; then
    [ "$(jget "$OUT_JSON" ok)" = "True" ] && ok "无配置 JSON ok=true" || bad "无配置 JSON ok 不是 true"
    [ "$(jget "$OUT_JSON" overrides)" = "0" ] && ok "无配置 JSON overrides=0" || bad "无配置 overrides 非 0"
elif [ "$rc" -eq 0 ]; then
    ok "无配置文件时 keys check --json 返回 0"
else
    bad "无配置文件时 keys check --json 返回 $rc"
fi

# ── 最大覆盖量：每条可绑定动作一条覆盖，验证无截断/无冲突 ─────────
if [ "$HAVE_PYTHON" -eq 1 ]; then
    info "最大覆盖量：118 条可绑定动作覆盖的 XML 读写与审计"
    "$TM_BIN" keys list --json >"$WORK_DIR/defaults.json" 2>"$ERR_LOG"
    python3 - "$WORK_DIR/defaults.json" "$CONFIG_FILE" <<'PY'
import json, sys, collections
actions = [a for a in json.load(open(sys.argv[1], encoding="utf-8"))["actions"]
           if a.get("bindable")]
pool = [chr(c) for c in range(0x21, 0x7f) if chr(c) not in '&<>"\'\\ ']
used = collections.defaultdict(int)
lines = ['<?xml version="1.0" encoding="UTF-8"?>',
         '<ter-music-config version="9">', '  <keybindings>']
for item in actions:
    ctx = item["context"]
    key = pool[used[ctx]]
    used[ctx] += 1
    lines.append('    <binding action="%s" key="%s"/>' % (item["id"], key))
lines += ['  </keybindings>', '</ter-music-config>']
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(lines) + "\n")
PY
    "$TM_BIN" keys check --json >"$OUT_JSON" 2>"$ERR_LOG"
    rc=$?
    [ "$rc" -eq 0 ] && ok "118 条覆盖的 keys check 返回 0（无截断/冲突）" \
        || bad "118 条覆盖的 keys check 返回 $rc（期望 0）：$(head -c 300 "$ERR_LOG")"
    if json_is_valid "$OUT_JSON"; then
        ok "118 条覆盖的 check --json 是合法 JSON"
    else
        bad "118 条覆盖的 check --json 不是合法 JSON"
    fi
    [ "$(jget "$OUT_JSON" overrides)" = "118" ] && ok "JSON overrides=118（全部可绑定动作）" \
        || bad "JSON overrides=$(jget "$OUT_JSON" overrides)（期望 118）"
    [ "$(jget "$OUT_JSON" conflicts)" = "0" ] && ok "118 条覆盖 conflicts=0" \
        || bad "118 条覆盖 conflicts=$(jget "$OUT_JSON" conflicts)"
    "$TM_BIN" keys list --json >"$OUT_JSON" 2>"$ERR_LOG"
    if json_is_valid "$OUT_JSON"; then
        ok "118 条覆盖时 keys list --json 仍合法"
    else
        bad "118 条覆盖时 keys list --json 不合法（可能被截断）"
    fi
else
    skip "最大覆盖量测试（未找到 python3）"
fi

# ── 用法错误 ─────────────────────────────────────────────────────
info "用法错误"
"$TM_BIN" keys >/dev/null 2>&1
[ "$?" -eq 1 ] && ok "keys 无子命令 → 1(USAGE)" || bad "keys 无子命令未返回 1"

"$TM_BIN" keys list --bogus >/dev/null 2>&1
[ "$?" -eq 1 ] && ok "keys list 未知选项 → 1(USAGE)" || bad "keys list 未知选项未返回 1"

"$TM_BIN" keys nope >/dev/null 2>&1
[ "$?" -eq 1 ] && ok "keys 未知子命令 → 1(USAGE)" || bad "keys 未知子命令未返回 1"

info "结果"
printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
