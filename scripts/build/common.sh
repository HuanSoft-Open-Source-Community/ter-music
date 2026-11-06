#!/usr/bin/env bash
#
# 构建脚本共享工具：版本规范化、工作树暂存、release 复制、所有权修复。

# shellcheck shell=bash

TM_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

tm_log_info()  { echo "[INFO] $1"; }
tm_log_warn()  { echo "[WARN] $1" >&2; }
tm_log_error() { echo "[ERROR] $1" >&2; }

# 版本号只由参数指定：接受 v 前缀、三段或四段数字；三段补 .0 为四段。
tm_normalize_version_or_die() {
    local v="${1:-}"
    if [ -z "$v" ]; then
        tm_log_error "-v/--version is required"
        exit 1
    fi
    v="${v#v}"
    if [[ ! "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
        tm_log_error "invalid version '$1' (expected X.Y.Z or X.Y.Z.W)"
        exit 1
    fi
    if [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        v="${v}.0"
    fi
    printf '%s\n' "$v"
}

tm_is_container() {
    [ "${TER_MUSIC_IN_CONTAINER:-0}" = "1" ]
}

TM_SOURCE_EXCLUDES=(
    ".git"
    "build"
    "build-*"
    ".tmp"
    ".cache"
    ".linyaps_temp"
    ".appimagetool"
    ".screenshots"
    ".reasonix"
    ".trae"
    ".claude"
    ".lingma"
    ".workbuddy"
    ".local"
    ".vscode"
    "__pycache__"
    "*.log"
    "*.o"
    "*.a"
    "*.so"
    "*.deb"
    "*.rpm"
    "*.AppImage"
    "*.uab"
    "*.layer"
    "*.pkg.tar.zst"
)

# tm_stage_source <dest> [额外排除模式...]
# 从工作树复制（包含未提交修改），排除构建产物与缓存。
tm_stage_source() {
    local dest="${1:-}"
    shift || true
    if [ -z "$dest" ]; then
        tm_log_error "tm_stage_source: destination is required"
        return 1
    fi
    rm -rf -- "$dest"
    mkdir -p -- "$dest"

    if command -v rsync >/dev/null 2>&1; then
        local -a args=(-a)
        local pattern
        for pattern in "${TM_SOURCE_EXCLUDES[@]}"; do
            args+=(--exclude="$pattern")
        done
        for pattern in "$@"; do
            args+=(--exclude="$pattern")
        done
        rsync "${args[@]}" "${TM_REPO_ROOT}/" "${dest}/"
        return 0
    fi

    local -a tar_excludes=()
    local pattern
    for pattern in "${TM_SOURCE_EXCLUDES[@]}"; do
        tar_excludes+=(--exclude="./$pattern")
    done
    for pattern in "$@"; do
        tar_excludes+=(--exclude="./$pattern")
    done
    tar -C "$TM_REPO_ROOT" "${tar_excludes[@]}" -cf - . | tar -C "$dest" -xf -
}

tm_prepare_output() {
    mkdir -p -- "${TM_REPO_ROOT}/build/release"
}

tm_copy_release() {
    local src="${1:-}"
    if [ -z "$src" ] || [ ! -f "$src" ]; then
        return 0
    fi
    tm_prepare_output
    cp -f -- "$src" "${TM_REPO_ROOT}/build/release/"
    tm_log_info "release: $(basename "$src")"
}

# 宿主共享缓存：工具与源码 tarball 只下载一次，多镜像并行构建时复用。
TM_TOOLS_DIR="${TM_REPO_ROOT}/.cache/tools"
TM_LOCK_DIR="${TM_REPO_ROOT}/.cache/locks"
TM_APPIMAGETOOL_SOURCE="${TM_TOOLS_DIR}/appimagetool-x86_64.AppImage"

# FFmpeg 源码只下载一次，之后所有 Docker 镜像复用同一份 tarball。
# 注意：Dockerfile.debian10 / Dockerfile.rpm 里的 COPY 文件名要与此保持一致。
TM_FFMPEG_VERSION="8.1.2"
TM_FFMPEG_SOURCE="${TM_REPO_ROOT}/.cache/sources/ffmpeg-${TM_FFMPEG_VERSION}.tar.gz"
TM_FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${TM_FFMPEG_VERSION}.tar.gz"

# tm_download_to <url> <dest>
# 用 curl / wget / python3 下载到调用方给定的临时路径。
tm_download_to() {
    local url="${1:-}"
    local dest="${2:-}"
    if [ -z "$url" ] || [ -z "$dest" ]; then
        tm_log_error "tm_download_to: url and destination are required"
        return 1
    fi

    if command -v curl >/dev/null 2>&1; then
        if ! curl -fL --retry 5 --retry-delay 3 --connect-timeout 20 \
                -o "$dest" "$url"; then
            tm_log_error "failed to download with curl: ${url}"
            return 1
        fi
    elif command -v wget >/dev/null 2>&1; then
        if ! wget -O "$dest" "$url"; then
            tm_log_error "failed to download with wget: ${url}"
            return 1
        fi
    elif command -v python3 >/dev/null 2>&1; then
        if ! python3 - "$url" "$dest" <<'PY'
import sys
import urllib.request

urllib.request.urlretrieve(sys.argv[1], sys.argv[2])
PY
        then
            tm_log_error "failed to download with python3: ${url}"
            return 1
        fi
    else
        tm_log_error "no download tool (curl/wget/python3) for ${url}"
        return 1
    fi
}

# tm_lock_run <lockfile> <command...>
# 有 flock 时用独占锁串行化并发构建，否则直接执行。
tm_lock_run() {
    local lockfile="${1:-}"
    shift || true
    if [ -z "$lockfile" ] || [ "$#" -eq 0 ]; then
        tm_log_error "tm_lock_run: lockfile and command are required"
        return 1
    fi
    mkdir -p "$(dirname "$lockfile")"
    if command -v flock >/dev/null 2>&1; then
        (
            flock -x 9
            "$@"
        ) 9>"$lockfile"
    else
        "$@"
    fi
}

# 锁内执行，先二次检查缓存，避免等待者重复下载。
tm__ensure_ffmpeg_source_locked() {
    if [ -s "$TM_FFMPEG_SOURCE" ] && tar -tzf "$TM_FFMPEG_SOURCE" >/dev/null 2>&1; then
        tm_log_info "using cached FFmpeg source: ${TM_FFMPEG_SOURCE}"
        return 0
    fi

    mkdir -p "$(dirname "$TM_FFMPEG_SOURCE")"
    local tmp="${TM_FFMPEG_SOURCE}.part.$$"
    rm -f "$tmp"
    tm_log_info "downloading FFmpeg ${TM_FFMPEG_VERSION} source once: ${TM_FFMPEG_URL}"

    if ! tm_download_to "$TM_FFMPEG_URL" "$tmp"; then
        rm -f "$tmp"
        tm_log_error "failed to download FFmpeg source"
        return 1
    fi

    if ! tar -tzf "$tmp" >/dev/null 2>&1; then
        rm -f "$tmp"
        tm_log_error "downloaded FFmpeg source is not a valid tarball"
        return 1
    fi
    mv -f "$tmp" "$TM_FFMPEG_SOURCE"
    tm_log_info "FFmpeg source cached: ${TM_FFMPEG_SOURCE}"
}

tm_ensure_ffmpeg_source() {
    if [ -s "$TM_FFMPEG_SOURCE" ] && tar -tzf "$TM_FFMPEG_SOURCE" >/dev/null 2>&1; then
        tm_log_info "using cached FFmpeg source: ${TM_FFMPEG_SOURCE}"
        return 0
    fi

    tm_lock_run "${TM_LOCK_DIR}/ffmpeg-source.lock" tm__ensure_ffmpeg_source_locked
}

# appimagetool 同样只下载一次：优先最新 release，再退到固定标签与 continuous。
TM_APPIMAGETOOL_URLS=(
    "https://github.com/AppImage/appimagetool/releases/latest/download/appimagetool-x86_64.AppImage"
    "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
)

# 校验：文件 >1MB 且前 4 字节是 ELF 魔数（7f 45 4c 46）。
tm__appimagetool_is_valid() {
    local file="${1:-}"
    [ -f "$file" ] || return 1
    local size
    size="$(wc -c < "$file" 2>/dev/null | tr -d ' \n')"
    [ "${size:-0}" -gt 1048576 ] || return 1
    local magic
    magic="$(od -An -tx1 -N4 "$file" 2>/dev/null | tr -d ' \n')"
    [ "$magic" = "7f454c46" ]
}

tm__use_cached_appimagetool() {
    tm__appimagetool_is_valid "$TM_APPIMAGETOOL_SOURCE" || return 1
    chmod +x "$TM_APPIMAGETOOL_SOURCE"
    tm_log_info "using cached appimagetool: ${TM_APPIMAGETOOL_SOURCE}"
}

tm__ensure_appimagetool_locked() {
    if tm__use_cached_appimagetool; then
        return 0
    fi

    mkdir -p "$TM_TOOLS_DIR"
    local tmp="${TM_APPIMAGETOOL_SOURCE}.part.$$"
    local url
    local -a tried=()
    for url in "${TM_APPIMAGETOOL_URLS[@]}"; do
        tried+=("$url")
        rm -f "$tmp"
        tm_log_info "downloading appimagetool: ${url}"
        if ! tm_download_to "$url" "$tmp"; then
            rm -f "$tmp"
            continue
        fi
        if ! tm__appimagetool_is_valid "$tmp"; then
            tm_log_warn "downloaded appimagetool is not a valid ELF binary: ${url}"
            rm -f "$tmp"
            continue
        fi
        mv -f "$tmp" "$TM_APPIMAGETOOL_SOURCE"
        chmod +x "$TM_APPIMAGETOOL_SOURCE"
        tm_log_info "appimagetool cached from ${url}: ${TM_APPIMAGETOOL_SOURCE}"
        return 0
    done

    tm_log_error "failed to download appimagetool; tried:"
    for url in "${tried[@]}"; do
        tm_log_error "  ${url}"
    done
    return 1
}

tm_ensure_appimagetool() {
    if tm__use_cached_appimagetool; then
        return 0
    fi

    tm_lock_run "${TM_LOCK_DIR}/appimagetool.lock" tm__ensure_appimagetool_locked
}

# 新版 appimagetool 会调用 mksquashfs -mkfs-time（squashfs-tools >= 4.4），
# Debian 10 自带的 4.3 不支持，因此镜像里从源码装一份最新的 release。
TM_SQUASHFS_TOOLS_VERSION="4.7.5"
TM_SQUASHFS_TOOLS_SOURCE="${TM_REPO_ROOT}/.cache/sources/squashfs-tools-${TM_SQUASHFS_TOOLS_VERSION}.tar.gz"
TM_SQUASHFS_TOOLS_URLS=(
    "https://codeload.github.com/plougher/squashfs-tools/tar.gz/refs/tags/${TM_SQUASHFS_TOOLS_VERSION}"
    "https://github.com/plougher/squashfs-tools/releases/download/${TM_SQUASHFS_TOOLS_VERSION}/squashfs-tools-${TM_SQUASHFS_TOOLS_VERSION}.tar.gz"
)

# appimagetool 需要 type2 runtime 才能生成 AppImage；从 latest release 取，
# 宿主缓存后 COPY 进镜像，避免容器内联网下载失败。
TM_APPIMAGE_RUNTIME_SOURCE="${TM_TOOLS_DIR}/runtime-x86_64"
TM_APPIMAGE_RUNTIME_URLS=(
    "https://github.com/AppImage/type2-runtime/releases/latest/download/runtime-x86_64"
    "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"
)

tm__squashfs_tools_is_valid() {
    local file="${1:-}"
    [ -s "$file" ] || return 1
    tar -tzf "$file" >/dev/null 2>&1
}

tm__appimage_runtime_is_valid() {
    local file="${1:-}"
    [ -f "$file" ] || return 1
    local size
    size="$(wc -c < "$file" 2>/dev/null | tr -d ' \n')"
    [ "${size:-0}" -gt 102400 ] || return 1
    local magic
    magic="$(od -An -tx1 -N4 "$file" 2>/dev/null | tr -d ' \n')"
    [ "$magic" = "7f454c46" ]
}

tm__ensure_squashfs_tools_locked() {
    if tm__squashfs_tools_is_valid "$TM_SQUASHFS_TOOLS_SOURCE"; then
        tm_log_info "using cached squashfs-tools source: ${TM_SQUASHFS_TOOLS_SOURCE}"
        return 0
    fi

    mkdir -p "$(dirname "$TM_SQUASHFS_TOOLS_SOURCE")"
    local tmp="${TM_SQUASHFS_TOOLS_SOURCE}.part.$$"
    local url
    for url in "${TM_SQUASHFS_TOOLS_URLS[@]}"; do
        rm -f "$tmp"
        tm_log_info "downloading squashfs-tools ${TM_SQUASHFS_TOOLS_VERSION}: ${url}"
        if ! tm_download_to "$url" "$tmp"; then
            rm -f "$tmp"
            continue
        fi
        if ! tm__squashfs_tools_is_valid "$tmp"; then
            tm_log_warn "downloaded squashfs-tools source is not a valid tarball: ${url}"
            rm -f "$tmp"
            continue
        fi
        mv -f "$tmp" "$TM_SQUASHFS_TOOLS_SOURCE"
        tm_log_info "squashfs-tools source cached: ${TM_SQUASHFS_TOOLS_SOURCE}"
        return 0
    done

    tm_log_error "failed to download squashfs-tools source"
    return 1
}

tm_ensure_squashfs_tools() {
    if tm__squashfs_tools_is_valid "$TM_SQUASHFS_TOOLS_SOURCE"; then
        tm_log_info "using cached squashfs-tools source: ${TM_SQUASHFS_TOOLS_SOURCE}"
        return 0
    fi

    tm_lock_run "${TM_LOCK_DIR}/squashfs-tools.lock" tm__ensure_squashfs_tools_locked
}

tm__ensure_appimage_runtime_locked() {
    if tm__appimage_runtime_is_valid "$TM_APPIMAGE_RUNTIME_SOURCE"; then
        tm_log_info "using cached AppImage runtime: ${TM_APPIMAGE_RUNTIME_SOURCE}"
        return 0
    fi

    mkdir -p "$TM_TOOLS_DIR"
    local tmp="${TM_APPIMAGE_RUNTIME_SOURCE}.part.$$"
    local url
    for url in "${TM_APPIMAGE_RUNTIME_URLS[@]}"; do
        rm -f "$tmp"
        tm_log_info "downloading AppImage runtime: ${url}"
        if ! tm_download_to "$url" "$tmp"; then
            rm -f "$tmp"
            continue
        fi
        if ! tm__appimage_runtime_is_valid "$tmp"; then
            tm_log_warn "downloaded AppImage runtime is not a valid ELF binary: ${url}"
            rm -f "$tmp"
            continue
        fi
        mv -f "$tmp" "$TM_APPIMAGE_RUNTIME_SOURCE"
        chmod +x "$TM_APPIMAGE_RUNTIME_SOURCE"
        tm_log_info "AppImage runtime cached: ${TM_APPIMAGE_RUNTIME_SOURCE}"
        return 0
    done

    tm_log_error "failed to download AppImage runtime"
    return 1
}

tm_ensure_appimage_runtime() {
    if tm__appimage_runtime_is_valid "$TM_APPIMAGE_RUNTIME_SOURCE"; then
        tm_log_info "using cached AppImage runtime: ${TM_APPIMAGE_RUNTIME_SOURCE}"
        return 0
    fi

    tm_lock_run "${TM_LOCK_DIR}/appimage-runtime.lock" tm__ensure_appimage_runtime_locked
}

# 容器构建后用宿主 UID/GID 修复 build/ 与 .tmp/；非 root 时只保证可写。
tm_fix_ownership() {
    local dir
    for dir in "${TM_REPO_ROOT}/build" "${TM_REPO_ROOT}/.tmp"; do
        [ -d "$dir" ] || continue
        if [ -n "${HOST_UID:-}" ] && [ -n "${HOST_GID:-}" ] &&
           [ "$(id -u)" = "0" ] && [ "$HOST_UID" != "0" ]; then
            chown -R "${HOST_UID}:${HOST_GID}" "$dir" 2>/dev/null || \
                chmod -R u+rwX "$dir" 2>/dev/null || true
        else
            chmod -R u+rwX "$dir" 2>/dev/null || true
        fi
    done
}
