#!/usr/bin/env bash
#
# 卡拉OK（消人声）端到端脚本
#
# 覆盖的不是一件事，而是"一条链"：
#   CLI `ter-music karaoke` ─┐
#   D-Bus Control.SetKaraoke ─┼→ 核心 karaoke_set_mode()（引擎即时生效）
#   Config.Set karaoke_mode  ─┘   ↓ 同步镜像 + 去抖落盘
#                            config.xml <karaoke_mode>N</karaoke_mode>
#                                 ↓ 重启
#                            init_karaoke_from_config() 回灌引擎
#
# 另外验证：取值域钳制（越界退回不启用）、cycle 三档循环、非法档位 USAGE(1)、
# 播放中切档不崩、单声道曲目在 full 档下照常播放（DSP 侧为直通）。
# DSP 本身的衰减量由 scripts/test/test_karaoke.c 单测负责，不在这里重复。
#
# 用法：scripts/test/karaoke-e2e.sh [--bin <ter-music>] [--keep]
# 环境：需要会话总线（未设置时用 dbus-run-session）；ffmpeg 可选（造立体声素材）。

set -uo pipefail

# 断言依赖中文显示名（"消除人声"等）：固定 CLI 语言，避免英文 LANG 下误报
export TER_MUSIC_CLI_LANG=zh_CN
# 容器/打包环境（Linyaps）里 CLI 拒绝 fork 后台核心（cli.c: cli_in_sandbox）。
# 本脚本的用法就是"自己 fork 一个私有总线上的临时核心"，故显式声明非沙箱；
# 宿主机会话里这条没有影响（默认判定本来就是非沙箱）。
export TER_MUSIC_SANDBOX=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

# 私有会话总线（与 cli-subcommands-smoke.sh 同一套约定）
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ "${TM_TEST_REUSE_SESSION_BUS:-0}" != "1" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：需要 dbus-run-session，或设 TM_TEST_REUSE_SESSION_BUS=1。" >&2
        exit 2
    fi
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

if [ ! -x "$TM_BIN" ]; then
    TM_BIN_ABS="$(realpath -- "$TM_BIN" 2>/dev/null)"
    [ -n "$TM_BIN_ABS" ] && TM_BIN="$TM_BIN_ABS"
fi
[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN" >&2; exit 2; }

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-karaoke-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$XDG_CONFIG_HOME/ter-music" "$WORK_DIR/music"
CONFIG_XML="$XDG_CONFIG_HOME/ter-music/config.xml"

cat > "$CONFIG_XML" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <paths>
    <default_startup_path>$WORK_DIR/music</default_startup_path>
  </paths>
</ter-music-config>
EOF

cleanup() {
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

daemon_up() { "$TM_BIN" daemon status 2>/dev/null | grep -q "pid"; }
stop_core() {
    "$TM_BIN" daemon stop >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do daemon_up || break; sleep 0.1; done
}

# 核心 D-Bus 调用（总线名与 MPRIS 同名，见 media/session.c 头注）
g() {
    timeout 10 gdbus call --session \
        --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 \
        --method "cn.org.huansoft.ter_music.$1.$2" "${@:3}" 2>&1
}

# 当前档位（走 CLI，输出形如 "full (2) 消除人声"）
level() { "$TM_BIN" karaoke 2>/dev/null; }

# ── 素材：一首立体声（左右不同频）+ 一首单声道 ───────────────────
HAVE_FFMPEG=0
if command -v ffmpeg >/dev/null 2>&1; then
    HAVE_FFMPEG=1
    # 左右声道各一频：中置消除后仍应有声（两侧差分保留）
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "aevalsrc=0.4*sin(880*2*PI*t)|0.4*sin(440*2*PI*t):d=6" \
        -c:a pcm_s16le -y "$WORK_DIR/music/stereo.wav" 2>/dev/null
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "sine=frequency=660:duration=6" \
        -ac 1 -c:a pcm_s16le -y "$WORK_DIR/music/mono.wav" 2>/dev/null
fi
if [ "$HAVE_FFMPEG" -ne 1 ]; then
    echo "错误：本脚本需要 ffmpeg 造立体声/单声道素材。" >&2
    exit 2
fi

# ── 起核心 ───────────────────────────────────────────────────────
info "daemon start"
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1
daemon_up && ok "核心上线" || { bad "核心未上线，后续断言无意义"; exit 1; }

# ── 1. 默认值：不启用 ────────────────────────────────────────────
info "默认档位"
[ "$(level)" = "off (0) 不启用" ] && ok "初始为 off (0)" || bad "初始档位异常：'$(level)'"
g Control GetKaraoke | grep -q "(0,)" && ok "Control.GetKaraoke=0" || bad "GetKaraoke 应答异常：$(g Control GetKaraoke)"

# ── 2. CLI 设档 → D-Bus 与落盘同步 ───────────────────────────────
info "CLI 设档 full"
"$TM_BIN" karaoke full >/dev/null 2>&1 && ok "karaoke full 退出码 0" || bad "karaoke full 失败"
sleep 1.5   # 去抖落盘（CORE_CONFIG_SAVE_DEBOUNCE_MS=500）
[ "$(level)" = "full (2) 消除人声" ] && ok "回读 full (2)" || bad "回读异常：'$(level)'"
g Control GetKaraoke | grep -q "(2,)" && ok "Control.GetKaraoke=2" || bad "GetKaraoke 未跟进：$(g Control GetKaraoke)"
grep -q "<karaoke_mode>2</karaoke_mode>" "$CONFIG_XML" && ok "config.xml 落盘 karaoke_mode=2" || bad "config.xml 未见 karaoke_mode=2"

# ── 3. cycle 三档循环 ───────────────────────────────────────────
info "cycle 循环"
"$TM_BIN" karaoke cycle >/dev/null 2>&1; [ "$(level)" = "off (0) 不启用" ] \
    && ok "full→off 回绕" || bad "full→off 失败：'$(level)'"
"$TM_BIN" karaoke cycle >/dev/null 2>&1; [ "$(level)" = "soft (1) 人声较弱" ] \
    && ok "off→soft" || bad "off→soft 失败：'$(level)'"
"$TM_BIN" karaoke cycle >/dev/null 2>&1; [ "$(level)" = "full (2) 消除人声" ] \
    && ok "soft→full" || bad "soft→full 失败：'$(level)'"

# ── 4. 非法档位 → USAGE(1) ──────────────────────────────────────
info "非法输入"
"$TM_BIN" karaoke bogus_level >/dev/null 2>&1
[ "$?" -eq 1 ] && ok "非法档位 → USAGE(1)" || bad "非法档位未返回 1"
[ "$(level)" = "full (2) 消除人声" ] && ok "非法输入不改动原档位" || bad "非法输入后档位漂移：'$(level)'"

# ── 5. Config.Set 走运行时应用（与 karaoke 子命令同一入口） ──────
info "Config.Set 路径"
"$TM_BIN" config set karaoke_mode 1 >/dev/null 2>&1
[ "$(level)" = "soft (1) 人声较弱" ] && ok "Config.Set 即时生效（不必重启）" || bad "Config.Set 后档位：'$(level)'"
# 越界：k_fields 的 max=2 钳制（config_json.c 的 clamp），不应报错也不应存 99
"$TM_BIN" config set karaoke_mode 99 >/dev/null 2>&1
[ "$(level)" = "full (2) 消除人声" ] && ok "越界值被钳制到 2" || bad "越界钳制异常：'$(level)'"
"$TM_BIN" config set karaoke_mode 0 >/dev/null 2>&1
[ "$(level)" = "off (0) 不启用" ] && ok "Config.Set 归零生效" || bad "归零后档位：'$(level)'"

# ── 6. 播放中切档：核心不崩、档位可查 ───────────────────────────
info "播放中切档"
"$TM_BIN" play "$WORK_DIR/music/stereo.wav" >/dev/null 2>&1
sleep 1.2
"$TM_BIN" karaoke full >/dev/null 2>&1
sleep 1.5
daemon_up && ok "切档后核心仍在（DSP 在播放线程里热切换）" || bad "切档后核心掉线"
# 位置取 JSON 的 playback.position_ms（info.c 同时发 "position" 文本，两者同源）
pos_ms() {
    "$TM_BIN" show --json 2>/dev/null | python3 -c '
import json, sys
raw = sys.stdin.read().strip()
if not raw:
    raise SystemExit(0)
try:
    doc = json.loads(raw)
except ValueError:
    raise SystemExit(0)
pb = doc.get("playback") or {}
sys.stdout.write(str(pb.get("position_ms", "")))
'
}
pos_a="$(pos_ms)"
sleep 2
pos_b="$(pos_ms)"
case "$pos_a:$pos_b" in
    :*) bad "读不到播放位置（show --json 结构变化？）" ;;
    *)  [ "${pos_b:-0}" -ge "${pos_a:-0}" ] && ok "full 档下播放位置推进（$pos_a→$pos_b ms）" \
            || bad "播放位置倒退：$pos_a→$pos_b" ;;
esac
[ "$(level)" = "full (2) 消除人声" ] && ok "播放中档位保持" || bad "播放中档位漂移：'$(level)'"

# ── 7. 单声道素材：full 档直通，不崩 ────────────────────────────
info "单声道直通"
"$TM_BIN" play "$WORK_DIR/music/mono.wav" >/dev/null 2>&1
sleep 1.5
"$TM_BIN" karaoke soft >/dev/null 2>&1
sleep 2
daemon_up && ok "单声道 + soft 档：核心正常（引擎侧直通）" || bad "单声道播放导致核心掉线"

# ── 8. 重启回灌：档位从 config.xml 恢复到引擎 ───────────────────
info "重启后档位回灌"
"$TM_BIN" karaoke full >/dev/null 2>&1
sleep 1.5
"$TM_BIN" daemon restart >/dev/null 2>&1
sleep 2
if ! daemon_up; then
    "$TM_BIN" daemon start >/dev/null 2>&1
    sleep 2
fi
daemon_up && ok "重启后核心在线" || bad "重启后核心未在线"
[ "$(level)" = "full (2) 消除人声" ] \
    && ok "档位由配置回灌到引擎（init_karaoke_from_config）" || bad "重启后档位丢失：'$(level)'"
g Control GetKaraoke | grep -q "(2,)" && ok "重启后 GetKaraoke=2" || bad "重启后 GetKaraoke 异常：$(g Control GetKaraoke)"

# ── 9. 撤销：配置里没有这个键时按默认 0 处理 ────────────────────
info "旧配置兼容（无 karaoke_mode 元素）"
"$TM_BIN" karaoke off >/dev/null 2>&1
"$TM_BIN" config reset --yes >/dev/null 2>&1
sleep 1.5
python3 - "$CONFIG_XML" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
s = re.sub(r"\s*<karaoke_mode>\d+</karaoke_mode>", "", s)
open(p, "w", encoding="utf-8").write(s)
PY
grep -q "karaoke_mode" "$CONFIG_XML" && bad "仍残留 karaoke_mode" || ok "已从 config.xml 删除该元素（模拟旧配置）"
stop_core
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 2
[ "$(level)" = "off (0) 不启用" ] && ok "缺元素时默认不启用（听感与升级前一致）" || bad "缺元素时档位异常：'$(level)'"

# ── 结果 ─────────────────────────────────────────────────────────
info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
