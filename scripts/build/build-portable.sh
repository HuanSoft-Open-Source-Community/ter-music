#!/bin/bash

set -e

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
SCRIPT_DIR="$(pwd)"
PROJECT_NAME="ter-music"
OUTPUT_DIR="${SCRIPT_DIR}/build/portable"
TEMP_DIR="${SCRIPT_DIR}/.portable_temp"

log_info() {
    echo "[INFO] $1"
}

log_error() {
    echo "[ERROR] $1" >&2
}

log_clean() {
    echo "[CLEAN] $1"
}

log_warn() {
    echo "[WARN] $1" >&2
}

# 复制构建结果到 build/release 目录
copy_to_release() {
    local source_file="$1"
    local release_dir="${SCRIPT_DIR}/build/release"
    
    if [ -f "$source_file" ]; then
        mkdir -p "$release_dir"
        if cp "$source_file" "$release_dir/"; then
            log_info "构建结果已复制到: ${release_dir}/$(basename "$source_file")"
        fi
    fi
}

show_help() {
    cat << EOF
用法: $0 [选项]

直接从源码构建 ter-music 可移植压缩包（也可从 RPM 转换）

选项:
    -h, --help          显示此帮助信息
    -v, --version VERSION  指定版本号（默认：自动检测）
    -a, --arch ARCH     指定目标架构（默认：自动检测）
    -r, --rpm FILE      从指定的RPM包文件转换（可选）
    -k, --keep-temp     保留临时构建文件（用于调试）

示例:
    $0                                          自动检测版本和架构，直接从源码构建
    $0 -v 1.4.1                                 使用指定版本号直接从源码构建
    $0 -r build/rpm/x86_64/ter-music-1.0.0-1.x86_64.rpm  从指定RPM包转换
    $0 --keep-temp                              构建后保留临时文件

输出:
    可移植包将输出到: ${OUTPUT_DIR}/<arch>/

EOF
}

check_dependencies() {
    local target_arch="${1:-}"
    
    log_info "检查构建依赖..."

    local missing_deps=()

    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi

    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
    fi

    # 检查 gcc
    if ! command -v gcc &> /dev/null; then
        missing_deps+=("gcc")
    fi

    if ! command -v tar &> /dev/null; then
        missing_deps+=("tar")
    fi

    if [ ${#missing_deps[@]} -gt 0 ]; then
        log_error "缺少以下构建工具:"
        for dep in "${missing_deps[@]}"; do
            echo "  - $dep"
        done
        echo ""
        log_error "请使用以下命令安装缺失的工具:"
        echo "  Debian/Ubuntu: sudo apt install cmake make gcc tar"
        echo "  Fedora/RHEL: sudo dnf install cmake make gcc tar"
        exit 1
    fi

    log_info "所有构建依赖已满足"
}

detect_version() {
    local default_version="2.2.0"

    if [ -d "${SCRIPT_DIR}/.git" ] && command -v git >/dev/null 2>&1; then
        local git_version=$(git describe --tags --abbrev=0 2>/dev/null || true)
        if [[ $git_version =~ ^v?([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
            echo "${BASH_REMATCH[1]}"
            return
        fi
    fi

    if [ -f "${SCRIPT_DIR}/include/org.yxzl.ter-music/types.h" ]; then
        local match
        match=$(grep -E 'APP_VERSION' "${SCRIPT_DIR}/include/org.yxzl.ter-music/types.h" | head -1)
        if [[ $match =~ ([0-9]+\.[0-9]+\.[0-9]+) ]]; then
            echo "${BASH_REMATCH[1]}"
            return
        fi
    fi

    echo "$default_version"
}

detect_architecture() {
    # Only x86_64 is supported
    echo "x86_64"
}

validate_architecture() {
    local arch="$1"
    if [ "$arch" != "x86_64" ]; then
        log_error "不支持的架构: $arch（仅支持 x86_64）"
        return 1
    fi
    return 0
}

find_rpm_package() {
    local rpm_file="$1"

    if [ -z "$rpm_file" ]; then
        rpm_file=$(find "${SCRIPT_DIR}/build/rpm" -name "${PROJECT_NAME}-*.x86_64.rpm" -type f | grep -v debuginfo | grep -v debugsource | head -1)
    fi

    if [ -z "$rpm_file" ] || [ ! -f "$rpm_file" ]; then
        log_error "未找到RPM包文件"
        log_error "请先运行 ./build-rpm.sh 构建RPM包，或使用 -r 选项指定RPM包路径"
        exit 1
    fi

    echo "$rpm_file"
}

extract_rpm() {
    local rpm_file="$1"
    local extract_dir="$2"

    log_info "解压RPM包: $rpm_file"

    mkdir -p "${extract_dir}"
    cd "${extract_dir}"

    rpm2cpio "$rpm_file" | cpio -idmv

    log_info "RPM包解压完成"
}

build_from_source() {
    local build_dir="$1"
    local target_arch="${2:-}"

    log_info "从源码构建..."

    mkdir -p "${build_dir}"
    cd "${build_dir}"

    # 准备CMake参数
    local cmake_args=("${SCRIPT_DIR}" -DCMAKE_BUILD_TYPE=Release)

    cmake "${cmake_args[@]}"
    make -j"$(nproc)"

    log_info "源码构建完成"
}

copy_dependencies() {
    local binary="$1"
    local portable_dir="$2"

    log_info "复制依赖库..."

    local lib_dir="${portable_dir}/lib"
    local processed=()

    mkdir -p "${lib_dir}"

    if [ ! -f "$binary" ]; then
        log_error "未找到二进制文件: $binary"
        exit 1
    fi

    log_info "二进制文件: $binary"
    log_info "目标库目录: $lib_dir"

    local deps=$(ldd "$binary" | grep -E "=>\s+/" | awk '{print $3}' | sort -u)
    log_info "找到直接依赖: $(echo "$deps" | wc -w) 个"

    process_dependency() {
        local dep="$1"
        local target_lib_dir="$2"

        if [ ! -f "$dep" ]; then
            return
        fi

        local dep_name=$(basename "$dep")

        if [[ "$dep_name" =~ ^libc\.so.* ]]; then
            return
        fi
        if [[ "$dep_name" =~ ^ld-linux.*\.so.* ]]; then
            return
        fi
        if [[ "$dep_name" =~ ^libpthread\.so.* ]]; then
            return
        fi
        if [[ "$dep_name" =~ ^libdl\.so.* ]]; then
            return
        fi
        if [[ "$dep_name" =~ ^librt\.so.* ]]; then
            return
        fi
        if [[ "$dep_name" =~ ^libm\.so.* ]]; then
            return
        fi

        if printf "%s\n" "${processed[@]}" | grep -qFx "$dep"; then
            return
        fi

        processed+=("$dep")

        if [ ! -f "${target_lib_dir}/${dep_name}" ]; then
            cp -L "$dep" "${target_lib_dir}/"
            log_info "  复制: $dep_name"
        fi

        local sub_deps=$(ldd "$dep" | grep -E "^\s+/" | awk '{print $3}')
        for sub_dep in $sub_deps; do
            process_dependency "$sub_dep" "$target_lib_dir"
        done
    }

    for dep in $deps; do
        process_dependency "$dep" "$lib_dir"
    done

    local copied=$(ls -1 "${lib_dir}" | wc -l)
    log_info "依赖库复制完成（共 ${#processed[@]} 个依赖，复制了 $copied 个文件到库目录）"
    if [ "$copied" -eq 0 ]; then
        log_error "警告：库目录仍然为空！这可能意味着没有复制任何依赖库"
    fi
}

create_portable_package() {
    local binary_source="$1"
    local binary_name="$2"
    local portable_dir="$3"
    local version="$4"

    log_info "创建可移植包结构..."

    mkdir -p "${portable_dir}"/{bin,lib,share}

    cp "${binary_source}/${binary_name}" "${portable_dir}/bin/"

    mkdir -p "${portable_dir}/share/ter-music"
    cp "${SCRIPT_DIR}/data/help-quickstart-zh.txt" "${portable_dir}/share/ter-music/" 2>/dev/null || true
    cp "${SCRIPT_DIR}/data/help-quickstart-en.txt" "${portable_dir}/share/ter-music/" 2>/dev/null || true

    cat > "${portable_dir}/run.sh" << 'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="${SCRIPT_DIR}/bin:${PATH}"
export LD_LIBRARY_PATH="${SCRIPT_DIR}/lib:${LD_LIBRARY_PATH}"

exec "${SCRIPT_DIR}/bin/ter-music" "$@"
EOF

    chmod +x "${portable_dir}/run.sh"

    cat > "${portable_dir}/README.txt" << EOF
Ter-Music 可移植包
===================

版本: ${version}

使用方法:
1. 解压此压缩包
2. 进入解压后的目录
3. 运行: ./run.sh

或者直接运行:
  ./run.sh --help

注意:
- 此包包含所有必要的依赖库
- 可以在任何兼容的Linux系统上运行
- 不需要安装任何依赖

特性:
- 支持多种音频格式 (MP3, WAV, FLAC, OGG, M4A, AAC, WMA, APE, OPUS)
- LRC歌词同步显示
- 多种播放模式 (顺序、单曲循环、列表循环、随机)
- 播放列表管理
- 收藏功能
- 播放历史
- 自定义颜色主题
- 键盘快捷键
- 实时进度条

项目主页: https://github.com/HuanSoft-Open-Source-Community/ter-music
EOF

    log_info "可移植包结构创建完成"
}

create_tarball() {
    local portable_dir="$1"
    local version="$2"
    local target_arch="$3"

    local package_name="${PROJECT_NAME}-${version}-portable-${target_arch}"
    local tarball_name="${package_name}.tar.gz"
    local tarball_path="${OUTPUT_DIR}/${target_arch}/${tarball_name}"

    mkdir -p "${OUTPUT_DIR}/${target_arch}"

    local original_dir=$(pwd)
    cd "$(dirname "${portable_dir}")"

    if [ -d "$(basename "${portable_dir}")" ]; then
        if tar -czf "${tarball_path}" "$(basename "${portable_dir}")"; then
            cd "$original_dir"
            # Echo path to stdout for command-substitution capture;
            # redirect copy_to_release output to stderr so its log_info
            # does not pollute the captured path.
            echo "${tarball_path}"
            copy_to_release "${tarball_path}" >&2
            return 0
        else
            cd "$original_dir"
            return 1
        fi
    else
        cd "$original_dir"
        return 1
    fi
}

# ── 修复容器内构建产物的所有权 ────────────────────────────
fix_output_ownership() {
    if [ -z "${HOST_UID:-}" ] || [ -z "${HOST_GID:-}" ]; then
        return 0
    fi
    if [ "$(id -u)" != "0" ]; then
        if [ -d "${OUTPUT_DIR}" ]; then
            chmod -R u+rwX "${OUTPUT_DIR}" 2>/dev/null || true
        fi
        return 0
    fi
    if [ "${HOST_UID}" != "0" ]; then
        log_info "修复构建产物所有权为宿主用户 (${HOST_UID}:${HOST_GID})..."
        if [ -d "${OUTPUT_DIR}" ]; then
            chown -R "${HOST_UID}:${HOST_GID}" "${OUTPUT_DIR}" 2>/dev/null || \
                chmod -R u+rwX,go+rX "${OUTPUT_DIR}" 2>/dev/null || true
        fi
    fi
}

cleanup() {
    local keep_temp="$1"

    if [ "$keep_temp" != "true" ]; then
        log_clean "清理临时文件..."
        # Ensure temp files are writable before removal (some may be
        # created with restrictive permissions during the build)
        if [ -d "${TEMP_DIR}" ]; then
            chmod -R u+rwX "${TEMP_DIR}" 2>/dev/null || true
            rm -rf "${TEMP_DIR}" || true
        fi
        log_clean "临时文件已清理"
    else
        log_info "保留临时文件: ${TEMP_DIR}"
    fi
}

show_summary() {
    local target_arch="$1"
    local tarball_path="$2"

    echo ""
    echo "=========================================="
    echo "可移植包构建完成！"
    echo "=========================================="
    echo ""
    echo "目标架构: $target_arch"
    echo "输出目录: $(dirname "$tarball_path")/"
    echo ""
    echo "生成的可移植包:"
    ls -lh "$tarball_path" 2>/dev/null || echo "  未找到可移植包"
    echo ""
    echo "使用方法:"
    echo "  1. 解压: tar -xzf ${tarball_path}"
    echo "  2. 进入目录: cd ${PROJECT_NAME}-*-portable-${target_arch}"
    echo "  3. 运行: ./run.sh"
    echo ""
}

prepare_directories() {
    local target_arch="$1"
    log_info "准备构建目录..."

    mkdir -p "${OUTPUT_DIR}/${target_arch}"

    rm -rf "${TEMP_DIR}"
    mkdir -p "${TEMP_DIR}"

    log_clean "已清理并创建构建目录"
}

main() {
    local version=""
    local rpm_file=""
    local keep_temp="false"
    local target_arch=""

    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -v|--version)
                version="$2"
                shift 2
                ;;
            -a|--arch)
                target_arch="$2"
                shift 2
                ;;
            -r|--rpm)
                rpm_file="$2"
                shift 2
                ;;
            -k|--keep-temp)
                keep_temp="true"
                shift
                ;;
            *)
                log_error "未知选项: $1"
                show_help
                exit 1
                ;;
        esac
    done

    echo "=========================================="
    echo "Ter-Music 可移植包构建脚本"
    echo "=========================================="
    echo ""

    log_info "构建环境信息:"
    log_info "  操作系统: $(uname -s)"
    log_info "  内核版本: $(uname -r)"
    log_info "  主机架构: $(uname -m)"
    echo ""

    if [ -z "$version" ]; then
        version=$(detect_version)
        if [ "$version" != "1.0.0" ]; then
            log_info "从 Git/types.h 检测到版本: $version"
        else
            log_info "无法检测版本，使用默认版本: $version"
        fi
    else
        log_info "使用指定版本: $version"
    fi

    if [ -z "$target_arch" ]; then
        target_arch=$(detect_architecture)
        if [ $? -eq 0 ]; then
            log_info "自动检测到架构: $target_arch"
        else
            log_error "无法检测系统架构"
            exit 1
        fi
    else
        if ! validate_architecture "$target_arch"; then
            exit 1
        fi
        log_info "使用指定架构: $target_arch"
    fi
    
    check_dependencies "$target_arch"

    prepare_directories "$target_arch"

    local portable_dir="${TEMP_DIR}/${PROJECT_NAME}-portable"
    mkdir -p "${portable_dir}"

    if [ -n "$rpm_file" ]; then
        rpm_file=$(find_rpm_package "$rpm_file")
        log_info "使用RPM包: $rpm_file"

        local extract_dir="${TEMP_DIR}/rpm_extract"
        extract_rpm "$rpm_file" "$extract_dir"

        if [ -z "$version" ]; then
            version=$(basename "$rpm_file" | sed -E "s/${PROJECT_NAME}-([0-9]+\.[0-9]+\.[0-9]+).*/\1/")
            log_info "从RPM文件名提取版本: $version"
        fi

        create_portable_package "$extract_dir/usr/bin" "$PROJECT_NAME" "$portable_dir" "$version"
        copy_dependencies "$extract_dir/usr/bin/${PROJECT_NAME}" "$portable_dir"
    else
        local build_dir="${TEMP_DIR}/build"
        build_from_source "$build_dir" "$target_arch"

        create_portable_package "$build_dir" "$PROJECT_NAME" "$portable_dir" "$version"
        copy_dependencies "$build_dir/${PROJECT_NAME}" "$portable_dir"
    fi

    log_info "创建压缩包..."
    local tarball_path=$(create_tarball "$portable_dir" "$version" "$target_arch")

    if [ -n "$tarball_path" ] && [ -f "$tarball_path" ]; then
        log_info "压缩包已创建: ${tarball_path}"
        fix_output_ownership
        cleanup "$keep_temp"
        show_summary "$target_arch" "$tarball_path"
    else
        log_error "可移植包构建失败，跳过清理和总结步骤"
        fix_output_ownership
        cleanup "$keep_temp"
        exit 1
    fi
}

main "$@"
