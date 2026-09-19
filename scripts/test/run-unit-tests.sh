#!/usr/bin/env bash
#
# 编译并运行 ter-music 的 C 单元测试（scripts/test/test_*.c）
#
# 用法：scripts/test/run-unit-tests.sh [测试名...]   （缺省运行全部）

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
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
            -I "$REPO_ROOT/include/org.yxzl.ter-music" \
            "$src" "$REPO_ROOT/src/org.yxzl.ter-music/util/json.c" "${extra[@]}" \
            "${flags[@]}" \
            -o "$bin" 2>"$OUT_DIR/$name.build.log"; then
        echo "FAIL $name 编译失败："
        sed 's/^/      /' "$OUT_DIR/$name.build.log"
        failures=$((failures + 1))
        continue
    fi
    if "$bin"; then
        echo "PASS $name"
    else
        echo "FAIL $name"
        failures=$((failures + 1))
    fi
done

exit $((failures > 0 ? 1 : 0))
