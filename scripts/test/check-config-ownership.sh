#!/usr/bin/env bash
#
# 配置归属检查：前端不得直写 config.xml
#
# 配置的拥有者是核心（playback core）：`config.xml` 由它独占写。前端（TUI/CLI）
# 只能改自己的配置镜像，再经门面 `player_config_persist()` 把差异下发——
#   - 本地后端：前端与核心同进程，门面内部就是 save_config()；
#   - 远端后端：差异经 Config.Set 交给核心，核心落盘并回广播 ConfigChanged。
# 前端目录里直接出现 save_config() / config_save_to_xml() 就是越权写盘，
# 两个进程会互相覆盖（验收 A4 的"无文件写竞争"）。
#
#   scripts/test/check-config-ownership.sh            # 严格：有违规即退出 1
#   scripts/test/check-config-ownership.sh --report   # 只报告，始终退出 0
#
# 扫描范围（前端平面）：src/.../ui/、src/.../app/、src/.../main/
# 不在范围：config/（核心的配置实现）、media/（核心的 Config 接口）、
#           player/（门面：两端各有一份实现）、cli/daemon.c（核心入口）

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC_ROOT="$REPO_ROOT/src/org.yxzl.ter-music"

MODE="strict"
[ "${1:-}" = "--report" ] && MODE="report"

FRONT_DIRS=(ui app main)
PATTERNS='save_config\(|config_save_to_xml\('

total=0
declare -a rows=()
declare -a samples=()

for dir in "${FRONT_DIRS[@]}"; do
    for file in "$SRC_ROOT/$dir"/*.c; do
        [ -e "$file" ] || continue
        # 注释行不算违规
        count=$(grep -vE '^[[:space:]]*(\*|/\*|//)' "$file" | grep -cE "$PATTERNS" || true)
        [ -z "$count" ] && count=0
        if [ "$count" -gt 0 ]; then
            rows+=("$(printf '%4d  %s' "$count" "${file#"$REPO_ROOT"/}")")
            while IFS= read -r hit; do
                [ -n "$hit" ] && samples+=("$hit")
            done < <(grep -nvE '^[[:space:]]*(\*|/\*|//)' "$file" | grep -E "$PATTERNS" | head -4 |
                     sed "s|^|${file#"$REPO_ROOT"/}:|")
        fi
        total=$((total + count))
    done
done

echo "前端直写核心配置统计（应为 0，全部经 player_config_persist()）："
echo "  处数  文件"
if [ "${#rows[@]}" -gt 0 ]; then
    printf '%s\n' "${rows[@]}" | sort -rn
fi
echo "  ----  ----"
printf '%4d  合计\n' "$total"

if [ "$total" -eq 0 ]; then
    echo "PASS 前端不再直写 config.xml：设置改动经门面下发"
    exit 0
fi

if [ "$MODE" = "report" ]; then
    echo "REPORT 迁移进行中：前端仍剩 $total 处直写配置"
    exit 0
fi

if [ "${#samples[@]}" -gt 0 ]; then
    echo "违规样例（每文件最多 4 条）："
    printf '  %s\n' "${samples[@]}"
fi
echo "FAIL 前端仍在直写核心配置（请改用 player_config_persist()）"
exit 1
