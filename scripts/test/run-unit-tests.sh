#!/usr/bin/env bash
#
# 编译并运行 ter-music 的 C 单元测试（scripts/test/test_*.c）
#
# 用法：scripts/test/run-unit-tests.sh [测试名...]   （缺省运行全部）
#
# 依赖缺失语义（T-06 / t5 V-04）：
#   某些单测需要本机未安装的开发头/库（libavcodec、ncursesw、libxml2 等）。
#   这类**环境**编译失败输出 `SKIP`（不算测试失败），并在结尾汇总；
#   测试代码本身的编译错误仍然是 FAIL。默认退出码：
#     0 = 全部通过或仅有环境 SKIP；1 = 有真实失败。
#   设 TM_UNIT_STRICT=1 时，SKIP 也按失败处理（CI 完整环境用）。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT" || exit 1
OUT_DIR="${TMPDIR:-/tmp}/tm-unit-tests"
CC="${CC:-gcc}"

mkdir -p "$OUT_DIR"

names=("$@")
if [ "${#names[@]}" -eq 0 ]; then
    for f in "$SCRIPT_DIR"/test_*.c; do
        [ -e "$f" ] || continue
        names+=("$(basename "$f" .c)")
    done
fi

failures=0
skipped=0
passed=0
STRICT="${TM_UNIT_STRICT:-0}"
for name in "${names[@]}"; do
    src="$SCRIPT_DIR/$name.c"
    if [ ! -f "$src" ]; then
        echo "FAIL 找不到测试源 $src"
        failures=$((failures + 1))
        continue
    fi
    bin="$OUT_DIR/$name"

    # 可选：<name>.srcs 里每行一个额外源文件（相对仓库根），供需要链接模块的单测用
    extra=()
    if [ -f "$SCRIPT_DIR/$name.srcs" ]; then
        while IFS= read -r extra_src; do
            [ -n "$extra_src" ] && extra+=("$REPO_ROOT/$extra_src")
        done < "$SCRIPT_DIR/$name.srcs"
    fi

    # 可选：<name>.env 里给出额外的编译/链接参数（每行一个），
    # 例如需要 ffmpeg / sqlite 的单测
    flags=()
    if [ -f "$SCRIPT_DIR/$name.env" ]; then
        while IFS= read -r flag; do
            [ -n "$flag" ] && flags+=("$flag")
        done < "$SCRIPT_DIR/$name.env"
    fi

    if ! "$CC" -std=gnu99 -D_GNU_SOURCE -Wall -Wextra \
            -I "$REPO_ROOT/include/cn.org.huansoft.ter-music" \
            "$src" "$REPO_ROOT/src/cn.org.huansoft.ter-music/util/json.c" "${extra[@]}" \
            "${flags[@]}" \
            -o "$bin" 2>"$OUT_DIR/$name.build.log"; then
        # 只把“系统头/库缺失”归类为环境 SKIP；任何其它 error 仍算真实失败，
        # 避免用 SKIP 吞掉测试代码自身的编译错误。
        missing="$(grep -oE "fatal error: [^:]+: No such file or directory" \
            "$OUT_DIR/$name.build.log" | head -1 | sed 's/^fatal error: //; s/: No such file or directory$//')"
        missing_lib="$(grep -oE "cannot find -l[^ :]+" \
            "$OUT_DIR/$name.build.log" | head -1)"
        other_error="$(grep -E "error:" "$OUT_DIR/$name.build.log" \
            | grep -vE "fatal error: .*: No such file or directory" | head -1)"
        if [ -z "$other_error" ] && { [ -n "$missing" ] || [ -n "$missing_lib" ]; }; then
            reason="${missing:-$missing_lib}"
            echo "SKIP $name（环境缺少依赖：$reason；装好开发包后重跑）"
            skipped=$((skipped + 1))
            continue
        fi
        echo "FAIL $name 编译失败："
        sed 's/^/      /' "$OUT_DIR/$name.build.log"
        failures=$((failures + 1))
        continue
    fi
    if "$bin"; then
        echo "PASS $name"
        passed=$((passed + 1))
    else
        echo "FAIL $name"
        failures=$((failures + 1))
    fi
done

printf '%d passed, %d skipped, %d failed\n' "$passed" "$skipped" "$failures"
if [ "$failures" -gt 0 ]; then
    exit 1
fi
if [ "$STRICT" = "1" ] && [ "$skipped" -gt 0 ]; then
    echo "TM_UNIT_STRICT=1：环境 SKIP 也按失败处理" >&2
    exit 1
fi
exit 0
