#!/usr/bin/env bash
#
# UI 纯度检查（重建版）：界面不得直连**播放面**
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
#   scripts/test/check-ui-purity.sh            # 报告（有违规即退出 1）
#   scripts/test/check-ui-purity.sh --report   # 只报告，始终退出 0
#
# 正则带词边界（避免 player_playlist_count( 被 playlist_count( 误计），并排除
# extern 声明行与注释行。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
UI_DIR="$REPO_ROOT/src/org.yxzl.ter-music/ui"

MODE="strict"
[ "${1:-}" = "--report" ] && MODE="report"

# 播放面全局（后端状态拥有者）
PLAYBACK_GLOBALS='(^|[^A-Za-z0-9_])(g_play_state|g_current_play_index|g_current_position|g_total_duration|g_playback_speed|g_play_mode|g_play_queue|g_play_thread_running|g_play_thread_active|g_play_thread_finished|g_pending_playback_index|g_speed_index|g_speed_ratios|g_speed_count|g_lyrics|g_cue_offset|g_seek_request|g_seek_position|g_initial_seek_position|g_active_backend|g_audio_sample_rate|g_audio_bit_rate|g_audio_bit_depth|g_audio_codec_name)'

# 播放面命令（必须经 player_* 门面下发）
PLAYBACK_COMMANDS='(^|[^A-Za-z0-9_])(play_audio|pause_audio|resume_audio|stop_audio|next_track|prev_track|seek_audio|set_volume_percent|get_volume_percent|adjust_volume|toggle_playback_speed|apply_playback_speed_change|set_play_mode|get_play_mode|get_play_mode_str|play_mode_display_name|play_queue_[a-z_]+|load_lyrics|reload_lyrics_with_source|update_lyrics_display|get_current_album_cover_path|eq_set_[a-z_]+|eq_is_enabled|eq_get_band_gain|eq_apply_preset|reset_playlist_state|playlist_install|get_visualizer_snapshot|audio_backend_shutdown|init_audio_device)\('

# 过滤：注释行与 extern 声明行（声明不是耦合）
filter_lines() {
    grep -vE '^[[:space:]]*(\*|/\*|//)' "$1" 2>/dev/null | grep -vE '^[[:space:]]*extern[[:space:]]'
}

count_in() {   # $1=file $2=pattern
    filter_lines "$1" | grep -cE "$2" || true
}

total_globals=0
total_commands=0
violating_files=0
declare -a rows=()
declare -a samples=()

for file in "$UI_DIR"/*.c "$UI_DIR"/braille/*.c; do
    [ -e "$file" ] || continue
    base="${file#"$UI_DIR"/}"

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

echo "UI 播放面耦合统计（引擎播放全局 / 引擎播放命令）："
echo "  全局  命令  文件"
if [ "${#rows[@]}" -gt 0 ]; then
    printf '%s\n' "${rows[@]}" | sort -rn
fi
echo "  ----  ----  ----"
printf '%4d %4d  合计（%d 个文件仍有播放面耦合）\n' "$total_globals" "$total_commands" "$violating_files"

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
