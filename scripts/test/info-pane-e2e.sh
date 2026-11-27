#!/usr/bin/env bash
#
# 信息窗格 ↔ 信息显示设置 端到端回归
#
# 验证主页信息窗格确实读取 g_app_config.info_fields / info_show_cover：
#   T1 只开 state+title：窗格只显示这两行，不显示模式/音频；
#   T2 只开 format：窗格只有 4 行音频，不显示状态；
#   T3 全量：5 行元数据 + 4 行音频；
#   T4 封面双开关：两者都开 => cover_requested=1；任一关 => 0；
#   T5 窄而高 + 开封面：auto 隐藏封面、长文本省略为 …、文字行不被压没；
#   T6 宽而矮：块内多列流动，5+4 行全部可见（cols=3/2）；
#   T7 info_cover_priority 三态：auto/text-first 隐藏封面，cover-first 保留封面并裁文字；
#   T8 高度不足：按优先级裁剪（状态>标题>艺术家>模式>专辑；采样率>位深>比特率>编码）。
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

usage() { sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

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
CASE_COLS=100
CASE_ROWS=30
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

# 封面/长文本素材：FLAC + 内嵌 PNG（attached_pic），标题/艺术家/专辑故意超长，
# 用来在窄窗格里触发尾部省略（…）。播放一次后 player_cover_path() 才有封面。
COVER_MUSIC="$WORK_DIR/cover-music"
mkdir -p "$COVER_MUSIC"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "color=c=red:s=120x120:d=1" -frames:v 1 -y "$WORK_DIR/cover.png" 2>/dev/null || {
    echo "错误：ffmpeg 造封面素材失败" >&2
    exit 2
}
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "sine=frequency=440:duration=8" -i "$WORK_DIR/cover.png" \
    -map 0:a -map 1:v -c:a flac -c:v png -disposition:v attached_pic \
    -metadata title="A Very Long Track Title That Cannot Fit In A Narrow Pane 1234567890" \
    -metadata artist="A Very Long Artist Name For Ellipsis Testing 0987654321" \
    -metadata album="A Very Long Album Name For Ellipsis Testing ABCDEFGHIJ" \
    -y "$COVER_MUSIC/cover.flac" 2>/dev/null || {
    echo "错误：ffmpeg 造封面音轨失败" >&2
    exit 2
}
# 短标签封面素材：宽而矮/优先级用例只用“行数/封面”判定，避免长文本改变
# 两块的 desired width（长标签会把音频块挤成单子列，掩盖 5+4 全可见的验证）。
SHORT_COVER_MUSIC="$WORK_DIR/short-cover-music"
mkdir -p "$SHORT_COVER_MUSIC"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "sine=frequency=440:duration=8" -i "$WORK_DIR/cover.png" \
    -map 0:a -map 1:v -c:a flac -c:v png -disposition:v attached_pic \
    -metadata title="Cover Track" -metadata artist="Cover Artist" \
    -metadata album="Cover Album" \
    -y "$SHORT_COVER_MUSIC/cover.flac" 2>/dev/null || {
    echo "错误：ffmpeg 造短标签封面音轨失败" >&2
    exit 2
}

write_config() {   # $1=mask $2=info_cover $3=album_cover [$4=info_cover_priority]
    local priority="${4:-0}"
    cat > "$CONFIG" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <ui_language>zh_CN</ui_language>
    <info_preset>2</info_preset>
    <info_fields>$1</info_fields>
    <info_show_cover>$2</info_show_cover>
    <show_album_cover>$3</show_album_cover>
    <info_cover_priority>$priority</info_cover_priority>
    <stop_core_on_exit>0</stop_core_on_exit>
  </preferences>
</ter-music-config>
EOF
}

run_case() {   # $1=name [$2=cols] [$3=rows] [$4=play(0/1)] [$5=music dir]
    local name="$1"
    local cols="${2:-100}" rows="${3:-30}" play="${4:-0}"
    CASE_COLS="$cols"; CASE_ROWS="$rows"
    local music="${5:-$MUSIC}"
    local ctl="$WORK_DIR/ctl-$name"
    local out="$WORK_DIR/out-$name"
    local log_dir="$TER_MUSIC_LOG_DIR/$name"
    local driver_pid

    rm -f "$ctl" "$out"
    mkdir -p "$log_dir"
    : > "$ctl"
    TER_MUSIC_LOG_DIR="$log_dir" \
        python3 "$DRIVER" drive "$TM_BIN" "$music" "$out" \
        --ctl "$ctl" --cols "$cols" --rows "$rows" --max-runtime 20 \
        -- --debug --frontend=local >/dev/null 2>&1 &
    driver_pid=$!
    sleep 1.6
    if [ "$play" = "1" ]; then
        # 列表首行在 SGR 行 3（标题行之后）；播放一次让封面进缓存。
        # 封面解码/缓存是异步的：最多等 5s 直到日志里出现 cover_drawn=1
        # （期望 cover_drawn=0 的用例会等满，但不影响判定）。
        printf 'key:<click:6:3>\n' >> "$ctl"
        local i
        for i in $(seq 1 20); do
            sleep 0.25
            grep -h "\[info_bar\]" "$log_dir"/*.log 2>/dev/null | tail -1 |
                grep -q "cover_drawn=1" && break
        done
    else
        sleep 0.4
    fi
    printf 'wait:0.5\nkill\n' >> "$ctl"
    wait "$driver_pid" 2>/dev/null || true

    if [ ! -s "$out" ]; then
        bad "$name：TUI 没有输出任何画面"
        return 1
    fi
    printf '%s' "$out"
}

has_text() {   # $1=needle $2=file（网格尺寸与最近一次 run_case 一致）
    python3 "$VT" --cols "$CASE_COLS" --rows "$CASE_ROWS" --has "$1" "$2" >/dev/null 2>&1
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

# ── T5：窄而高 + 开封面（auto）→ 隐藏封面、文字省略而不是压没 ─────
info "T5 窄而高 + 开封面：auto 隐藏封面，长文本尾部省略"
write_config 2047 1 1 0
run_case t5 50 30 1 "$COVER_MUSIC" >/dev/null
T5="$WORK_DIR/out-t5"
line="$(last_info_log t5)"
case "$line" in
    *"metadata=5/5"*"cover_requested=1"*"cover_drawn=0"*"cover_priority=0"*"stacked=1"*)
        ok "窄而高：封面按 auto 隐藏，5 行元数据全保留（stacked=1）" ;;
    *) bad "窄而高布局不符：${line:-<none>}" ;;
esac
if has_text '状态：' "$T5" && has_text '标题：' "$T5"; then
    ok "窄窗格文字行仍以标签呈现（未被封面挤没）"
else
    bad "窄窗格文字行缺失"
fi
if has_text '…' "$T5"; then
    ok "超长标题/艺术家尾部省略为 …（不是溢出/截断到不可读）"
else
    bad "窄窗格没有出现省略号 …"
fi
if missing_text '采样率：' "$T5"; then
    ok "窄而高放不下音频块时（audio=0/4）不画半截标签"
else
    bad "窄而高仍画了音频标签"
fi

# ── T6：宽而矮 → 块内多列流动，5+4 行全可见 ──────────────────────
info "T6 宽而矮：块内多列流动，5+4 行全部可见"
write_config 2047 1 1 0
run_case t6 200 14 1 "$SHORT_COVER_MUSIC" >/dev/null
T6="$WORK_DIR/out-t6"
line="$(last_info_log t6)"
case "$line" in
    *"metadata=5/5"*"audio=4/4"*"stacked=0"*"cols=3/2"*)
        ok "宽而矮：元数据 5 行铺 3 子列、音频 4 行铺 2 子列（cols=3/2）" ;;
    *) bad "宽而矮布局不符：${line:-<none>}" ;;
esac
if has_text '专辑：' "$T6" && has_text '编码：' "$T6"; then
    ok "多列流动后低优先级字段（专辑/编码）仍可见"
else
    bad "多列流动后仍缺字段"
fi

# ── T7：info_cover_priority 三态 ─────────────────────────────────
info "T7 info_cover_priority=0/1/2 行为差异"
write_config 2047 1 1 0
run_case t7a 60 40 1 "$SHORT_COVER_MUSIC" >/dev/null
line="$(last_info_log t7a)"
case "$line" in
    *"metadata=5/5"*"cover_drawn=0"*"cover_priority=0"*)
        ok "auto(0)：60x40 下隐藏封面保住全部文字" ;;
    *) bad "auto(0) 行为不符：${line:-<none>}" ;;
esac
write_config 2047 1 1 1
run_case t7b 60 40 1 "$SHORT_COVER_MUSIC" >/dev/null
line="$(last_info_log t7b)"
case "$line" in
    *"metadata=5/5"*"cover_drawn=0"*"cover_priority=1"*)
        ok "text-first(1)：同样以文字优先（封面隐藏）" ;;
    *) bad "text-first(1) 行为不符：${line:-<none>}" ;;
esac
write_config 2047 1 1 2
run_case t7c 60 40 1 "$SHORT_COVER_MUSIC" >/dev/null
T7C="$WORK_DIR/out-t7c"
line="$(last_info_log t7c)"
case "$line" in
    *"metadata=1/5"*"cover_drawn=1"*"cover_priority=2"*)
        ok "cover-first(2)：保留封面，文字按优先级裁到 1 行" ;;
    *) bad "cover-first(2) 行为不符：${line:-<none>}" ;;
esac
if missing_text '标题：' "$T7C" && has_text '状态：' "$T7C"; then
    ok "cover-first 下只保留最高优先级的状态行"
else
    bad "cover-first 下裁剪的可见行不符"
fi

# ── T8：高度不足按优先级裁剪 ─────────────────────────────────────
info "T8 高度不足：按优先级裁剪（状态>标题>艺术家>模式>专辑）"
write_config 2047 0 0 0
run_case t8 100 20 0 >/dev/null
T8="$WORK_DIR/out-t8"
line="$(last_info_log t8)"
case "$line" in
    *"metadata=3/5"*"audio=3/4"*)
        ok "100x20 高度不足：保留元数据 3/5、音频 3/4 行" ;;
    *) bad "高度裁剪行数不符：${line:-<none>}" ;;
esac
if has_text '状态：' "$T8" && has_text '标题：' "$T8" && has_text '艺术家：' "$T8"; then
    ok "高优先级元数据（状态/标题/艺术家）保留"
else
    bad "高优先级元数据缺失"
fi
if missing_text '模式：' "$T8" && missing_text '专辑：' "$T8"; then
    ok "低优先级元数据（模式/专辑）先被裁掉（旧实现会先丢专辑/艺术家）"
else
    bad "低优先级元数据没有被优先裁掉"
fi
if has_text '采样率：' "$T8" && has_text '位深：' "$T8" && has_text '比特率：' "$T8" &&
   missing_text '编码：' "$T8"; then
    ok "音频块按 采样率>位深>比特率>编码 裁剪"
else
    bad "音频块裁剪顺序不符"
fi

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
