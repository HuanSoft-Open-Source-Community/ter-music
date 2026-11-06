#!/usr/bin/env bash
#
# Docker 构建包装器：按需构建镜像，然后在容器内运行指定构建脚本。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=../build/common.sh
. "${SCRIPT_DIR}/scripts/build/common.sh"

DOCKER_CONFIG_DIR="${TM_REPO_ROOT}/.cache/docker-buildx"
mkdir -p "$DOCKER_CONFIG_DIR"

HOST_UID="$(id -u)"
HOST_GID="$(id -g)"
export HOST_UID HOST_GID

DOCKERFILE=""
IMAGE_NAME=""
SCRIPT=""
REBUILD=false
ROOT=false
INTERACTIVE=false
NO_CACHE=()
BUILD_ARGS=()
SCRIPT_ARGS=()

show_help() {
    cat <<'EOF'
用法: docker-build.sh -f DOCKERFILE -n IMAGE -s SCRIPT [选项] -- [构建脚本参数]

选项:
  -f, --dockerfile PATH    Dockerfile 路径（相对仓库根）
  -n, --image-name NAME    镜像名
  -s, --script NAME        scripts/build/ 下的脚本名
      --build-arg K=V      传给 docker build，可重复
      --rebuild            删除并重建镜像
      --no-cache           docker build 不使用缓存
      --root               以 root 运行容器（AUR 安装验收需要）
  -i, --interactive        进入容器交互式 shell
  -h, --help               显示帮助

示例:
  docker-build.sh -f scripts/docker/Dockerfile.debian10 -n ter-music-debian10 \
      -s build-deb.sh -- -v 26.4.1.0
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        -f|--dockerfile) DOCKERFILE="${2:-}"; shift 2 ;;
        -n|--image-name) IMAGE_NAME="${2:-}"; shift 2 ;;
        -s|--script) SCRIPT="${2:-}"; shift 2 ;;
        --build-arg) BUILD_ARGS+=(--build-arg "${2:-}"); shift 2 ;;
        --rebuild) REBUILD=true; shift ;;
        --no-cache) NO_CACHE=(--no-cache); shift ;;
        --root) ROOT=true; shift ;;
        -i|--interactive) INTERACTIVE=true; shift ;;
        --)
            shift
            SCRIPT_ARGS=("$@")
            break
            ;;
        *)
            tm_log_error "unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

if [ -z "$DOCKERFILE" ] || [ -z "$IMAGE_NAME" ]; then
    tm_log_error "-f/--dockerfile and -n/--image-name are required"
    exit 1
fi
if [ "$INTERACTIVE" = false ] && [ -z "$SCRIPT" ]; then
    tm_log_error "-s/--script is required"
    exit 1
fi
if ! command -v docker >/dev/null 2>&1; then
    tm_log_error "docker is not installed"
    exit 1
fi

DOCKER_PREFIX=(env "DOCKER_CONFIG=${DOCKER_CONFIG_DIR}" docker)
context="$(cd "$(dirname "$DOCKERFILE")" && pwd)"

# 需要 FFmpeg 源码、appimagetool、squashfs-tools 或 AppImage runtime 的镜像
# 从宿主缓存取同一份文件，避免每个镜像重复下载，并统一使用同一个构建上下文。
NEED_FFMPEG=false
NEED_APPIMAGETOOL=false
NEED_SQUASHFS_TOOLS=false
NEED_APPIMAGE_RUNTIME=false
if grep -q '^COPY ffmpeg-' "$DOCKERFILE"; then
    NEED_FFMPEG=true
fi
if grep -q '^COPY appimagetool-' "$DOCKERFILE"; then
    NEED_APPIMAGETOOL=true
fi
if grep -q '^COPY squashfs-tools-' "$DOCKERFILE"; then
    NEED_SQUASHFS_TOOLS=true
fi
if grep -q '^COPY runtime-' "$DOCKERFILE"; then
    NEED_APPIMAGE_RUNTIME=true
fi

CONTEXT_BUILD_ARGS=()
if [ "$NEED_FFMPEG" = true ] || [ "$NEED_APPIMAGETOOL" = true ] ||
   [ "$NEED_SQUASHFS_TOOLS" = true ] || [ "$NEED_APPIMAGE_RUNTIME" = true ]; then
    context="${TM_REPO_ROOT}/.cache/docker-context"
    mkdir -p "$context"
fi
if [ "$NEED_FFMPEG" = true ]; then
    tm_ensure_ffmpeg_source || exit 1
    cp -f "$TM_FFMPEG_SOURCE" "$context/ffmpeg-${TM_FFMPEG_VERSION}.tar.gz"
    CONTEXT_BUILD_ARGS+=(--build-arg "FFMPEG_VERSION=${TM_FFMPEG_VERSION}")
fi
if [ "$NEED_APPIMAGETOOL" = true ]; then
    tm_ensure_appimagetool || exit 1
    cp -f "$TM_APPIMAGETOOL_SOURCE" "$context/appimagetool-x86_64.AppImage"
fi
if [ "$NEED_SQUASHFS_TOOLS" = true ]; then
    tm_ensure_squashfs_tools || exit 1
    cp -f "$TM_SQUASHFS_TOOLS_SOURCE" "$context/squashfs-tools-${TM_SQUASHFS_TOOLS_VERSION}.tar.gz"
    CONTEXT_BUILD_ARGS+=(--build-arg "SQUASHFS_TOOLS_VERSION=${TM_SQUASHFS_TOOLS_VERSION}")
fi
if [ "$NEED_APPIMAGE_RUNTIME" = true ]; then
    tm_ensure_appimage_runtime || exit 1
    cp -f "$TM_APPIMAGE_RUNTIME_SOURCE" "$context/runtime-x86_64"
fi

if [ "$REBUILD" = true ] || ! "${DOCKER_PREFIX[@]}" image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    if [ "$REBUILD" = true ]; then
        "${DOCKER_PREFIX[@]}" rmi -f "$IMAGE_NAME" >/dev/null 2>&1 || true
    fi
    tm_log_info "building image ${IMAGE_NAME} (${DOCKERFILE})"
    if ! "${DOCKER_PREFIX[@]}" build "${NO_CACHE[@]}" \
            "${CONTEXT_BUILD_ARGS[@]}" "${BUILD_ARGS[@]}" \
            -f "$DOCKERFILE" -t "$IMAGE_NAME" "$context"; then
        tm_log_error "image build failed: ${IMAGE_NAME}"
        exit 1
    fi
fi

run_opts=(--rm -v "${TM_REPO_ROOT}:/workspace" --workdir /workspace)
if [ "$ROOT" = true ]; then
    run_opts+=(--user 0:0)
else
    run_opts+=(--user "${HOST_UID}:${HOST_GID}")
fi
run_opts+=(-e "HOST_UID=${HOST_UID}" -e "HOST_GID=${HOST_GID}")
run_opts+=(-e "TER_MUSIC_IN_CONTAINER=1")

if [ "$INTERACTIVE" = true ]; then
    run_opts+=(-it)
    cmd=(/bin/bash)
else
    cmd=("./scripts/build/${SCRIPT}" "${SCRIPT_ARGS[@]}")
fi

set +e
"${DOCKER_PREFIX[@]}" run "${run_opts[@]}" "$IMAGE_NAME" "${cmd[@]}"
rc=$?
set -e

tm_fix_ownership
exit "$rc"
