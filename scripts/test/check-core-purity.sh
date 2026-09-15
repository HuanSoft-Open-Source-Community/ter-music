#!/usr/bin/env bash
#
# 核心纯度检查：核心不得认识“远程音乐源”
#
# 终态约定（见 .tmp/todo/前后端分离-核心服务化-TUI远程-待办.md）：
# 远程音乐源（SMB/SFTP/FTP/WebDAV/HTTP 的服务器列表、凭据、目录浏览、
# 下载）是**前端**功能，由 TUI 前端本地完成并把本地文件路径交给核心；
# 核心只播放本地文件，不得出现任何远程概念。
#
#   scripts/test/check-core-purity.sh            # 严格模式：有残留即退出 1
#   scripts/test/check-core-purity.sh --report   # 只报告，始终退出 0
#
# 扫描范围：核心侧目录（src 与 include 两侧）——
#   app/ audio/ cli/ config/ core/ info/ library/ media/ playlist/ search/
# 不在范围内：ui/（前端）、player/（门面：前端到核心的入口）、
#   remote/（远程实现，前端专用）、main/（前端进程入口）。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC_ROOT="$REPO_ROOT/src/org.yxzl.ter-music"
INC_ROOT="$REPO_ROOT/include/org.yxzl.ter-music"

MODE="strict"
[ "${1:-}" = "--report" ] && MODE="report"

CORE_DIRS=(app audio cli config core info library media playlist search)

# 远程符号与远程专用判定：核心侧出现即违规
PATTERNS='remote/remote\.h|remote_[a-z_]+\(|RemoteConnectionConfig|RemoteDirEntry|RPC_IFACE_REMOTE|RPC_JOB_REMOTE|rpc_remote_|load_remote_playlist|playlist_build_remote|HAVE_LIBCURL|#include <curl/|is_remote'

total=0
violating_files=0
declare -a rows=()
declare -a samples=()

for dir in "${CORE_DIRS[@]}"; do
    for file in "$SRC_ROOT/$dir"/*.c "$INC_ROOT/$dir"/*.h; do
        [ -e "$file" ] || continue
        count=$(grep -cE "$PATTERNS" "$file" 2>/dev/null || true)
        [ -z "$count" ] && count=0
        if [ "$count" -gt 0 ]; then
            violating_files=$((violating_files + 1))
            rows+=("$(printf '%4d  %s' "$count" "${file#"$REPO_ROOT"/}")")
            while IFS= read -r hit; do
                [ -n "$hit" ] && samples+=("$hit")
            done < <(grep -nE "$PATTERNS" "$file" 2>/dev/null | head -5 | sed "s|^|${file#"$REPO_ROOT"/}:|")
        fi
        total=$((total + count))
    done
done

echo "核心侧远程残留统计（核心应完全不知道“远程音乐源”）："
echo "  处数  文件"
if [ "${#rows[@]}" -gt 0 ]; then
    printf '%s\n' "${rows[@]}" | sort -rn
fi
echo "  ----  ----"
printf '%4d  合计（%d 个文件仍有远程残留）\n' "$total" "$violating_files"

if [ "$total" -eq 0 ]; then
    echo "PASS 核心已不认识远程：只接受本地路径"
    exit 0
fi

if [ "$MODE" = "report" ]; then
    echo "REPORT 迁移进行中：核心侧仍剩 $total 处远程引用"
    exit 0
fi

if [ "${#samples[@]}" -gt 0 ]; then
    echo "违规样例（每文件最多 5 条）："
    printf '  %s\n' "${samples[@]}"
fi
echo "FAIL 核心仍在认识远程（迁移完成前可用 --report 查看进度）"
exit 1
