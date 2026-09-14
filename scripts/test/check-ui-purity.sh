#!/usr/bin/env bash
#
# UI 纯度检查：界面不得直连引擎
#
# M3 的目标是 UI 只经 player_* 门面读写状态。本脚本按文件统计 ui/ 下对引擎
# 全局与引擎命令的直接引用，作为迁移进度表与最终验收门槛：
#
#   scripts/test/check-ui-purity.sh            # 报告（有违规即退出 1）
#   scripts/test/check-ui-purity.sh --report   # 只报告，始终退出 0
#
# 允许清单：ui/lyrics.c 持有歌词状态（g_lyrics*）——歌词推进由核心经
# core_set_lyrics_tick 回调进入该模块，属 M1 已确立的边界，迁移到门面后
# 再由 M6 收编；ui/dialog.c 的本地文件遍历是客户端行为，不算引擎耦合。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
UI_DIR="$REPO_ROOT/src/org.yxzl.ter-music/ui"

MODE="strict"
[ "${1:-}" = "--report" ] && MODE="report"

# 引擎全局（状态拥有者不在界面）
GLOBALS='g_play_queue|g_play_state|g_current_play_index|g_total_duration|g_current_position|g_playback_speed|g_playlist\b|g_playlist_manager|g_sort_state|g_search_state|g_favorites|g_play_history|g_dir_history|g_seek_mutex'

# 引擎命令（必须经 player_* 下发）
COMMANDS='play_audio\(|pause_audio\(|resume_audio\(|stop_audio\(|next_track\(|prev_track\(|cycle_play_mode\(|set_play_mode\(|get_play_mode\(|set_volume_percent\(|adjust_volume\(|seek_audio\(|toggle_playback_speed\(|apply_playback_speed_change\(|play_queue_[a-z_]*\(|load_playlist\(|append_playlist\(|load_single_file\(|load_remote_playlist\(|recompute_sort_order\(|clear_metadata_cache\(|preload_visible_tracks\(|library_[a-z_]*\(|search_async_[a-z_]*\(|playlist_count\(|playlist_page\(|playlist_visible_count\(|playlist_tree_is_active\(|playlist_toggle_directory_expand\(|playlist_reveal_track\(|playlist_is_loaded\(|playlist_copy_folder_path\(|playlist_get_track_path\(|playlist_find_track_index_by_path\(|get_track_metadata\(|track_matches_query\('

report_file() {
    local file="$1"
    local globals commands
    globals=$(grep -cE "$GLOBALS" "$file" 2>/dev/null || true)
    commands=$(grep -cE "$COMMANDS" "$file" 2>/dev/null || true)
    printf '%4d %4d  %s\n' "$globals" "$commands" "${file#"$UI_DIR"/}"
    echo $((globals + commands))
}

total_globals=0
total_commands=0
violating_files=0
declare -a rows=()

for file in "$UI_DIR"/*.c "$UI_DIR"/braille/*.c; do
    [ -e "$file" ] || continue
    base=$(basename "$file")

    # 允许清单：歌词状态与本地文件遍历
    if [ "$base" = "lyrics.c" ]; then
        continue
    fi

    globals=$(grep -cE "$GLOBALS" "$file" 2>/dev/null || true)
    commands=$(grep -cE "$COMMANDS" "$file" 2>/dev/null || true)
    theming=$(grep -cE "g_app_config" "$file" 2>/dev/null || true)
    sum=$((globals + commands))

    if [ "$sum" -gt 0 ]; then
        violating_files=$((violating_files + 1))
        rows+=("$(printf '%4d %4d %4d  %s' "$globals" "$commands" "$theming" "$base")")
    fi
    total_globals=$((total_globals + globals))
    total_commands=$((total_commands + commands))
done

echo "UI 引擎耦合统计（引擎全局 / 引擎命令 / 配置读取）："
echo "  全局  命令  配置  文件"
if [ "${#rows[@]}" -gt 0 ]; then
    printf '%s\n' "${rows[@]}" | sort -rn
fi
echo "  ----  ----  ----  ----"
printf '%4d %4d       合计（%d 个文件仍有耦合）\n' "$total_globals" "$total_commands" "$violating_files"

if [ "$total_globals" -eq 0 ] && [ "$total_commands" -eq 0 ]; then
    echo "PASS UI 已完全经 player 门面访问核心"
    exit 0
fi

if [ "$MODE" = "report" ]; then
    echo "REPORT 迁移进行中：仍剩 $((total_globals + total_commands)) 处直接引用"
    exit 0
fi

echo "FAIL UI 仍在直连引擎（迁移完成前请用 --report 查看进度）"
exit 1
