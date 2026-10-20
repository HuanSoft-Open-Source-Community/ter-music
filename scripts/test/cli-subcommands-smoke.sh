#!/usr/bin/env bash
#
# Task 8「CLI 功能补完」冒烟脚本（独立、只读复用既有公共约定，不改一行式 helper）
#
# 覆盖：
#   - play 分块下发（>500 大曲库）、单文件精确定位、无参 play、--foreground
#   - queue list/clear/shuffle/remove/append/insert
#   - config get/set/reload/reset(--yes)
#   - lyrics / track / cover 只读输出
#   - daemon status / restart --open
#   - 错误路径：无实例退出码 3；--bus 指向未知名同样 3
#
# 用法：scripts/test/cli-subcommands-smoke.sh [--bin <ter-music>] [--keep]
# 环境：需要会话总线（未设置时用 dbus-run-session）；需要 ffmpeg 造素材。
#       >500 场景需要 ffmpeg（或退化用复制）造 600 个占位 wav。

set -uo pipefail

# 断言依赖中文输出（"队列共"/"实例："）：固定 CLI 语言，避免英文 LANG 的开发者环境误报。
export TER_MUSIC_CLI_LANG=zh_CN

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0

# 原始参数：dbus-run-session 再入时必须带上（否则 --bin 丢失）
ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

PASS=0
FAIL=0
SKIP=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
skip() { SKIP=$((SKIP + 1)); printf '  SKIP %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

# 私有会话总线（除非显式复用）
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ "${TM_TEST_REUSE_SESSION_BUS:-0}" != "1" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：需要 dbus-run-session，或设 TM_TEST_REUSE_SESSION_BUS=1。" >&2
        exit 2
    fi
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

# 子 shell 里 cd 之后相对路径的 --bin 会失效（如 `( cd "$dir" && "$TM_BIN" play ... )`），
# 所以在任何用例使用 $TM_BIN 之前先绝对化。realpath 不可用或失败（文件不存在）时保持原值，
# 下面 "[ -x ] → 报错并 exit 2" 的既有语义不变。
if command -v realpath >/dev/null 2>&1; then
    TM_BIN_ABS="$(realpath -- "$TM_BIN" 2>/dev/null)"
    [ -n "$TM_BIN_ABS" ] && TM_BIN="$TM_BIN_ABS"
fi

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN" >&2; exit 2; }

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-cli-smoke-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$XDG_CONFIG_HOME" "$WORK_DIR/music" "$WORK_DIR/big"

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

# ── 造素材：3 个可分辨的小 wav + 一个带 CJK 名的文件 ──────────────
HAVE_FFMPEG=0
if command -v ffmpeg >/dev/null 2>&1; then
    HAVE_FFMPEG=1
    for i in 1 2 3; do
        ffmpeg -hide_banner -loglevel error -f lavfi \
            -i "sine=frequency=$((300 + i * 110)):duration=2" \
            -c:a pcm_s16le -y "$WORK_DIR/music/tone$i.wav" 2>/dev/null
    done
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "sine=frequency=880:duration=2" \
        -metadata title="第二首" -metadata artist="测试艺术家" \
        -c:a pcm_s16le -y "$WORK_DIR/music/tone2.wav" 2>/dev/null
fi
if [ "$HAVE_FFMPEG" -ne 1 ]; then
    # 退化：用同一份最小 wav 复制（无法验证标签，仅验证流程不崩）
    skip "未找到 ffmpeg：标签/单文件精确定位相关断言将降级"
fi

# ── 起核心（私有总线，默认主名） ──────────────────────────────────
info "daemon start（空核心，等前端下发）"
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1
if daemon_up; then
    ok "核心以 daemon 形态在私有总线上线"
else
    bad "核心未能上线，后续断言无意义"
fi

# ── play <目录>：分块下发 + 队列到位 ──────────────────────────────
info "play <小目录>"
if [ "$HAVE_FFMPEG" -eq 1 ]; then
    "$TM_BIN" play "$WORK_DIR/music" >/dev/null 2>&1
    rc=$?
    total="$("$TM_BIN" queue list --count 0 2>/dev/null | sed -n 's/^队列共 \([0-9]\{1,\}\) 首.*/\1/p')"
    if [ "$rc" -eq 0 ] && [ "$total" = "3" ]; then
        ok "play 目录成功，queue list 报告 total=3"
    else
        bad "play 目录后 total=$total（期望 3），rc=$rc"
    fi
else
    skip "play 目录（无素材）"
fi

# ── play <单文件>：精确定位（current 落在该文件，而非首曲） ────────
info "play <单文件>：精确命中起始下标"
if [ "$HAVE_FFMPEG" -eq 1 ]; then
    "$TM_BIN" play "$WORK_DIR/music/tone3.wav" >/dev/null 2>&1
    # tone3 在按名排序后是第 3 首（index 2）；> 标记应落在含 tone3 的行
    if "$TM_BIN" queue list 2>/dev/null | grep -E '^>' | grep -q 'tone3'; then
        ok "单文件 play 后当前行 '>' 命中 tone3（不是回落第一首）"
    else
        bad "单文件 play 当前行未命中 tone3"
    fi
    # 相对/带 ~ 也应能定位（用绝对路径已覆盖核心；这里补一个 cwd 相对路径）
    ( cd "$WORK_DIR/music" && "$TM_BIN" play ./tone1.wav >/dev/null 2>&1 )
    if "$TM_BIN" queue list 2>/dev/null | grep -E '^>' | grep -q 'tone1'; then
        ok "相对路径 ./tone1.wav 精确定位成功"
    else
        bad "相对路径未精确定位"
    fi
else
    skip "单文件精确定位（无素材）"
fi

# ── queue append / insert / remove / shuffle / clear ──────────────
info "queue 编辑子命令"
if [ "$HAVE_FFMPEG" -eq 1 ]; then
    "$TM_BIN" queue append "$WORK_DIR/music/tone1.wav" >/dev/null 2>&1
    after_add="$("$TM_BIN" queue list --count 0 2>/dev/null | sed -n 's/^队列共 \([0-9]\{1,\}\) 首.*/\1/p')"
    [ "$after_add" = "4" ] && ok "queue append 使 total 3→4" || bad "queue append 后 total=$after_add（期望 4）"

    "$TM_BIN" queue append "$WORK_DIR/music/tone2.wav,$WORK_DIR/music/tone3.wav" >/dev/null 2>&1
    after_add2="$("$TM_BIN" queue list --count 0 2>/dev/null | sed -n 's/^队列共 \([0-9]\{1,\}\) 首.*/\1/p')"
    [ "$after_add2" = "6" ] && ok "queue append 逗号分隔一次追加 2 首 → total=6" || bad "逗号追加后 total=$after_add2（期望 6）"

    "$TM_BIN" queue insert 0 "$WORK_DIR/music/tone1.wav" >/dev/null 2>&1
    after_ins="$("$TM_BIN" queue list --count 0 2>/dev/null | sed -n 's/^队列共 \([0-9]\{1,\}\) 首.*/\1/p')"
    [ "$after_ins" = "7" ] && ok "queue insert 0 → total=7" || bad "insert 后 total=$after_ins（期望 7）"

    "$TM_BIN" queue remove 0 >/dev/null 2>&1
    after_rm="$("$TM_BIN" queue list --count 0 2>/dev/null | sed -n 's/^队列共 \([0-9]\{1,\}\) 首.*/\1/p')"
    [ "$after_rm" = "6" ] && ok "queue remove 0 → total=6" || bad "remove 后 total=$after_rm（期望 6）"

    "$TM_BIN" queue shuffle >/dev/null 2>&1 && ok "queue shuffle 返回 0" || bad "queue shuffle 失败"

    "$TM_BIN" queue clear >/dev/null 2>&1
    after_clr="$("$TM_BIN" queue list --count 0 2>/dev/null | sed -n 's/^队列共 \([0-9]\{1,\}\) 首.*/\1/p')"
    [ "$after_clr" = "0" ] && ok "queue clear → total=0" || bad "clear 后 total=$after_clr（期望 0）"

    # 越界 remove 应回核心业务错误 → REFUSED(5)
    "$TM_BIN" queue remove 999 >/dev/null 2>&1
    [ "$?" -eq 5 ] && ok "queue remove 越界退出码 5(REFUSED)" || bad "queue remove 越界退出码非 5"
else
    skip "queue 编辑（无素材）"
fi

# ── config get / set / reload / reset ─────────────────────────────
info "config 子命令"
"$TM_BIN" config get >/dev/null 2>&1 && ok "config get（完整 JSON）退出码 0" || bad "config get 失败"
"$TM_BIN" config set volume_percent 33 >/dev/null 2>&1 && ok "config set volume_percent=33 退出码 0" || bad "config set 失败"
got_vol="$("$TM_BIN" config get volume_percent 2>/dev/null)"
[ "$got_vol" = "33" ] && ok "config get volume_percent 回读 = 33" || bad "volume_percent 回读='$got_vol'（期望 33）"
"$TM_BIN" config reload >/dev/null 2>&1 && ok "config reload 退出码 0" || bad "config reload 失败"
# reset 无 --yes 必须 USAGE(1)
"$TM_BIN" config reset >/dev/null 2>&1
[ "$?" -eq 1 ] && ok "config reset 无 --yes 被拒（退出码 1）" || bad "config reset 无 --yes 未按预期拒绝"
"$TM_BIN" config reset --yes >/dev/null 2>&1 && ok "config reset --yes 退出码 0" || bad "config reset --yes 失败"
"$TM_BIN" config set not_a_real_key 1 >/dev/null 2>&1
[ "$?" -eq 1 ] && ok "config set 未知键 → USAGE(1)" || bad "config set 未知键未返回 1"

# ── lyrics / track / cover 只读输出 ───────────────────────────────
info "lyrics / track / cover"
# 先播一首，让 track/cover 有当前曲目
if [ "$HAVE_FFMPEG" -eq 1 ]; then
    "$TM_BIN" play "$WORK_DIR/music" >/dev/null 2>&1
    sleep 0.5
fi
"$TM_BIN" track >/dev/null 2>&1 && ok "track 退出码 0" || bad "track 失败"
"$TM_BIN" lyrics >/dev/null 2>&1 && ok "lyrics（当前行）退出码 0" || bad "lyrics 失败"
"$TM_BIN" lyrics --lines 50 >/dev/null 2>&1 && ok "lyrics --lines 50（GetDocument 分页）退出码 0" || bad "lyrics --lines 失败"
# cover：无封面时也应正常返回 0（输出“没有封面”提示，不算错误）
"$TM_BIN" cover >/dev/null 2>&1 && ok "cover 退出码 0" || bad "cover 失败"
"$TM_BIN" cover --width 16 --height 8 --charset ascii >/dev/null 2>&1 && ok "cover 显式尺寸/字符集退出码 0" || bad "cover 选项失败"
"$TM_BIN" cover --charset bogus >/dev/null 2>&1
[ "$?" -eq 1 ] && ok "cover 非法 --charset → USAGE(1)" || bad "cover 非法 charset 未返回 1"

# ── daemon status / restart --open ────────────────────────────────
info "daemon status / restart --open"
status_out="$("$TM_BIN" daemon status 2>/dev/null)"
printf '%s' "$status_out" | grep -q "实例：" && ok "daemon status 打印实例模式" || bad "daemon status 输出异常：$status_out"

if [ "$HAVE_FFMPEG" -eq 1 ]; then
    # restart --open：不得命中“核心不扫描目录”拒绝分支静默空转，
    # 必须由父进程装载内容后队列真正到位
    "$TM_BIN" daemon restart --open "$WORK_DIR/music" >/dev/null 2>&1
    sleep 1
    re_total="$("$TM_BIN" queue list --count 0 2>/dev/null | sed -n 's/^队列共 \([0-9]\{1,\}\) 首.*/\1/p')"
    if [ "$re_total" = "3" ] && daemon_up; then
        ok "daemon restart --open 后队列 total=3 且核心在线（非空转）"
    else
        bad "daemon restart --open 未装载内容（total=$re_total）"
    fi
else
    skip "restart --open（无素材）"
fi

# ── >500 大曲库分块下发 ───────────────────────────────────────────
info ">500 大曲库（BQ_SET_MAX 分块）"
stop_core
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1
if [ "$HAVE_FFMPEG" -eq 1 ]; then
    for i in $(seq 1 600); do
        ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=0.05" \
            -c:a pcm_s16le -y "$WORK_DIR/big/track$(printf '%03d' "$i").wav" 2>/dev/null
    done
    big_count="$(find "$WORK_DIR/big" -name '*.wav' 2>/dev/null | wc -l)"
    "$TM_BIN" play "$WORK_DIR/big" >/dev/null 2>&1
    play_big_rc=$?
    big_total="$("$TM_BIN" queue list --count 0 2>/dev/null | sed -n 's/^队列共 \([0-9]\{1,\}\) 首.*/\1/p')"
    if [ "$play_big_rc" -eq 0 ] && [ "$big_total" = "600" ]; then
        ok "play 600 首成功，queue list --count 0 total=600（分块 Set+Append 生效）"
    else
        bad "600 首场景：rc=$play_big_rc total=$big_total（期望 0/600），生成文件数=$big_count"
    fi
else
    skip ">500 场景（无 ffmpeg 生成占位音频）"
fi

# ── 错误路径：无实例退出码 3 ──────────────────────────────────────
info "无实例：控制/读子命令退出码应为 3"
stop_core
for cmd in "queue list" "config get" "lyrics" "track" "cover" "pause" "show"; do
    # shellcheck disable=SC2086
    "$TM_BIN" $cmd >/dev/null 2>&1
    rc=$?
    if [ "$rc" -eq 3 ]; then
        ok "无实例 [$cmd] → 退出码 3(NO_INSTANCE)"
    else
        bad "无实例 [$cmd] → 退出码 $rc（期望 3）"
    fi
done

# --bus 指向不存在的实例名：同样应判为无实例（3）
"$TM_BIN" queue list --bus org.mpris.MediaPlayer2.nope >/dev/null 2>&1
[ "$?" -eq 3 ] && ok "--bus 未知名 → 退出码 3" || bad "--bus 未知名退出码非 3"

info "结果"
printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
