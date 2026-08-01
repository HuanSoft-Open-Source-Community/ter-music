#!/bin/bash
#
# Docker build wrapper script
# Runs build scripts inside a Docker container with proper build environment
#

set -e

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
SCRIPT_DIR="$(pwd)"
IMAGE_NAME="ter-music-deb-static"
DOCKERFILE="scripts/docker/Dockerfile.deb-static"

# Redirect Docker client config directory to a writable location
# (default ~/.docker/ may be on a read-only filesystem, causing Buildx
#  activity tracking and other Docker writes to fail)
DOCKER_CONFIG_DIR="$SCRIPT_DIR/.cache/docker-buildx"
mkdir -p "$DOCKER_CONFIG_DIR"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1" >&2
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

show_help() {
    cat << EOF
用法: $0 [选项] -- [构建脚本参数]

在 Docker 容器中进行构建

选项:
    -h, --help          显示此帮助信息
    -b, --build-image   删除并重新构建 scripts/docker/ 下所有四个构建镜像
    -s, --script SCRIPT 指定构建脚本 (默认: build-deb.sh)
    -i, --interactive   进入容器的交互式 shell
    -f, --dockerfile DOCKERFILE  指定 Dockerfile 路径 (默认: scripts/docker/Dockerfile.deb-static)
    -n, --image-name NAME        指定 Docker 镜像名 (默认: ter-music-deb-static)
    -p, --privileged    以特权模式运行容器（Linyaps 需要）
    --build-arg KEY=VALUE        传递构建参数给 docker build
    --no-cache          构建镜像时不使用缓存

示例:
    $0                          # 使用默认设置构建 DEB 包
    $0 -s build-rpm.sh         # 使用 RPM 构建脚本
    $0 -s build-rpm.sh -f scripts/docker/Dockerfile.rpm --build-arg EL_VERSION=9
    $0 -i                       # 进入交互式 shell
    $0 -b                       # 重建所有四个构建镜像
    $0 -- --keep-temp           # 传递参数给构建脚本

EOF
}

# Parse arguments
BUILD_IMAGE=false
SCRIPT="build-deb.sh"
INTERACTIVE=false
NO_CACHE=""
PRIVILEGED=false
BUILD_ARGS=()
BUILD_SCRIPT_ARGS=()

# 四个构建镜像的 (Dockerfile:镜像名) 对
BUILD_TARGETS=(
    "scripts/docker/Dockerfile.deb-static:ter-music-deb-static"
    "scripts/docker/Dockerfile.rpm:ter-music-rpm"
    "scripts/docker/Dockerfile.rpm-static:ter-music-rpm-static"
    "scripts/docker/Dockerfile.uab:ter-music-uab-builder"
)

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -b|--build-image)
            BUILD_IMAGE=true
            shift
            ;;
        -s|--script)
            SCRIPT="$2"
            shift 2
            ;;
        -i|--interactive)
            INTERACTIVE=true
            shift
            ;;
        -p|--privileged)
            PRIVILEGED=true
            shift
            ;;
        --no-cache)
            NO_CACHE="--no-cache"
            shift
            ;;
        -f|--dockerfile)
            DOCKERFILE="$2"
            shift 2
            ;;
        -n|--image-name)
            IMAGE_NAME="$2"
            shift 2
            ;;
        --build-arg)
            if [ -z "$2" ]; then
                log_error "--build-arg requires a value (format: KEY=VALUE)"
                exit 1
            fi
            BUILD_ARGS+=("--build-arg" "$2")
            shift 2
            ;;
        --)
            shift
            BUILD_SCRIPT_ARGS=("$@")
            break
            ;;
        *)
            BUILD_SCRIPT_ARGS+=("$1")
            shift
            ;;
    esac
done

# Check if Docker is installed
if ! command -v docker &> /dev/null; then
    log_error "Docker 未安装，请先安装 Docker"
    log_info "安装命令: sudo apt install docker.io"
    exit 1
fi

# Build Docker image(s)
if [ "$BUILD_IMAGE" = true ]; then
    # -b 模式：删除并重建所有四个构建镜像
    # 注意：-f/-n 参数在 -b 模式下被忽略（始终构建全部四个镜像）
    if [ "$DOCKERFILE" != "scripts/docker/Dockerfile.deb-static" ] || [ "$IMAGE_NAME" != "ter-music-deb-static" ]; then
        log_warn "-b 模式下 -f/-n 参数不生效，将构建全部四个镜像"
    fi
    log_info "开始重建所有 Docker 构建镜像..."
    for target in "${BUILD_TARGETS[@]}"; do
        df="${target%%:*}"
        name="${target#*:}"
        if [ ! -f "$df" ]; then
            log_error "Dockerfile 不存在: $df"
            exit 1
        fi
        # 删除已存在的镜像（仅删除构建镜像本身，不删除 pull 的基础镜像）
        if docker image inspect "$name" &>/dev/null; then
            log_info "删除旧镜像: $name"
            docker rmi -f "$name" >/dev/null 2>&1 || true
        fi
        log_info "构建镜像: $name (Dockerfile: $df)"
        if ! DOCKER_CONFIG="$DOCKER_CONFIG_DIR" docker build "${BUILD_ARGS[@]}" --no-cache -f "$df" -t "$name" "$SCRIPT_DIR"; then
            log_error "镜像构建失败: $name"
            exit 1
        fi
        log_info "镜像构建完成: $name"
    done
    log_info "所有 Docker 构建镜像已重建完成"
    exit 0
fi

# 非 -b 模式：按需构建单个镜像
if ! docker image inspect "$IMAGE_NAME" &> /dev/null; then
    log_info "构建 Docker 镜像: $IMAGE_NAME (Dockerfile: $DOCKERFILE)"
    if ! DOCKER_CONFIG="$DOCKER_CONFIG_DIR" docker build "${BUILD_ARGS[@]}" $NO_CACHE -f "$DOCKERFILE" -t "$IMAGE_NAME" "$SCRIPT_DIR"; then
        log_error "Docker 镜像构建失败"
        exit 1
    fi
    log_info "Docker 镜像构建完成"
else
    log_info "使用已存在的 Docker 镜像: $IMAGE_NAME"
fi

# Run container
if [ "$INTERACTIVE" = true ]; then
    log_info "进入交互式容器..."
    iopts=(-v "$SCRIPT_DIR:/workspace" --workdir /workspace)
    if [ "$PRIVILEGED" = true ]; then
        iopts+=(--privileged)
        iopts+=(--security-opt seccomp=unconfined)
        iopts+=(--security-opt apparmor=unconfined)
    else
        iopts+=(--user "$(id -u):$(id -g)")
    fi
    iopts+=(-e "HOST_UID=$(id -u)" -e "HOST_GID=$(id -g)" -e SKIP_GIT_ARCHIVE=1)
    docker run --rm -it "${iopts[@]}" \
        "$IMAGE_NAME" \
        /bin/bash
else
    log_info "在容器中运行构建脚本: $SCRIPT"
    log_info "构建参数: ${BUILD_SCRIPT_ARGS[*]}"

    run_opts=()
    run_opts+=(-v "$SCRIPT_DIR:/workspace")
    run_opts+=(--workdir /workspace)

    if [ "$PRIVILEGED" = true ]; then
        run_opts+=(--privileged)
        run_opts+=(--security-opt seccomp=unconfined)
        run_opts+=(--security-opt apparmor=unconfined)
        # Persist linglong cache to avoid re-downloading base layer each run
        mkdir -p "$SCRIPT_DIR/.cache/linglong"
        run_opts+=(-v "$SCRIPT_DIR/.cache/linglong:/var/lib/linglong")
        # Mount /tmp as tmpfs so that ll-builder can do overlayfs mounts there
        # (Docker root is overlayfs; nested overlay is rejected by the kernel)
        run_opts+=(--tmpfs /tmp:exec,size=4G)
    else
        run_opts+=(--user "$(id -u):$(id -g)")
    fi

    run_opts+=(-e "HOST_UID=$(id -u)")
    run_opts+=(-e "HOST_GID=$(id -g)")
    run_opts+=(-e SKIP_GIT_ARCHIVE=1)
    # Increase linyaps remote repo timeout (default 5s is too short for deepin mirrors)
    run_opts+=(-e LINGLONG_CONNECT_TIMEOUT=120)

    build_rc=0
    if docker run --rm "${run_opts[@]}" \
        "$IMAGE_NAME" \
        "./scripts/build/$SCRIPT" "${BUILD_SCRIPT_ARGS[@]}"; then
        log_info "构建完成！输出目录: ${SCRIPT_DIR}/build/"
    else
        log_error "构建失败"
        build_rc=1
    fi

    # 兜底：容器退出后统一修复 build/ 产物和缓存目录的所有权
    # 特权容器 (--privileged) 以 root 运行，产物可能属于 root
    if [ -d "${SCRIPT_DIR}/build" ]; then
        chown -R "$(id -u):$(id -g)" "${SCRIPT_DIR}/build" 2>/dev/null || \
            chmod -R u+rwX,go+rX "${SCRIPT_DIR}/build" 2>/dev/null || true
    fi
    if [ -d "${SCRIPT_DIR}/.cache/linglong" ]; then
        chown -R "$(id -u):$(id -g)" "${SCRIPT_DIR}/.cache/linglong" 2>/dev/null || \
            chmod -R u+rwX,go+rX "${SCRIPT_DIR}/.cache/linglong" 2>/dev/null || true
    fi

    exit $build_rc
fi
