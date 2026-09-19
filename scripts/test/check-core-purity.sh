#!/usr/bin/env bash
#
# 后端纯度检查：核心（播放服务）不得越界
#
# 2026-09-15 架构调整后的边界（见 .tmp/todo/前后端分离-核心服务化-TUI远程-待办.md）：
#   后端（daemon/核心）＝ 播放服务：音频设备、播放状态与进度、传输命令、音量/
#     倍速/播放模式、执行前端下发的路径队列、当前曲目信息（歌词/封面/可视化）、
#     配置、前端注册与心跳。
#   前端（TUI/CLI）＝ 文件系统与内容：曲库/SQLite、扫描与元数据、播放列表内容、
#     用户歌单、收藏/历史/目录历史、排序/过滤/搜索、远程源、界面。
#
# 本脚本检查两条互不重叠的红线（两条都必须为 0）：
#   A 远程残留：后端不得出现任何"远程音乐源"符号（远程源是前端功能）。
#   B 内容/UI 越界：后端不得引用内容模块（playlist/library/search）、内容全局、
#     UI 渲染函数与界面钩子；也不得 include 前端头文件。
#     例外：`ui/braille/` 的字符封面渲染（字符封面属"当前曲目信息"＝后端职责）。
#
#   scripts/test/check-core-purity.sh            # 严格模式：任一条非 0 即退出 1
#   scripts/test/check-core-purity.sh --report   # 只报告，始终退出 0
#
# 扫描范围＝后端目录（src 与 include 两侧）：audio cli config core info lyrics media queue
# 不在范围：ui/（前端）、playlist/ library/ search/ app/（内容，前端）、remote/（前端）、
#           player/（门面）、main/（前端进程入口）

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC_ROOT="$REPO_ROOT/src/org.yxzl.ter-music"
INC_ROOT="$REPO_ROOT/include/org.yxzl.ter-music"

MODE="strict"
[ "${1:-}" = "--report" ] && MODE="report"

BACKEND_DIRS=(audio config core info lyrics media queue)

# 后端文件清单（目录之外的例外与排除）
#   cli/daemon.c —— 无界面播放进程（后端外壳）
#   cli/cli.c、cli/cli_client.c —— **前端**：CLI 子命令与 D-Bus 客户端，负责扫描
#     内容、装配路径队列并下发给核心（内容归前端）
BACKEND_EXTRA_FILES=(cli/daemon.c)
BACKEND_EXCLUDE_FILES=(cli/cli.c cli/cli_client.c)

# A 远程符号
PATTERNS_REMOTE='remote/remote\.h|remote_[a-z_]+\(|RemoteConnectionConfig|RemoteDirEntry|RPC_IFACE_REMOTE|RPC_JOB_REMOTE|rpc_remote_|load_remote_playlist|playlist_build_remote|HAVE_LIBCURL|#include <curl/|is_remote'

# B 内容模块 / 内容全局 / UI 渲染与钩子
PATTERNS_CONTENT='#include "(playlist|library|search|app|player)/|#include "ui/|(^|[^A-Za-z0-9_])(g_playlist|g_playlist_manager|g_favorites|g_play_history|g_dir_history|g_sort_state|g_search_state|g_search_mutex|g_selected_index|g_library_state)|(^|[^A-Za-z0-9_])(playlist_[a-z_]+|library_[a-z_]+|search_[a-z_]+|render_[a-z_]+|request_ui_refresh|update_controls_status|exit_current_view|rerender_active_view|handle_menu_input|handle_settings_input|handle_library_input|init_all_persistent_data|init_menu_views|load_all_playlists|load_history|load_favorites|load_dir_history|add_history_entry|add_to_favorites|create_user_playlist|load_playlist|append_playlist|load_single_file|load_temp_playlist|save_temp_playlist|cleanup_temp_playlist|app_open_path|app_restore_session|library_load_into_playlist)\('

# 过滤：注释行、extern 声明行、以及字符封面渲染（后端职责）
filter_lines() {
    grep -vE '^[[:space:]]*(\*|/\*|//)' "$1" 2>/dev/null \
        | grep -vE '^[[:space:]]*extern[[:space:]]' \
        | grep -vE 'braille'
}

count_in() {   # $1=file $2=pattern
    filter_lines "$1" | grep -cE "$2" || true
}

total_remote=0
total_content=0
declare -a rows_remote=()
declare -a rows_content=()
declare -a samples=()

is_excluded() {
    local rel="$1" name
    for name in "${BACKEND_EXCLUDE_FILES[@]}"; do
        [ "$rel" = "$name" ] && return 0
    done
    return 1
}

scan_file() {
        local file="$1"
        [ -e "$file" ] || return 0
        local rel="${file#"$REPO_ROOT"/}"
        is_excluded "$rel" && return 0

        rem=$(count_in "$file" "$PATTERNS_REMOTE");  [ -z "$rem" ] && rem=0
        con=$(count_in "$file" "$PATTERNS_CONTENT"); [ -z "$con" ] && con=0

        if [ "$rem" -gt 0 ]; then
            rows_remote+=("$(printf '%4d  %s' "$rem" "$rel")")
        fi
        if [ "$con" -gt 0 ]; then
            rows_content+=("$(printf '%4d  %s' "$con" "$rel")")
            while IFS= read -r hit; do
                [ -n "$hit" ] && samples+=("$hit")
            done < <(filter_lines "$file" | grep -nE "$PATTERNS_CONTENT" | head -3 | sed "s|^|$rel:|")
        fi

        total_remote=$((total_remote + rem))
        total_content=$((total_content + con))
}

for dir in "${BACKEND_DIRS[@]}"; do
    for file in "$SRC_ROOT/$dir"/*.c "$INC_ROOT/$dir"/*.h; do
        scan_file "$file"
    done
done
for rel in "${BACKEND_EXTRA_FILES[@]}"; do
    scan_file "$SRC_ROOT/$rel"
done

echo "后端纯度统计："
echo "  ── A 远程残留（后端不得认识远程音乐源）"
if [ "${#rows_remote[@]}" -gt 0 ]; then
    printf '%s\n' "${rows_remote[@]}" | sort -rn
fi
printf '  合计 %d 处\n' "$total_remote"
echo "  ── B 内容/UI 越界（后端不得引用内容模块与界面）"
if [ "${#rows_content[@]}" -gt 0 ]; then
    printf '%s\n' "${rows_content[@]}" | sort -rn
fi
printf '  合计 %d 处\n' "$total_content"

if [ "$total_remote" -eq 0 ] && [ "$total_content" -eq 0 ]; then
    echo "PASS 后端只做播放：不认识远程，也不引用内容与界面"
    exit 0
fi

if [ "$MODE" = "report" ]; then
    printf 'REPORT 迁移进行中：远程 %d 处，内容/UI 越界 %d 处\n' "$total_remote" "$total_content"
    if [ "${#samples[@]}" -gt 0 ]; then
        echo "越界样例（每文件最多 3 条）："
        printf '  %s\n' "${samples[@]}"
    fi
    exit 0
fi

if [ "${#samples[@]}" -gt 0 ]; then
    echo "违规样例（每文件最多 3 条）："
    printf '  %s\n' "${samples[@]}"
fi
printf 'FAIL 后端仍在越界（远程 %d，内容/UI %d）\n' "$total_remote" "$total_content"
exit 1
