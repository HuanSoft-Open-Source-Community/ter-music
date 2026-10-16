#!/usr/bin/env bash
#
# 配置归属检查：前端不得直写 config.xml
#
# 配置的拥有者是核心（playback core）：`config.xml` 由它独占写。前端（TUI/CLI）
# 只能改自己的配置镜像，再经门面 `player_config_persist()` 把差异下发——
#   - 本地后端：前端与核心同进程，门面内部就是 save_config()；
#   - 远端后端：差异经 Config.Set 交给核心，核心落盘并回广播 ConfigChanged。
# 前端平面里出现任何落盘入口的直接调用（或经其它函数的间接调用）就是越权写盘，
# 两个进程会互相覆盖。
#
# 验收 A4 口径（Task 10.1 强化，2026-10-16）：
#   "无文件写竞争" = 前端平面不存在可达落盘入口的写者。命中清单按头文件实读：
#     save_config()                    —— config/config.h（原子写入配置文件）
#     config_save_to_xml()             —— config/config.h（序列化到任意路径）
#     persist_playback_session_state() —— audio/audio.h（会话恢复持久化，内部写
#                                         config.xml；前端调用它即"间接写"盲区）
#     reset_config()                   —— 防御性名字：当前 config.h/audio.h 并无
#                                         该独立函数，Config.Reset 的落盘由核心
#                                         save_config() 完成；列入仅为防将来出现
#                                         同名"恢复出厂写盘"入口绕过门面。
#
# 分域判定依据（Task 10.1）：
#   前端文件（永不得直写，纳入扫描）：
#     ui/、main/、app/            —— 界面与前端进程代码
#     player/player_remote.c      —— 远端门面实现：纯 D-Bus 代理，本平面内
#                                    不存在"它就是核心"的时刻，落盘调用必为违规
#   宿主文件（核心/宿主实现，允许落盘，不在扫描面）：
#     player/player_local.c       —— 本地门面实现：TUI-即-核心，它就是配置拥有
#                                    者（player_config_persist 的本地分支）
#     cli/daemon.c、config/、media/、audio/ —— 核心进程与核心实现
#   前端共享代码里确属宿主职责的历史调用（如 cleanup() 中仅在
#   player_backend()==PLAYER_BACKEND_LOCAL 分支执行的会话持久化）用**显式豁免
#   标记**放行：违规行同行或紧邻上一行的注释含 `config-ownership-allow: 理由`；
#   脚本统计并打印豁免处数，禁止整目录放行。
#   extern 声明行（整行仅为 `extern ... ;`）不是调用，不计违规；若声明与调用
#   同行（`extern void f(void); f();`），剥离声明后调用仍会被命中。
#
#   scripts/test/check-config-ownership.sh            # 严格：有违规即退出 1
#   scripts/test/check-config-ownership.sh --report   # 只报告，始终退出 0

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC_ROOT="$REPO_ROOT/src/org.yxzl.ter-music"

MODE="strict"
[ "${1:-}" = "--report" ] && MODE="report"

FRONT_DIRS=(ui app main)
# 前端专属实现文件（分域判定见头注）：远端门面同样禁止直写。
FRONT_FILES=(player/player_remote.c)
PATTERNS='save_config\(|config_save_to_xml\(|persist_playback_session_state\(|reset_config\('

# 计入围前过滤：
#   1) 含 config-ownership-allow: 的行（同行豁免）与"独立豁免标记注释行"
#      （标记紧跟其违规行的上一行时豁免下一行）不计违规；
#   2) 纯注释行（以 *、/*、// 开头）不计；
#   3) 纯 extern 声明行不计（声明不是调用）；extern 后同带语句的，剥掉声明
#      部分保留其余内容继续检查。
filter_lines() {
    awk '
        {
            line = $0
            stripped = line
            sub(/^[[:space:]]+/, "", stripped)
            if (line ~ /config-ownership-allow:/) {
                if (stripped ~ /^(\*|\/\*|\/\/)/) allow_next = 1
                next
            }
            if (stripped ~ /^(\*|\/\*|\/\/)/ || stripped == "") next
            if (stripped ~ /^extern[[:space:]]/) {
                rest = line
                sub(/^[[:space:]]*extern[[:space:]]+[^;]*;/, " ", rest)
                sub(/\/\/.*$/, "", rest)
                if (rest ~ /^[[:space:]]*$/) next
                line = rest
            }
            if (allow_next) { allow_next = 0; next }
            print line
        }' "$1" 2>/dev/null
}

total=0
allows=0
declare -a rows=()
declare -a samples=()

scan_file() {
    local file="$1" rel
    [ -e "$file" ] || return 0
    rel="${file#"$SRC_ROOT"/}"
    local count
    count=$(filter_lines "$file" | grep -cE "$PATTERNS" || true)
    [ -z "$count" ] && count=0
    allows=$((allows + $(grep -c 'config-ownership-allow:' "$file" || true)))
    if [ "$count" -gt 0 ]; then
        rows+=("$(printf '%4d  %s' "$count" "${file#"$REPO_ROOT"/}")")
        while IFS= read -r hit; do
            [ -n "$hit" ] && samples+=("$hit")
        done < <(filter_lines "$file" | grep -nE "$PATTERNS" | head -4 |
                 sed "s|^|${rel}:|")
    fi
    total=$((total + count))
}

for dir in "${FRONT_DIRS[@]}"; do
    for file in "$SRC_ROOT/$dir"/*.c; do
        scan_file "$file"
    done
done
for rel in "${FRONT_FILES[@]}"; do
    scan_file "$SRC_ROOT/$rel"
done

echo "前端直写核心配置统计（应为 0，全部经 player_config_persist()）："
echo "  处数  文件"
if [ "${#rows[@]}" -gt 0 ]; then
    printf '%s\n' "${rows[@]}" | sort -rn
fi
echo "  ----  ----"
printf '%4d  合计\n' "$total"
printf '   %d  显式豁免（config-ownership-allow，宿主分支）\n' "$allows"

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
