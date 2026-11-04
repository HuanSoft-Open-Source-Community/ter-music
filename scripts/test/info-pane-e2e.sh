#!/usr/bin/env bash
#
# 信息窗格 ↔ 信息显示设置 端到端回归
#
# 验证主页信息窗格确实读取 g_app_config.info_fields / info_show_cover：
#   T1 只开 state+title：窗格只显示这两行，不显示模式/音频；
#   T2 只开 format：窗格只有 4 行音频，不显示状态；
#   T3 全量：5 行元数据 + 4 行音频；
#   T4 封面双开关：两者都开 => cover_requested=1；任一关 => 0。
#
# 判定依据：
#   · 画面事实：lifecycle-driver.py 把 pty 原始字节写入 out 文件，vt_grid.py
#     还原字符网格后断言标签在不在；
#   · 渲染事实：--debug 日志里的 [info_bar] 行给出 mask/metadata/audio/
#     cover_requested/cover_drawn。
#
# 用法：scripts/test/info-pane-e2e.sh [--bin <ter-music>] [--keep]
# 环境：ffmpeg、python3；本地前端模式，不需要会话总线/音频设备。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""

usage() { sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

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
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

cleanup() {
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（用 --bin 指定）" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo "错误：需要 ffmpeg 造素材" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "错误：需要 python3" >&2; exit 2; }
[ -f "$SCRIPT_DIR/lifecycle-driver.py" ] || { echo "错误：缺少 lifecycle-driver.py" >&2; exit 2; }
[ -f "$SCRIPT_DIR/vt_grid.py" ] || { echo "错误：缺少 vt_grid.py" >&2; exit 2; }

DRIVER="$SCRIPT_DIR/lifecycle-driver.py"
VT="$SCRIPT_DIR/vt_grid.py"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-info-pane-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_CACHE_HOME="$HOME/.cache"
export TER_MUSIC_LOG_DIR="$WORK_DIR/log"
export TER_MUSIC_SANDBOX=0
mkdir -p "$XDG_CONFIG_HOME/ter-music" "$XDG_CACHE_HOME" "$TER_MUSIC_LOG_DIR" "$WORK_DIR/music"

CONFIG="$XDG_CONFIG_HOME/ter-music/config.xml"
MUSIC="$WORK_DIR/music"

info "造素材"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "sine=frequency=440:duration=1.2" \
    -metadata title="Info Pane E2E" -metadata artist="E2E Artist" \
    -metadata album="E2E Album" -c:a pcm_s16le -y "$MUSIC/e2e.wav" 2>/dev/null || {
    echo "错误：ffmpeg 造素材失败" >&2
    exit 2
}

write_config() {   # $1=mask $2=info_cover $3=album_cover
    cat > "$CONFIG" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <ui_language>zh_CN</ui_language>
    <info_preset>2</info_preset>
    <info_fields>$1</info_fields>
    <info_show_cover>$2</info_show_cover>
    <show_album_cover>$3</show_album_cover>
    <stop_core_on_exit>0</stop_core_on_exit>
  </preferences>
</ter-music-config>
EOF
}

run_case() {   # $1=name
    local name="$1"
    local ctl="$WORK_DIR/ctl-$name"
    local out="$WORK_DIR/out-$name"
    local log_dir="$TER_MUSIC_LOG_DIR/$name"
    local driver_pid

    rm -f "$ctl" "$out"
    mkdir -p "$log_dir"
    : > "$ctl"
    TER_MUSIC_LOG_DIR="$log_dir" python3 "$DRIVER" drive "$TM_BIN" "$MUSIC" "$out" \
        --ctl "$ctl" --cols 100 --rows 30 --max-runtime 20 \
        -- --debug --frontend=local >/dev/null 2>&1 &
    driver_pid=$!
    sleep 2
    printf 'wait:1\nkill\n' >> "$ctl"
    wait "$driver_pid" 2>/dev/null || true

    if [ ! -s "$out" ]; then
        bad "$name：TUI 没有输出任何画面"
        return 1
    fi
    printf '%s' "$out"
}

has_text() {   # $1=needle $2=file
    python3 "$VT" --has "$1" "$2" >/dev/null 2>&1
}

missing_text() {
    ! has_text "$1" "$2"
}

last_info_log() {
    grep -h "\[info_bar\]" "$TER_MUSIC_LOG_DIR"/"$1"/*.log 2>/dev/null | tail -1
}

# ── T1：state+title only ──────────────────────────────────────────
info "T1 info_fields=17（state+title）"
write_config 17 0 0
run_case t1 >/dev/null
T1="$WORK_DIR/out-t1"
if has_text '状态：' "$T1" && has_text '标题：' "$T1" &&
   missing_text '模式：' "$T1" && missing_text '采样率：' "$T1"; then
    ok "窗格只显示状态与标题"
else
    bad "窗格字段与 mask 不符"
fi
line="$(last_info_log t1)"
case "$line" in
    *"metadata=2"*"audio=0"*) ok "debug 行 metadata=2 audio=0" ;;
    *) bad "debug 行不符：${line:-<none>}" ;;
esac

# ── T2：format only ───────────────────────────────────────────────
info "T2 info_fields=128（format）"
write_config 128 0 0
run_case t2 >/dev/null
T2="$WORK_DIR/out-t2"
if has_text '采样率：' "$T2" && missing_text '状态：' "$T2"; then
    ok "窗格只显示音频块"
else
    bad "format-only 时仍出现/缺少错误字段"
fi
line="$(last_info_log t2)"
case "$line" in
    *"metadata=0"*"audio=4"*) ok "debug 行 metadata=0 audio=4" ;;
    *) bad "debug 行不符：${line:-<none>}" ;;
esac

# ── T3：full ─────────────────────────────────────────────────────
info "T3 info_fields=2047（全量）"
write_config 2047 0 0
run_case t3 >/dev/null
T3="$WORK_DIR/out-t3"
if has_text '状态：' "$T3" && has_text '模式：' "$T3" &&
   has_text '标题：' "$T3" && has_text '艺术家：' "$T3" &&
   has_text '专辑：' "$T3" && has_text '采样率：' "$T3"; then
    ok "全量字段都出现"
else
    bad "全量字段缺失"
fi
line="$(last_info_log t3)"
case "$line" in
    *"metadata=5"*"audio=4"*) ok "debug 行 metadata=5 audio=4" ;;
    *) bad "debug 行不符：${line:-<none>}" ;;
esac

# ── T4：封面双开关 ───────────────────────────────────────────────
info "T4 封面请求开关"
write_config 2047 1 1
run_case t4a >/dev/null
line="$(last_info_log t4a)"
case "$line" in
    *"cover_requested=1"*) ok "两个开关都开 => cover_requested=1" ;;
    *) bad "cover_requested 应为 1：${line:-<none>}" ;;
esac

write_config 2047 0 1
run_case t4b >/dev/null
line="$(last_info_log t4b)"
case "$line" in
    *"cover_requested=0"*) ok "info_show_cover=0 => cover_requested=0" ;;
    *) bad "cover_requested 应为 0：${line:-<none>}" ;;
esac

write_config 2047 1 0
run_case t4c >/dev/null
line="$(last_info_log t4c)"
case "$line" in
    *"cover_requested=0"*) ok "show_album_cover=0 => cover_requested=0" ;;
    *) bad "cover_requested 应为 0：${line:-<none>}" ;;
esac

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
