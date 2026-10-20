#!/usr/bin/env bash
#
# PipeWire ABI 漂移守卫
#
# 仓库为了 dlopen 而不依赖 PipeWire 头文件，在
# include/org.yxzl.ter-music/audio/backend/pipewire.h 里手写了一份 SPA/PipeWire
# ABI 子集。本脚本用两份同源的 C 探针分别对着“系统真实头文件”和“仓库手写头
# 文件”编译运行，逐项打印 key=value，再 diff 两份输出：常量、结构体尺寸、
# 字段偏移的任何漂移都会暴露出来。
#
# 覆盖：
#   - SPA pod 类型 / SPA object 类型 / SPA param id / format 键 / audio format；
#   - PW stream flags / direction / stream state；
#   - spa_hook、spa_data、spa_buffer、pw_buffer 的尺寸与字段偏移；
#   - pw_stream_events 回调偏移（与 spa_hook 的私有布局是两回事）。
#
# 用法：scripts/test/pipewire-abi-check.sh
#
# 退出码：0 一致，或环境缺少 libpipewire-0.3 头文件（SKIP，不算失败）；
#         1 ABI 漂移 / 探针编译失败（仓库手写 ABI 不完整）；
#         2 环境错误（找不到仓库头文件、临时目录创建失败等）。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPO_HEADER_DIR="$REPO_ROOT/include/org.yxzl.ter-music"
REPO_HEADER="$REPO_HEADER_DIR/audio/backend/pipewire.h"

PASS=0
FAIL=0
SKIP=0

ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
skip() { SKIP=$((SKIP + 1)); printf '  SKIP %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

# 无头文件时统一 SKIP：CI 的 shellcheck job 只做语法检查，不装 PipeWire 开发包。
skip_unavailable() {
    printf 'SKIP: libpipewire-0.3 headers not available\n'
    exit 0
}

# ── 环境探测 ──────────────────────────────────────────────────────
command -v pkg-config >/dev/null 2>&1 || skip_unavailable
pkg-config --exists libpipewire-0.3 || skip_unavailable

PC_CFLAGS="$(pkg-config --cflags libpipewire-0.3 2>/dev/null)"
PC_LIBS="$(pkg-config --libs libpipewire-0.3 2>/dev/null)"
read -r -a PC_CFLAGS_ARR <<< "$PC_CFLAGS"
read -r -a PC_LIBS_ARR <<< "$PC_LIBS"

have_header() {
    local hdr="$1" inc
    for inc in "${PC_CFLAGS_ARR[@]}"; do
        case "$inc" in
            -I*) [ -f "${inc#-I}/$hdr" ] && return 0 ;;
        esac
    done
    return 1
}

for hdr in spa/utils/type.h spa/utils/hook.h spa/buffer/buffer.h \
           spa/param/format.h spa/param/audio/raw.h spa/param/props.h \
           pipewire/stream.h; do
    have_header "$hdr" || skip_unavailable
done

if ! command -v cc >/dev/null 2>&1; then
    printf 'SKIP: C compiler (cc) not available\n'
    exit 0
fi

if [ ! -f "$REPO_HEADER" ]; then
    echo "错误：找不到仓库手写 ABI 头文件 $REPO_HEADER" >&2
    exit 2
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-pw-abi-XXXXXX")" || {
    echo "错误：无法创建临时目录" >&2
    exit 2
}
trap 'rm -rf "$WORK_DIR"' EXIT

# ── 探针源码 ──────────────────────────────────────────────────────
# 输出体只有一份（probe_body.inc），两个 .c 只在 include 前导上不同，保证两边
# 打印的键与顺序完全一致。
cat > "$WORK_DIR/probe_body.inc" <<'EOF'
#include <stddef.h>
#include <stdio.h>

#define KEY(name, value) printf("%s=%lld\n", (name), (long long)(value))
#define SZ(type)         KEY("sizeof(" #type ")", sizeof(type))
#define OFF(type, field) KEY("offsetof(" #type "," #field ")", offsetof(type, field))

int main(void)
{
    /* ---- SPA pod 类型 ---- */
    KEY("SPA_TYPE_Array", SPA_TYPE_Array);
    KEY("SPA_TYPE_Struct", SPA_TYPE_Struct);
    KEY("SPA_TYPE_Object", SPA_TYPE_Object);
    KEY("SPA_TYPE_Choice", SPA_TYPE_Choice);

    /* ---- SPA object 类型 ---- */
    KEY("SPA_TYPE_OBJECT_Format", SPA_TYPE_OBJECT_Format);
    KEY("SPA_TYPE_OBJECT_ParamBuffers", SPA_TYPE_OBJECT_ParamBuffers);

    /* ---- SPA param id ---- */
    KEY("SPA_PARAM_Format", SPA_PARAM_Format);
    KEY("SPA_PARAM_Buffers", SPA_PARAM_Buffers);
    KEY("SPA_PARAM_Props", SPA_PARAM_Props);

    /* ---- format 键 ---- */
    KEY("SPA_FORMAT_mediaType", SPA_FORMAT_mediaType);
    KEY("SPA_FORMAT_mediaSubtype", SPA_FORMAT_mediaSubtype);
    KEY("SPA_FORMAT_AUDIO_format", SPA_FORMAT_AUDIO_format);
    KEY("SPA_FORMAT_AUDIO_rate", SPA_FORMAT_AUDIO_rate);
    KEY("SPA_FORMAT_AUDIO_channels", SPA_FORMAT_AUDIO_channels);

    /* ---- media type / subtype / audio format ---- */
    KEY("SPA_MEDIA_TYPE_audio", SPA_MEDIA_TYPE_audio);
    KEY("SPA_MEDIA_SUBTYPE_raw", SPA_MEDIA_SUBTYPE_raw);
    KEY("SPA_AUDIO_FORMAT_S32_LE", SPA_AUDIO_FORMAT_S32_LE);

    /* ---- SPA prop ---- */
    KEY("SPA_PROP_volume", SPA_PROP_volume);

    /* ---- PipeWire stream flags / direction / state ---- */
    KEY("PW_STREAM_FLAG_AUTOCONNECT", PW_STREAM_FLAG_AUTOCONNECT);
    KEY("PW_STREAM_FLAG_MAP_BUFFERS", PW_STREAM_FLAG_MAP_BUFFERS);
    KEY("PW_STREAM_FLAG_RT_PROCESS", PW_STREAM_FLAG_RT_PROCESS);
    KEY("PW_STREAM_FLAG_DONT_RECONNECT", PW_STREAM_FLAG_DONT_RECONNECT);
    KEY("PW_STREAM_FLAG_ASYNC", PW_STREAM_FLAG_ASYNC);
    KEY("PW_DIRECTION_OUTPUT", PW_DIRECTION_OUTPUT);
    KEY("PW_STREAM_STATE_ERROR", PW_STREAM_STATE_ERROR);
    KEY("PW_STREAM_STATE_UNCONNECTED", PW_STREAM_STATE_UNCONNECTED);
    KEY("PW_STREAM_STATE_CONNECTING", PW_STREAM_STATE_CONNECTING);
    KEY("PW_STREAM_STATE_PAUSED", PW_STREAM_STATE_PAUSED);
    KEY("PW_STREAM_STATE_STREAMING", PW_STREAM_STATE_STREAMING);

    /* ---- spa_hook：link/cb/removed/priv 的布局 ---- */
    SZ(struct spa_hook);
    OFF(struct spa_hook, cb);
    OFF(struct spa_hook, removed);
    OFF(struct spa_hook, priv);

    /* ---- spa_data：fd 宽度影响后续所有偏移 ---- */
    SZ(struct spa_data);
    OFF(struct spa_data, fd);
    OFF(struct spa_data, mapoffset);
    OFF(struct spa_data, maxsize);
    OFF(struct spa_data, data);
    OFF(struct spa_data, chunk);

    /* ---- spa_buffer：metas/datas 是否为真实指针字段 ---- */
    SZ(struct spa_buffer);
    OFF(struct spa_buffer, metas);
    OFF(struct spa_buffer, datas);

    /* ---- pw_buffer ---- */
    SZ(struct pw_buffer);

    /* ---- pw_stream_events 回调偏移 ---- */
    OFF(struct pw_stream_events, state_changed);
    OFF(struct pw_stream_events, param_changed);
    OFF(struct pw_stream_events, process);

    return 0;
}
EOF

cat > "$WORK_DIR/probe_real.c" <<'EOF'
#include <spa/utils/type.h>
#include <spa/utils/hook.h>
#include <spa/buffer/buffer.h>
#include <spa/param/format.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <pipewire/stream.h>

#include "probe_body.inc"
EOF

cat > "$WORK_DIR/probe_repo.c" <<'EOF'
#include "audio/backend/pipewire.h"

#include "probe_body.inc"
EOF

# ── 编译 ──────────────────────────────────────────────────────────
compile_probe() {   # $1 = 名字, 其余 = 额外编译参数
    local name="$1"; shift
    local log="$WORK_DIR/$name.build.log"
    if cc -std=gnu11 -D_GNU_SOURCE -O0 "$@" "$WORK_DIR/$name.c" \
          -o "$WORK_DIR/$name" > "$log" 2>&1; then
        return 0
    fi
    bad "$name.c 编译失败"
    sed 's/^/       /' "$log"
    return 1
}

info "编译系统头文件探针"
if ! compile_probe probe_real "${PC_CFLAGS_ARR[@]}" "${PC_LIBS_ARR[@]}"; then
    info "结果"
    printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
    exit 1
fi
ok "probe_real.c 编译通过（对着 libpipewire-0.3 真实头文件）"

info "编译仓库手写 ABI 探针"
if ! compile_probe probe_repo -I "$REPO_HEADER_DIR"; then
    bad "仓库手写 ABI 不完整：头文件缺少探针引用的常量/字段（见上方编译器错误）"
    info "结果"
    printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
    exit 1
fi
ok "probe_repo.c 编译通过（对着 include/org.yxzl.ter-music）"

# ── 运行 ──────────────────────────────────────────────────────────
run_probe() {   # $1 = 名字
    local name="$1"
    if "$WORK_DIR/$name" > "$WORK_DIR/$name.txt" 2> "$WORK_DIR/$name.run.log"; then
        return 0
    fi
    bad "$name 运行失败"
    sed 's/^/       /' "$WORK_DIR/$name.run.log"
    return 1
}

info "采集两边 ABI 快照"
run_real=0
run_repo=0
run_probe probe_real || run_real=1
run_probe probe_repo || run_repo=1
if [ "$run_real" -ne 0 ] || [ "$run_repo" -ne 0 ]; then
    info "结果"
    printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
    exit 1
fi
keys="$(wc -l < "$WORK_DIR/probe_real.txt" | tr -d ' ')"
ok "两份探针各输出 $keys 项 ABI 快照"

# ── 比较 ──────────────────────────────────────────────────────────
info "比较仓库手写 ABI 与系统头文件"
if diff -u "$WORK_DIR/probe_real.txt" "$WORK_DIR/probe_repo.txt" \
        > "$WORK_DIR/abi.diff"; then
    ok "仓库手写 ABI 与系统头文件一致（$keys 项）"
else
    drift="$(awk '/^[+-][^+-]/{n++} END{print n+0}' "$WORK_DIR/abi.diff")"
    bad "检测到 ABI 漂移（$drift 行差异，- 系统真实 / + 仓库手写）"
    sed 's/^/       /' "$WORK_DIR/abi.diff"
fi

info "结果"
printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
