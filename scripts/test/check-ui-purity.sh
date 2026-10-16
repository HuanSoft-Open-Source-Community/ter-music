#!/usr/bin/env bash
#
# UI 纯度检查（重建版 + Task 10.2 强化）：界面不得直连**播放面**
#
# 口径（2026-09-15 架构调整后）：
#   后端（daemon/核心）＝ 播放服务：音频设备、播放状态与进度、传输命令、
#     音量/倍速/播放模式、执行前端下发的路径队列、当前曲目信息（歌词/封面/
#     可视化）、配置、前端注册与心跳。
#   前端（TUI/CLI）＝ 文件系统与内容：曲库/SQLite、扫描与元数据、播放列表
#     内容、用户歌单、收藏/历史/目录历史、排序/过滤/搜索、远程源、界面。
#
# 因此本脚本只统计"前端**播放面**的直连"：
#   - 引擎播放全局（g_play_state / g_play_queue / g_current_position …）
#   - 引擎播放命令（play_audio / seek_audio / set_play_mode / play_queue_* …）
# 曲库、播放列表内容、收藏/历史、排序/搜索等**内容面**调用属前端自有，不计。
#
# Task 10.2 强化点（2026-10-16）：
#   1. 播放命令黑名单补入引擎进度/队列符号：playlist_queue_sync、
#      progress_tracker_[a-z_]+（此前 ui/progress_ui.c 直读进度跟踪器是死耦合）。
#   2. 扫描面从 ui/ 扩到 ui/、main/、app/（"直调引擎符号"三目录全覆盖；
#      include 面维持原策略不扩）。
#   3. 堵"函数体内 extern 后同行直调"绕过：原先用
#      `grep -vE '^[[:space:]]*extern'` 整行跳过，会让
#      `extern void play_audio(int); play_audio(0);` 漏检。改为**仅剥离
#      extern 声明本身**，声明之后若仍有调用代码则继续命中黑名单。
#   4. media_session_tick 的取舍：**不纳入黑名单**（保持既有豁免）。依据
#      media/session.c 头注——它是"媒体循环线程里泵 D-Bus 属性/信号同步"的
#      钩子（session.c:12、rpc_queue.c:66 注释），不触碰 g_play_state、不起
#      播放线程、不发音频命令，属核心宿主在嵌套输入循环（ui.c:973/1140、
#      dialog.c:202、search.c:195）里的既定事件泵，非"播放面命令"。现脚本
#      设计者本就未将其列入黑名单，故此处延续该豁免，不强行扩大战果。
#   5. 确属"宿主职责（TUI-即-核心）"的既有调用点（main.c 与 app/open.c 的
#      local 后端分支直调引擎初始化/播放）用**显式行尾标记**
#      `ui-purity-allow: 理由` 放行（也支持紧邻上一行的独立注释标记放行下一行），
#      而不是整目录放行；脚本统计并打印豁免使用处数。
#
#   scripts/test/check-ui-purity.sh            # 严格（有违规即退出 1）
#   scripts/test/check-ui-purity.sh --report   # 只报告，始终退出 0
#
# 正则带词边界（避免 player_playlist_count( 被 playlist_count( 误计）。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC_ROOT="$REPO_ROOT/src/org.yxzl.ter-music"

MODE="strict"
[ "${1:-}" = "--report" ] && MODE="report"

# 播放面全局（后端状态拥有者）
PLAYBACK_GLOBALS='(^|[^A-Za-z0-9_])(g_play_state|g_current_play_index|g_current_position|g_total_duration|g_playback_speed|g_play_mode|g_play_queue|g_play_thread_running|g_play_thread_active|g_play_thread_finished|g_pending_playback_index|g_speed_index|g_speed_ratios|g_speed_count|g_lyrics|g_cue_offset|g_seek_request|g_seek_position|g_initial_seek_position|g_active_backend|g_audio_sample_rate|g_audio_bit_rate|g_audio_bit_depth|g_audio_codec_name)'

# 播放面命令（必须经 player_* 门面下发）。media_session_tick 依设计豁免、不入表
# （理由见头注第 4 点），其余引擎命令/进度/队列符号一律计入。
PLAYBACK_COMMANDS='(^|[^A-Za-z0-9_])(play_audio|pause_audio|resume_audio|stop_audio|next_track|prev_track|seek_audio|set_volume_percent|get_volume_percent|adjust_volume|toggle_playback_speed|apply_playback_speed_change|set_play_mode|get_play_mode|get_play_mode_str|play_mode_display_name|play_queue_[a-z_]+|playlist_queue_sync|progress_tracker_[a-z_]+|load_lyrics|reload_lyrics_with_source|get_current_album_cover_path|eq_set_[a-z_]+|eq_is_enabled|eq_get_band_gain|eq_apply_preset|reset_playlist_state|playlist_install|get_visualizer_snapshot|audio_backend_shutdown|init_audio_device)\('

# 计入围前过滤（Task 10.2）：
#   1) 含 ui-purity-allow: 的行：行尾同行标记→豁免本行；独立注释行标记→
#      豁免其后第一条代码行。均不计违规。
#   2) 纯注释行（以 *、/*、// 开头）与空行不计。
#   3) 纯 extern 声明行豁免声明本身；若 extern 之后同行仍有其它代码，剥离声明
#      后继续检查（堵"extern 后同行直调"绕过）。
filter_lines() {
    awk '
        function trim(s){ sub(/^[[:space:]]+/,"",s); sub(/[[:space:]]+$/,"",s); return s }
        {
            line = $0
            if (line ~ /ui-purity-allow:/) {
                t = trim(line)
                if (t ~ /^(\*|\/\*|\/\/)/) allow_next = 1   # 独立标记行：放行下一代码行
                next                                          # 同行/独立均本行放行
            }
            t = trim(line)
            if (t ~ /^(\*|\/\*|\/\/)/ || t == "") next
            if (t ~ /^extern[[:space:]]/) {
                rest = line
                sub(/^[[:space:]]*extern[[:space:]]+[^;]*;/, " ", rest)
                sub(/\/\/.*$/, "", rest)
                if (trim(rest) == "") next                      # 纯声明→整行豁免
                line = rest                                     # 声明+调用→保留调用
            }
            if (allow_next) { allow_next = 0; next }
            print line
        }' "$1" 2>/dev/null
}

count_in() {   # $1=file $2=pattern
    filter_lines "$1" | grep -cE "$2" || true
}

total_globals=0
total_commands=0
violating_files=0
declare -a rows=()
declare -a samples=()

# 扫描目录（Task 10.2 扩面）：ui/（含 braille 子目录）、main/、app/
scan_files=()
for f in "$SRC_ROOT"/ui/*.c "$SRC_ROOT"/ui/braille/*.c \
         "$SRC_ROOT"/main/*.c "$SRC_ROOT"/app/*.c; do
    [ -e "$f" ] || continue
    scan_files+=("$f")
done

for file in "${scan_files[@]}"; do
    base="${file#"$SRC_ROOT"/}"

    globals=$(count_in "$file" "$PLAYBACK_GLOBALS")
    commands=$(count_in "$file" "$PLAYBACK_COMMANDS")
    [ -z "$globals" ] && globals=0
    [ -z "$commands" ] && commands=0
    sum=$((globals + commands))

    if [ "$sum" -gt 0 ]; then
        violating_files=$((violating_files + 1))
        rows+=("$(printf '%4d %4d  %s' "$globals" "$commands" "$base")")
        while IFS= read -r hit; do
            [ -n "$hit" ] && samples+=("$hit")
        done < <(filter_lines "$file" | grep -nE "$PLAYBACK_GLOBALS|$PLAYBACK_COMMANDS" | head -4 | sed "s|^|${base}:|")
    fi

    total_globals=$((total_globals + globals))
    total_commands=$((total_commands + commands))
done

allows=$(for f in "${scan_files[@]}"; do grep -c 'ui-purity-allow:' "$f" 2>/dev/null || true; done | awk '{s+=$1} END{print s+0}')

echo "UI 播放面耦合统计（引擎播放全局 / 引擎播放命令）："
echo "  全局  命令  文件"
if [ "${#rows[@]}" -gt 0 ]; then
    printf '%s\n' "${rows[@]}" | sort -rn
fi
echo "  ----  ----  ----"
printf '%4d %4d  合计（%d 个文件仍有播放面耦合）\n' "$total_globals" "$total_commands" "$violating_files"
printf '   %d  显式豁免（ui-purity-allow，TUI-即-核心宿主分支）\n' "$allows"

total=$((total_globals + total_commands))

if [ "$total" -eq 0 ]; then
    echo "PASS 前端已完全经 player 门面访问播放面（内容面由前端自有）"
    exit 0
fi

if [ "$MODE" = "report" ]; then
    echo "REPORT 迁移进行中：播放面仍剩 $total 处直接引用"
    if [ "${#samples[@]}" -gt 0 ]; then
        echo "样例（每文件最多 4 条）："
        printf '  %s\n' "${samples[@]}"
    fi
    exit 0
fi

if [ "${#samples[@]}" -gt 0 ]; then
    echo "违规样例（每文件最多 4 条）："
    printf '  %s\n' "${samples[@]}"
fi
echo "FAIL 前端仍在直连播放面（迁移完成前可用 --report 查看进度）"
exit 1
