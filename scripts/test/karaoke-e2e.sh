#!/usr/bin/env bash
#
# 卡拉OK（消人声）端到端脚本
#
# 覆盖的不是一件事，而是"一条链"：
#   CLI `ter-music karaoke` ─┐
#   D-Bus Control.SetKaraoke ─┼→ 核心 karaoke_set_mode()（纯模式状态）
#   Config.Set karaoke_mode  ─┘   ↓ 同步镜像 + 去抖落盘
#                            config.xml <karaoke_mode>N</karaoke_mode>
#                                 ↓ 重启
#                            init_karaoke_from_config() 回灌模式
#
# 另外验证：取值域钳制（越界退回不启用）、cycle 三档循环、非法档位 USAGE(1)、
# 播放中切档不崩、单声道曲目在 soft 档下照常播放；第 10 段用真实模型验证
# "播放中 Off→Soft/Full 必须真正换源"（t7：待切换 + 有界重试，位置连续、
# Soft↔Full 不重跑模型、暂停中切档、Off 回退、模型缺失失败路径）。
# 消人声实现是"ONNX 模型后台分离 + 缓存 + 播放源切换"；config v12 起模型随包
# 内置（固定 uvr-mdxnet-3-9662）、没有功能开关/模型选择，按钮与三档始终可见：
# 本脚本只验控制面（CLI/D-Bus/配置）与不回归；模型定位/校验/分离质量由
# test_karaoke_model.c（真实模型）与 karaoke-model-e2e.sh / karaoke-segments-e2e.sh
# 覆盖。
#
# 用法：scripts/test/karaoke-e2e.sh [--bin <ter-music>] [--model <onnx>] [--keep]
# 环境：需要会话总线（未设置时用 dbus-run-session）；ffmpeg 造素材；
#       第 10 段需要真实 9662 模型（--model / TERMUSIC_KARAOKE_TEST_MODEL /
#       .tmp/models/ 或 .tmp/karaoke-research/model/ 下的 UVR_MDXNET_3_9662.onnx）。

set -uo pipefail

# 断言依赖中文显示名（"消除人声"等）：固定 CLI 语言，避免英文 LANG 下误报
export TER_MUSIC_CLI_LANG=zh_CN
# 容器/打包环境（Linyaps）里 CLI 拒绝 fork 后台核心（cli.c: cli_in_sandbox）。
# 本脚本的用法就是"自己 fork 一个私有总线上的临时核心"，故显式声明非沙箱；
# 宿主机会话里这条没有影响（默认判定本来就是非沙箱）。
export TER_MUSIC_SANDBOX=0
# 第 10 段要从核心 debug 日志取证"playing karaoke cache source=N"（真正换源）。
export TER_MUSIC_FORCE_LOG=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
MODEL_ARG=""
KEEP=0
ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --model) MODEL_ARG="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

# 第 10 段（播放中切档）需要真实模型：按与 karaoke-model-e2e 相同的候选顺序找。
MODEL_SIZE=29704436
find_model() {
    local c
    for c in "$MODEL_ARG" \
             "${TERMUSIC_KARAOKE_TEST_MODEL:-}" \
             "$REPO_ROOT/.tmp/models/UVR_MDXNET_3_9662.onnx" \
             "$REPO_ROOT/.tmp/karaoke-research/model/UVR_MDXNET_3_9662.onnx" \
             "$HOME/Downloads/UVR_MDXNET_3_9662.onnx"; do
        if [ -n "$c" ] && [ -f "$c" ] && \
           [ "$(stat -c %s "$c" 2>/dev/null)" = "$MODEL_SIZE" ]; then
            printf '%s' "$c"
            return 0
        fi
    done
    return 1
}

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
CORE_FG_PID=""
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
    [ -n "${CORE_FG_PID:-}" ] && kill "$CORE_FG_PID" 2>/dev/null
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
# 起核心：常规路径是 CLI 的 detach（daemon start）；受限宿主/容器里 CLI 可能
# 拒绝 fork 脱离进程（沙箱判定/权限），此时回退为前台核心 + 后台作业。
# 被测的配置/搬运/档位语义与进程归属无关，两条路径等价。
start_core() {
    "$TM_BIN" daemon start >/dev/null 2>&1
    for _ in $(seq 1 40); do daemon_up && return 0; sleep 0.25; done
    "$TM_BIN" daemon foreground >"$WORK_DIR/core-foreground.log" 2>&1 &
    CORE_FG_PID=$!
    for _ in $(seq 1 80); do daemon_up && return 0; sleep 0.25; done
    return 1
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
start_core
sleep 1
daemon_up && ok "核心上线" || { bad "核心未上线，后续断言无意义"; exit 1; }

# ── 1. 默认值：不启用 ────────────────────────────────────────────
info "默认档位"
[ "$(level)" = "off (0) 不启用" ] && ok "初始为 off (0)" || bad "初始档位异常：'$(level)'"
g Control GetKaraoke | grep -q "(0,)" && ok "Control.GetKaraoke=0" || bad "GetKaraoke 应答异常：$(g Control GetKaraoke)"

# ── 1b. config v12：删除功能开关/模型选择，默认 Off 也直接接受三档 ────
info "去门控：默认 Off 下 CLI/D-Bus 一律接受 0..2"
"$TM_BIN" karaoke full >/dev/null 2>&1
[ "$?" -eq 0 ] && ok "CLI karaoke full 退出码 0（默认 Off 不再被拒）" || bad "CLI 开档被拒绝"
[ "$(level)" = "full (2) 消除人声" ] && ok "回读 full (2)" || bad "开档后档位异常：'$(level)'"
g Control GetKaraoke | grep -q "(2,)" && ok "D-Bus GetKaraoke 与 CLI 一致（2）" || bad "GetKaraoke 未跟进：$(g Control GetKaraoke)"
g Control SetKaraoke 1 | grep -q "(true,)" && ok "D-Bus SetKaraoke(1) 回 true" || bad "SetKaraoke(1) 异常：$(g Control SetKaraoke 1)"
g Control GetKaraoke | grep -q "(1,)" && ok "D-Bus GetKaraoke=1（操作即时生效）" || bad "GetKaraoke 未跟进：$(g Control GetKaraoke)"
g Control SetKaraoke 9 | grep -q "(false,)" && ok "D-Bus SetKaraoke(9) 越界仍回 false" || bad "越界 SetKaraoke 异常：$(g Control SetKaraoke 9)"
g Control GetKaraoke | grep -q "(1,)" && ok "越界调用不改当前档位（仍 1）" || bad "越界调用改了档位：$(g Control GetKaraoke)"
"$TM_BIN" karaoke cycle >/dev/null 2>&1; [ "$(level)" = "full (2) 消除人声" ] \
    && ok "CLI cycle 与 D-Bus 行为一致（soft→full）" || bad "cycle 异常：'$(level)'"
# 配置面：v12 已删除两个键；CLI 按未知键拒绝，版本为 12。
if grep -Eq '<karaoke_feature[ >/]|<karaoke_model[ >/]' "$CONFIG_XML"; then
    bad "config.xml 含已删除的 karaoke_feature/karaoke_model"
else
    ok "config.xml 不含 karaoke_feature/karaoke_model（config v12）"
fi
"$TM_BIN" config set karaoke_feature 1 >/dev/null 2>&1 \
    && bad "config set karaoke_feature 仍被接受（键应已删除）" \
    || ok "config set karaoke_feature 被拒绝（未知键）"
[ "$("$TM_BIN" config get version 2>/dev/null)" = "12" ] \
    && ok "Config.GetAll version=12" \
    || bad "Config.GetAll version=$("$TM_BIN" config get version 2>/dev/null)（期望 12）"

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
daemon_up && ok "切档后核心仍在（模型缺失时回退原声，不崩溃）" || bad "切档后核心掉线"
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
# config v12：离屏/无模型不再是"静默 no-op"——档位随时可设，模型问题只在分离
# 时以状态行报出（模型缺失/损坏的可读错误与回退原声由 karaoke-model-e2e.sh 覆盖）。
[ "$("$TM_BIN" config get karaoke_mode 2>/dev/null)" = "2" ] \
    && ok "播放中档位已同步到持久配置（karaoke_mode=2）" \
    || bad "播放中档位未同步到配置（$("$TM_BIN" config get karaoke_mode 2>/dev/null)）"

# ── 7. 单声道素材：soft 档下照常播放，不崩 ──────────────────────
info "单声道播放"
"$TM_BIN" play "$WORK_DIR/music/mono.wav" >/dev/null 2>&1
sleep 1.5
"$TM_BIN" karaoke soft >/dev/null 2>&1
sleep 2
daemon_up && ok "单声道 + soft 档：核心正常（模型方案回退原声）" || bad "单声道播放导致核心掉线"

# ── 8. 重启回灌：档位从 config.xml 恢复到引擎 ───────────────────
info "重启后档位回灌"
"$TM_BIN" karaoke full >/dev/null 2>&1
sleep 1.5
stop_core
start_core
sleep 2
if ! daemon_up; then
    start_core
    sleep 2
fi
daemon_up && ok "重启后核心在线" || bad "重启后核心未在线"
[ "$(level)" = "full (2) 消除人声" ] \
    && ok "档位由配置回灌（init_karaoke_from_config）" || bad "重启后档位丢失：'$(level)'"
g Control GetKaraoke | grep -q "(2,)" && ok "重启后 GetKaraoke=2" || bad "重启后 GetKaraoke 异常：$(g Control GetKaraoke)"

# ── 9. 撤销：配置里没有这个键时按默认 0 处理 ────────────────────
info "旧配置兼容（无 karaoke_mode 元素）"
"$TM_BIN" karaoke off >/dev/null 2>&1
"$TM_BIN" config reset --yes >/dev/null 2>&1
sleep 1.5
# 先停核心再改文件：daemon 退出/去抖落盘会重写 config.xml，若先改后停
# 元素会被重新写回（测试假失败）。
stop_core
sleep 1
python3 - "$CONFIG_XML" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
s = re.sub(r"\s*<karaoke_mode>\d+</karaoke_mode>", "", s)
open(p, "w", encoding="utf-8").write(s)
PY
# 注意匹配带尖括号的完整元素（裸 "karaoke_mode" 会与其它键前缀混淆）；
# config v12 已删除 karaoke_feature/karaoke_model，不必再顾虑 model 前缀。
grep -q "<karaoke_mode>" "$CONFIG_XML" && bad "仍残留 karaoke_mode" || ok "已从 config.xml 删除该元素（模拟旧配置）"
start_core
sleep 2
[ "$(level)" = "off (0) 不启用" ] && ok "缺元素时默认不启用（听感与升级前一致）" || bad "缺元素时档位异常：'$(level)'"

# ── 10. 播放中切档：Off→Soft/Full 必须真正换源（真实模型）──────────
# 用户报障（2026-11-26 23:44）：播放中从 Off 切到卡拉OK 档位后不会真正消声。
# 根因：切档瞬间当前段还没分离出来，旧实现只试一次换源就判定"源不可用"，
# worker 产出段后没有任何重试点，于是永远停在原声。修复：登记"待切换"，
# 由 core_tick → karaoke_tick() 限频重试；当前段就绪即复用"重启播放线程并
# 在当前位置恢复"换源（不从 0 重播）。本段用真实 9662 模型验证：
#   · CLI / D-Bus 两条入口都能在有界时间内（≤40s）真正换源且位置连续；
#   · Soft↔Full 立即可切，且不重跑模型（已产出的段文件 inode/size 不变）；
#   · 暂停中 Off→Full 同样生效，恢复后按新档出声、位置不变；
#   · 晚位置启用（≥2 分钟，seek 到 120s）优先当前段、不整曲等待；
#   · CUE 子曲目（cue_offset>0）播放中切档：不登记待切换/不重启播放线程/
#     不用 v2 分段/不谎报“已切换”（t10 R1）；
#   · Non-Off→Off 播放中回到原声；
#   · 模型缺失的失败路径：原声继续、错误可见（三语键）、可再次切档重试、不崩。
info "播放中切档（真实模型）：Off→Full/Soft、Soft↔Full、暂停、Off 回退、失败路径"
SWITCH_MODEL="$(find_model)"
if [ -z "$SWITCH_MODEL" ]; then
    printf '  SKIP 未找到受支持模型（%s B）：新增的播放中切档用例跳过\n' "$MODEL_SIZE"
else
    # 真实模型放旧用户 XDG 只读兼容路径，然后重启核心：模型探测结果按进程
    # 缓存，必须让新进程在模型就绪后重新探测（否则仍报"内置模型缺失"）。
    stop_core
    DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/ter-music"
    mkdir -p "$DATA_DIR/models"
    cp -- "$SWITCH_MODEL" "$DATA_DIR/models/uvr-mdxnet-3-9662.onnx"
    chmod 600 "$DATA_DIR/models/uvr-mdxnet-3-9662.onnx"
    SW="$WORK_DIR/music/switch90.wav"
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "aevalsrc=0.35*sin(880*2*PI*t)|0.35*sin(440*2*PI*t):d=90" \
        -c:a pcm_s16le -y "$SW" 2>/dev/null
    [ -s "$SW" ] && ok "第 10 段：90s 立体声切档素材已就绪" || bad "第 10 段：造 90s 素材失败"
    start_core && ok "第 10 段：核心上线（真实模型就绪）" || bad "第 10 段：核心启动失败"
    # 本段要真播 90s 素材 + 分离：只用应用自身 stream 音量（夜里跑测试时压低），
    # 不碰宿主 sink 音量。
    "$TM_BIN" volume 5 >/dev/null 2>&1 || true
    sleep 0.5

    # 观测面：核心 debug 日志 + Info.GetStatus + show --json
    clog() { ls -t "$XDG_CONFIG_HOME/ter-music"/ter-music-debug-*.log 2>/dev/null | head -1; }
    log_lines() {   # $1 = grep 模式；无匹配/无日志都返回 0
        local n
        n="$(grep -c "$1" "$(clog)" 2>/dev/null)"
        printf '%s' "${n:-0}"
    }
    src_lines()  { log_lines "playing karaoke cache source=$1"; }
    orig_lines() { log_lines "playing original audio (karaoke"; }
    fallback_lines() { log_lines "karaoke segment .* fallback"; }
    wait_src() {    # $1 源编号 $2 基线行数 $3 超时秒
        local want="$1" before="$2" timeout="${3:-40}" i
        for i in $(seq 1 $((timeout * 2))); do
            [ "$(src_lines "$want")" -gt "$before" ] && return 0
            sleep 0.5
        done
        return 1
    }
    wait_log_lines() {  # $1 grep 模式 $2 基线行数 $3 超时秒
        local pat="$1" before="$2" timeout="${3:-15}" i
        for i in $(seq 1 $((timeout * 2))); do
            [ "$(log_lines "$pat")" -gt "$before" ] && return 0
            sleep 0.5
        done
        return 1
    }
    wait_pos() {    # $1 最小毫秒 $2 超时秒
        local want="$1" timeout="${2:-30}" i p
        for i in $(seq 1 $((timeout * 4))); do
            p="$(pos_ms)"; p="${p:-0}"
            [ "$p" -ge "$want" ] && return 0
            sleep 0.25
        done
        return 1
    }
    play_state() {
        "$TM_BIN" show --json 2>/dev/null | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    raise SystemExit(0)
print((doc.get("playback") or {}).get("state") or "")'
    }
    status_msg() {
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
    doc = json.loads(ast.literal_eval(raw)[0])
    print(doc.get("message") or "")
except Exception:
    pass'
    }
    require_status() {   # $1 正则 $2 超时秒 $3 ok 文案 $4 bad 文案
        local pat="$1" timeout="${2:-20}" i msg
        for i in $(seq 1 $((timeout * 2))); do
            msg="$(status_msg)"
            printf '%s' "$msg" | grep -Eq "$pat" && { ok "$3"; return 0; }
            sleep 0.5
        done
        bad "$4（当前状态：$(status_msg)）"
    }
    seg_snapshot() {   # 每行 "path inode size"（按 path 排序，空=无段文件）
        local f
        find /tmp/ter-music-karaoke-* -name 'seg*.flac' -type f 2>/dev/null | sort |
        while read -r f; do stat -c '%n %i %s' "$f" 2>/dev/null; done
    }
    seg_unchanged() {   # $1 = 之前的快照文件；只核对当时已存在的文件
        local path ino size now changed=0
        while read -r path ino size; do
            [ -n "$path" ] || continue
            now="$(stat -c '%i %s' "$path" 2>/dev/null)"
            [ "$now" = "$ino $size" ] || changed=1
        done <"$1"
        [ "$changed" = "0" ]
    }

    # 10a. CLI 入口：Off 档播放 ≥10s 后切 Full → 有界换源、位置连续
    "$TM_BIN" karaoke off >/dev/null 2>&1
    "$TM_BIN" play "$SW" >/dev/null 2>&1
    if wait_pos 10000 30; then
        P0="$(pos_ms)"
        BEFORE_FULL="$(src_lines 2)"
        T0="$(date +%s%3N)"
        "$TM_BIN" karaoke full >/dev/null 2>&1
        if wait_src 2 "$BEFORE_FULL" 40; then
            T1="$(date +%s%3N)"
            WAIT_MS=$((T1 - T0))
            P1="$(pos_ms)"
            ok "10a CLI Off→Full：${WAIT_MS}ms 后自动切到分片伴奏（日志 source=2，位置 ${P0}→${P1} ms）"
            if [ "${P1:-0}" -gt "${P0:-0}" ] && [ "${P1:-0}" -gt 5000 ]; then
                ok "10a 换源未从 0 重播（位置连续：${P0}→${P1} ms）"
            else
                bad "10a 换源位置异常（${P0}→${P1} ms；期望 >${P0} 且 >5000）"
            fi
            require_status '已切换到消除人声|Switched to No vocals' 10 \
                "10a 状态行提示「已切换到消除人声」" \
                "10a 状态行未提示已切换"
            ok "10a 实测切换耗时 ${WAIT_MS}ms（有界，≤40000ms）"
        else
            bad "10a CLI Off→Full 40s 内没有换源（位置 $(pos_ms)ms；段数 $(seg_snapshot | wc -l)）"
        fi
    else
        bad "10a 30s 内未起播（位置 $(pos_ms)ms）"
    fi

    # 10b. Soft↔Full：立即可切，且不重跑模型（已产出段文件 inode/size 不变）
    SEG_SNAP="$WORK_DIR/seg-before.txt"
    seg_snapshot >"$SEG_SNAP"
    BEFORE_SOFT="$(src_lines 1)"
    "$TM_BIN" karaoke soft >/dev/null 2>&1
    if wait_src 1 "$BEFORE_SOFT" 10; then
        ok "10b Full→Soft 播放中互切生效（日志 source=1）"
    else
        bad "10b Full→Soft 10s 内未换源（日志无 source=1）"
    fi
    BEFORE_FULL2="$(src_lines 2)"
    "$TM_BIN" karaoke full >/dev/null 2>&1
    if wait_src 2 "$BEFORE_FULL2" 10; then
        ok "10b Soft→Full 播放中互切生效（日志 source=2）"
    else
        bad "10b Soft→Full 10s 内未换源（日志无 source=2）"
    fi
    sleep 1
    if seg_unchanged "$SEG_SNAP"; then
        ok "10b Soft↔Full 没有重跑模型（已产出段文件 inode/size 不变）"
    else
        bad "10b Soft↔Full 改写了已产出的段文件（发生了第二次推理）"
    fi

    # 10c. 暂停中 Off→Full：同样换源，恢复后按新档出声、位置不变
    "$TM_BIN" karaoke off >/dev/null 2>&1
    sleep 1.0
    "$TM_BIN" pause >/dev/null 2>&1
    sleep 0.8
    [ "$(play_state)" = "paused" ] && ok "10c 已暂停（state=paused）" \
        || bad "10c 暂停失败（state=$(play_state)）"
    P_PAUSE="$(pos_ms)"
    BEFORE_FULL3="$(src_lines 2)"
    "$TM_BIN" karaoke full >/dev/null 2>&1
    if wait_src 2 "$BEFORE_FULL3" 40; then
        ok "10c 暂停中 Off→Full 仍完成换源（日志 source=2）"
        # 换源重建播放线程的瞬间 g_current_position 会短暂归 0，随后按
        # g_initial_seek_position seek 回原位并重新暂停；等它收敛再断言，
        # 避免把"重建窗口"当成位置漂移。
        P_MID=0
        ST_MID=""
        for _ in $(seq 1 24); do
            ST_MID="$(play_state)"
            P_MID="$(pos_ms)"
            [ "$ST_MID" = "paused" ] && [ "${P_MID:-0}" -gt 0 ] && break
            sleep 0.25
        done
        [ "$ST_MID" = "paused" ] && ok "10c 换源后仍处于暂停（state=paused）" \
            || bad "10c 换源后没有回到暂停（state=${ST_MID:-?}）"
        if [ "${P_MID:-0}" -ge $((P_PAUSE - 2000)) ] && \
           [ "${P_MID:-0}" -le $((P_PAUSE + 2000)) ]; then
            ok "10c 暂停期间位置不变（${P_PAUSE}→${P_MID} ms）"
        else
            bad "10c 暂停期间位置漂移（${P_PAUSE}→${P_MID} ms）"
        fi
        FB_BEFORE="$(fallback_lines)"
        "$TM_BIN" resume >/dev/null 2>&1
        sleep 1.5
        P_RESUME="$(pos_ms)"
        if [ "${P_RESUME:-0}" -gt "${P_MID:-0}" ] && [ "${P_RESUME:-0}" -gt 5000 ]; then
            ok "10c 恢复后按新档继续播放（位置 ${P_MID}→${P_RESUME} ms，未从 0 重播）"
        else
            bad "10c 恢复后播放异常（${P_MID}→${P_RESUME} ms）"
        fi
        [ "$(fallback_lines)" = "$FB_BEFORE" ] \
            && ok "10c 恢复后没有掉回原声（无新的段回退日志）" \
            || bad "10c 恢复后出现了新的段回退"
        [ "$(level)" = "full (2) 消除人声" ] && ok "10c 恢复后档位仍为 full" \
            || bad "10c 恢复后档位漂移：'$(level)'"
    else
        bad "10c 暂停中 Off→Full 40s 内没有换源（位置 $(pos_ms)ms）"
    fi

    # 10d. Non-Off→Off：播放中回到原声
    BEFORE_ORIG="$(orig_lines)"
    "$TM_BIN" karaoke off >/dev/null 2>&1
    if wait_log_lines 'playing original audio (karaoke' "$BEFORE_ORIG" 10; then
        ok "10d Full→Off 播放中回到原声（日志出现原声源）"
    else
        bad "10d Full→Off 10s 内没有回到原声"
    fi
    od0="$(pos_ms)"; sleep 1.2; od1="$(pos_ms)"
    [ "${od1:-0}" -gt "${od0:-0}" ] && ok "10d 回到原声后播放继续（${od0}→${od1} ms）" \
        || bad "10d 回到原声后播放异常（${od0}→${od1} ms）"
    [ "$(level)" = "off (0) 不启用" ] && ok "10d 档位为 off" || bad "10d 档位异常：'$(level)'"

    # 10e. 失败路径：模型缺失 → 原声 + 可读错误 + 可再次切档重试 + 不崩
    "$TM_BIN" karaoke off >/dev/null 2>&1
    sleep 0.8
    stop_core
    rm -f "$DATA_DIR/models/uvr-mdxnet-3-9662.onnx"
    start_core && ok "10e 模型缺失场景：核心重启上线" || bad "10e 核心重启失败"
    sleep 0.5
    "$TM_BIN" play "$SW" >/dev/null 2>&1
    wait_pos 3000 20 || true
    BEFORE_SRC="$(src_lines 2)"
    "$TM_BIN" karaoke full >/dev/null 2>&1
    require_status '内置模型不可用|内置模型缺失' 15 \
        "10e 模型缺失：状态行给出可读错误（内置模型不可用/缺失）" \
        "10e 模型缺失：状态行没有可读错误"
    sleep 2
    [ "$(src_lines 2)" = "$BEFORE_SRC" ] \
        && ok "10e 模型缺失时没有假装换源（继续原声）" \
        || bad "10e 模型缺失却切到了缓存源"
    fp0="$(pos_ms)"; sleep 1.2; fp1="$(pos_ms)"
    [ "${fp1:-0}" -gt "${fp0:-0}" ] && ok "10e 模型缺失时原声继续播放（${fp0}→${fp1} ms）" \
        || bad "10e 模型缺失时播放异常（${fp0}→${fp1} ms）"
    "$TM_BIN" karaoke soft >/dev/null 2>&1
    sleep 2
    daemon_up && ok "10e 再次切档重试不崩核心" || bad "10e 再次切档导致核心掉线"
    [ "$(level)" = "soft (1) 人声较弱" ] && ok "10e 再次切档被接受（soft）" \
        || bad "10e 再次切档档位异常：'$(level)'"
    [ "$(src_lines 1)" = "0" ] && ok "10e 再次切档仍不假装换源（无 source=1 日志）" \
        || bad "10e 再次切档切到了缓存源"

    # 10f. D-Bus 入口：Control.SetKaraoke(2) 同样触发有界换源
    stop_core
    cp -- "$SWITCH_MODEL" "$DATA_DIR/models/uvr-mdxnet-3-9662.onnx"
    chmod 600 "$DATA_DIR/models/uvr-mdxnet-3-9662.onnx"
    start_core && ok "10f D-Bus 用例：核心重启上线（模型恢复）" || bad "10f 核心重启失败"
    sleep 0.5
    "$TM_BIN" karaoke off >/dev/null 2>&1
    "$TM_BIN" play "$SW" >/dev/null 2>&1
    if wait_pos 10000 30; then
        DP0="$(pos_ms)"
        BEFORE_FULL4="$(src_lines 2)"
        DT0="$(date +%s%3N)"
        g Control SetKaraoke 2 | grep -q "(true,)" \
            && ok "10f D-Bus SetKaraoke(2) 回 true" \
            || bad "10f D-Bus SetKaraoke(2) 异常：$(g Control SetKaraoke 2)"
        if wait_src 2 "$BEFORE_FULL4" 40; then
            DT1="$(date +%s%3N)"
            DWAIT_MS=$((DT1 - DT0))
            DP1="$(pos_ms)"
            ok "10f D-Bus Off→Full：${DWAIT_MS}ms 后自动切到分片伴奏（位置 ${DP0}→${DP1} ms）"
            [ "${DP1:-0}" -gt "${DP0:-0}" ] && [ "${DP1:-0}" -gt 5000 ] \
                && ok "10f D-Bus 换源位置连续（${DP0}→${DP1} ms，未从 0 重播）" \
                || bad "10f D-Bus 换源位置异常（${DP0}→${DP1} ms）"
            require_status '已切换到消除人声|Switched to No vocals' 10 \
                "10f 状态行提示「已切换到消除人声」" \
                "10f 状态行未提示已切换"
        else
            bad "10f D-Bus Off→Full 40s 内没有换源（位置 $(pos_ms)ms）"
        fi
        g Control GetKaraoke | grep -q "(2,)" && ok "10f D-Bus GetKaraoke=2" \
            || bad "10f D-Bus GetKaraoke 异常：$(g Control GetKaraoke)"
    else
        bad "10f 30s 内未起播（位置 $(pos_ms)ms）"
    fi

    # 10g. 晚位置启用（≥2 分钟）：不得等整曲分离完成才切，必须 prompt 当前段
    LX="$WORK_DIR/music/late180.wav"
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "aevalsrc=0.30*sin(700*2*PI*t)|0.30*sin(350*2*PI*t):d=180" \
        -c:a pcm_s16le -y "$LX" 2>/dev/null
    if [ -s "$LX" ]; then
        "$TM_BIN" stop >/dev/null 2>&1
        "$TM_BIN" karaoke off >/dev/null 2>&1
        sleep 0.8
        "$TM_BIN" play "$LX" >/dev/null 2>&1
        if wait_pos 2000 20; then
            "$TM_BIN" seek 120 >/dev/null 2>&1
            if wait_pos 120000 20; then
                LP0="$(pos_ms)"
                BEFORE_LATE="$(src_lines 2)"
                LT0="$(date +%s%3N)"
                "$TM_BIN" karaoke full >/dev/null 2>&1
                if wait_src 2 "$BEFORE_LATE" 40; then
                    LT1="$(date +%s%3N)"
                    LWAIT_MS=$((LT1 - LT0))
                    LP1="$(pos_ms)"
                    ok "10g 晚位置（120s）Off→Full：${LWAIT_MS}ms 换源（位置 ${LP0}→${LP1} ms，未等整曲分离）"
                    if [ "${LP1:-0}" -gt "${LP0:-0}" ] && [ "${LP1:-0}" -gt 120000 ]; then
                        ok "10g 晚位置换源位置连续（${LP0}→${LP1} ms，未从 0 重播）"
                    else
                        bad "10g 晚位置换源位置异常（${LP0}→${LP1} ms）"
                    fi
                    require_status '已切换到消除人声|Switched to No vocals' 10 \
                        "10g 晚位置状态行提示「已切换到消除人声」" \
                        "10g 晚位置状态行未提示已切换"
                else
                    bad "10g 晚位置 Off→Full 40s 内没有换源（位置 $(pos_ms)ms）"
                fi
            else
                bad "10g seek 到 120s 后 20s 内位置未到位（$(pos_ms)ms）"
            fi
        else
            bad "10g 晚位置用例 20s 内未起播（位置 $(pos_ms)ms）"
        fi
    else
        bad "10g 造 180s 晚位置素材失败"
    fi
    # 10h. CUE 子曲目播放中切档：不登记待切换、不重启、不谎报（t10 R1）
    CUE_DIR="$WORK_DIR/music/cue"
    mkdir -p "$CUE_DIR"
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "aevalsrc=0.30*sin(660*2*PI*t)|0.30*sin(330*2*PI*t):d=90" \
        -c:a pcm_s16le -y "$CUE_DIR/album.wav" 2>/dev/null
    if [ -s "$CUE_DIR/album.wav" ]; then
        {
            printf 'FILE "album.wav" WAVE\n'
            printf '  TRACK 01 AUDIO\n    TITLE "A"\n    INDEX 01 00:05:00\n'
            printf '  TRACK 02 AUDIO\n    TITLE "B"\n    INDEX 01 00:50:00\n'
        } >"$CUE_DIR/album.cue"
        "$TM_BIN" stop >/dev/null 2>&1
        "$TM_BIN" karaoke off >/dev/null 2>&1
        sleep 0.8
        "$TM_BIN" play "$CUE_DIR/album.cue" >/dev/null 2>&1
        if wait_pos 1000 20; then
            "$TM_BIN" next >/dev/null 2>&1      # 第二轨：cue_offset>0
            if wait_pos 2000 20; then
                ok "10h CUE 第二轨播放中（cue_offset>0）"
            else
                bad "10h CUE 第二轨 20s 内未起播（位置 $(pos_ms)ms）"
            fi
        else
            bad "10h CUE 第一轨 20s 内未起播（位置 $(pos_ms)ms）"
        fi
        sleep 0.8
        CP0="$(pos_ms)"
        RS_BASE="$(log_lines 'Karaoke source switch:')"
        RA_BASE="$(log_lines 'karaoke source re-acquire')"
        SRC_BASE="$(log_lines 'playing karaoke cache source=')"
        SKIP_BASE="$(log_lines 'karaoke segments skipped for CUE sub-track')"
        "$TM_BIN" karaoke full >/dev/null 2>&1
        sleep 6
        CP1="$(pos_ms)"
        if [ "$(log_lines 'karaoke segments skipped for CUE sub-track')" -gt "$SKIP_BASE" ]; then
            ok "10h CUE 子曲目明确跳过 v2 分段源（日志新增 skipped 行）"
        else
            bad "10h CUE 子曲目切档没有跳过 v2 分段源（日志无新增 skipped 行）"
        fi
        if [ "$(log_lines 'Karaoke source switch:')" = "$RS_BASE" ]; then
            ok "10h CUE 切档：无播放线程重启（与 pre-t7 基线一致：0 次）"
        else
            bad "10h CUE 切档出现了播放线程重启（$(log_lines 'Karaoke source switch:') vs base $RS_BASE）"
        fi
        if [ "$(log_lines 'karaoke source re-acquire')" = "$RA_BASE" ]; then
            ok "10h CUE 切档：无 re-acquire 重排"
        else
            bad "10h CUE 切档出现了 re-acquire（$(log_lines 'karaoke source re-acquire') vs base $RA_BASE）"
        fi
        if [ "$(log_lines 'playing karaoke cache source=')" = "$SRC_BASE" ]; then
            ok "10h CUE 切档：仍保持原声（无 playing karaoke cache source 行）"
        else
            bad "10h CUE 切档出现了 cache source 行（$(log_lines 'playing karaoke cache source=') vs base $SRC_BASE）"
        fi
        MSG="$(status_msg)"
        if printf '%s' "$MSG" | grep -Eq '已切换到|Switched to'; then
            bad "10h CUE 切档后状态行谎报已切换：$MSG"
        else
            ok "10h CUE 切档后状态行没有谎报已切换（当前：${MSG:-空}）"
        fi
        if [ "${CP1:-0}" -gt "${CP0:-0}" ] && [ "${CP1:-0}" -gt 2000 ]; then
            ok "10h CUE 切档后位置连续（${CP0}→${CP1} ms，仍原声播放）"
        else
            bad "10h CUE 切档后位置异常（${CP0}→${CP1} ms）"
        fi
        "$TM_BIN" karaoke off >/dev/null 2>&1
    else
        bad "10h CUE 素材生成失败"
    fi

    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
fi


# ── 结果 ─────────────────────────────────────────────────────────
info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
