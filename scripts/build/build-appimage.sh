#!/bin/bash

set -e

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
SCRIPT_DIR="$(pwd)"
PROJECT_NAME="ter-music"
OUTPUT_DIR="${SCRIPT_DIR}/build/appimage"
TEMP_DIR="${SCRIPT_DIR}/.appimage_temp"
APPIMAGETOOL_DIR="${SCRIPT_DIR}/.appimagetool"
APPIMAGETOOL_VERSION="continuous"

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
        cp "$source_file" "$release_dir/"
        log_info "构建结果已复制到: ${release_dir}/$(basename "$source_file")"
    fi
}

# 检测是否需要交叉编译
is_cross_compiling() {
    local host_arch=$(uname -m)
    local target_arch="$1"
    
    # 标准化架构名称进行比较
    local normalized_host="$host_arch"
    local normalized_target="$target_arch"
    
    # 将 x86_64 和 amd64 视为相同
    if [ "$normalized_host" = "x86_64" ]; then
        normalized_host="amd64"
    fi
    if [ "$normalized_target" = "x86_64" ]; then
        normalized_target="amd64"
    fi
    
    # 将 aarch64 和 arm64 视为相同
    if [ "$normalized_host" = "aarch64" ]; then
        normalized_host="arm64"
    fi
    if [ "$normalized_target" = "aarch64" ]; then
        normalized_target="arm64"
    fi
    
    if [ "$normalized_host" != "$normalized_target" ]; then
        return 0  # true - 需要交叉编译
    else
        return 1  # false - 不需要交叉编译
    fi
}

# 获取交叉编译工具链前缀
get_cross_compile_prefix() {
    local target_arch="$1"
    
    case "$target_arch" in
        arm64|aarch64)
            echo "aarch64-linux-gnu"
            ;;
        *)
            echo ""
            ;;
    esac
}

# 检查交叉编译依赖
check_cross_compile_deps() {
    local target_arch="$1"
    local arch_prefix=$(get_cross_compile_prefix "$target_arch")
    
    if [ -z "$arch_prefix" ]; then
        return 0
    fi
    
    log_info "检查交叉编译工具链..."
    
    local missing_deps=()
    
    # 检查交叉编译器
    if ! command -v ${arch_prefix}-gcc &> /dev/null; then
        missing_deps+=("gcc-${arch_prefix}")
    fi
    
    if ! command -v ${arch_prefix}-g++ &> /dev/null; then
        missing_deps+=("g++-${arch_prefix}")
    fi
    
    if ! command -v ${arch_prefix}-ar &> /dev/null; then
        missing_deps+=("binutils-${arch_prefix}")
    fi
    
    if [ ${#missing_deps[@]} -gt 0 ]; then
        log_error "缺少以下交叉编译工具链组件:"
        for dep in "${missing_deps[@]}"; do
            echo "  - $dep"
        done
        echo ""
        log_error "请使用以下命令安装交叉编译工具链:"
        echo "  sudo apt install gcc-${arch_prefix} g++-${arch_prefix} binutils-${arch_prefix}"
        return 1
    fi
    
    # 检查aarch64开发库
    local pkg_config_dir="/usr/lib/${arch_prefix}/pkgconfig"
    local missing_libs=()
    
    if [ ! -d "$pkg_config_dir" ] && [ ! -d "/usr/${arch_prefix}/lib/pkgconfig" ]; then
        log_warn "未找到 ${arch_prefix} 的 pkgconfig 目录"
        missing_libs+=("缺少 pkgconfig 目录")
    fi
    
    # 检查常用开发库
    local required_libs=("libpng" "libjpeg" "libcurl")
    for lib in "${required_libs[@]}"; do
        if [ -d "$pkg_config_dir" ]; then
            if ! pkg-config --exists --silence-errors "$lib" 2>/dev/null && \
               ! pkg-config --exists --silence-errors "$lib" --define-prefix="${arch_prefix}" 2>/dev/null; then
                missing_libs+=("$lib")
            fi
        else
            missing_libs+=("$lib (需要安装 ${lib}-dev:arm64)")
        fi
    done
    
    if [ ${#missing_libs[@]} -gt 0 ]; then
        log_warn "以下开发库可能缺失或未正确安装:"
        for lib in "${missing_libs[@]}"; do
            echo "  - $lib"
        done
        echo ""
        log_warn "请确保已安装目标架构的开发库，例如:"
        echo "  sudo apt install libpng-dev:arm64 libjpeg-dev:arm64"
    fi
    
    log_info "交叉编译工具链检查通过"
    return 0
}

# 设置交叉编译环境变量
setup_cross_compile_env() {
    local target_arch="$1"
    local arch_prefix=$(get_cross_compile_prefix "$target_arch")
    
    if [ -z "$arch_prefix" ]; then
        return 0
    fi
    
    log_info "设置交叉编译环境..."
    
    export CC=${arch_prefix}-gcc
    export CXX=${arch_prefix}-g++
    export AR=${arch_prefix}-ar
    export STRIP=${arch_prefix}-strip
    export LD=${arch_prefix}-ld
    export PKG_CONFIG_PATH=/usr/lib/${arch_prefix}/pkgconfig
    export PKG_CONFIG_LIBDIR=/usr/lib/${arch_prefix}/pkgconfig
    
    log_info "交叉编译环境已设置:"
    log_info "  CC=$CC"
    log_info "  CXX=$CXX"
    log_info "  PKG_CONFIG_PATH=$PKG_CONFIG_PATH"
}

show_help() {
    cat << EOF
用法: $0 [选项]

直接从源码构建 ter-music AppImage 格式（也可从 RPM 转换）

选项:
    -h, --help          显示此帮助信息
    -v, --version VERSION  指定版本号（默认：自动检测）
    -a, --arch ARCH     指定目标架构（默认：自动检测）
    -r, --rpm FILE      从指定的RPM包文件转换（可选）
    -k, --keep-temp     保留临时构建文件（用于调试）

支持的架构:
    x86_64              Intel/AMD 64位
    aarch64             ARM 64位
    loong64             龙芯新世界
    loongarch64         龙芯旧世界
    sw64                申威
    mips64              MIPS 64位

交叉编译:
    在 x86_64 机器上构建 aarch64 包时，会自动使用交叉编译
    需要安装交叉编译工具链:
      sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu
    需要安装目标架构开发库:
      sudo dpkg --add-architecture arm64
      sudo apt update
      sudo apt install libncurses-dev:arm64 libpng-dev:arm64 libjpeg-dev:arm64 libcurl4-openssl-dev:arm64 \
                       libavcodec-dev:arm64 libavfilter-dev:arm64 \
                       libavformat-dev:arm64 libswresample-dev:arm64 libavutil-dev:arm64 libpulse-dev:arm64

示例:
    $0                                          自动检测版本和架构，直接从源码构建
    $0 -v 1.4.1                                 使用指定版本号直接从源码构建
    $0 -a aarch64                              为 ARM 64位架构构建
    $0 -v 1.4.1 -a aarch64                    指定版本和架构构建
    $0 -r build/rpm/x86_64/ter-music-1.0.0-1.x86_64.rpm  从指定RPM包转换
    $0 --keep-temp                              构建后保留临时文件

输出:
    AppImage 包将输出到: ${OUTPUT_DIR}/<<arch>/

EOF
}

check_dependencies() {
    local target_arch="${1:-}"
    
    log_info "检查构建依赖..."

    local missing_deps=()

    if ! command -v unsquashfs &> /dev/null; then
        missing_deps+=("squashfs-tools")
    fi

    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi

    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
    fi

    # 检查是否需要交叉编译
    if [ -n "$target_arch" ] && is_cross_compiling "$target_arch"; then
        # 交叉编译模式下，不检查本地gcc
        if ! check_cross_compile_deps "$target_arch"; then
            exit 1
        fi
    else
        # 本地编译模式下，检查本地gcc
        if ! command -v gcc &> /dev/null; then
            missing_deps+=("gcc")
        fi
    fi

    if [ ${#missing_deps[@]} -gt 0 ]; then
        log_error "缺少以下构建工具:"
        for dep in "${missing_deps[@]}"; do
            echo "  - $dep"
        done
        echo ""
        log_error "请使用以下命令安装缺失的工具:"
        echo "  Debian/Ubuntu: sudo apt install squashfs-tools cmake make gcc"
        echo "  Fedora/RHEL: sudo dnf install squashfs-tools cmake make gcc"
        exit 1
    fi

    log_info "所有构建依赖已满足"
}

check_environment() {
    log_info "检查构建环境..."

    if [ ! -d "${SCRIPT_DIR}/src" ]; then
        log_error "未找到源代码目录: ${SCRIPT_DIR}/src"
        log_error "请确保在项目根目录下运行此脚本"
        exit 1
    fi

    if [ ! -f "${SCRIPT_DIR}/CMakeLists.txt" ]; then
        log_error "未找到 CMakeLists.txt 文件"
        log_error "请确保在项目根目录下运行此脚本"
        exit 1
    fi

    if [ ! -f "${SCRIPT_DIR}/include/defs.h" ]; then
        log_error "未找到 defs.h 文件"
        log_error "请确保项目结构完整"
        exit 1
    fi

    if [ ! -d "${SCRIPT_DIR}/resources/icons/hicolor/128x128/apps" ]; then
        log_error "未找到图标文件目录"
        log_error "请确保项目结构完整"
        exit 1
    fi

    log_info "构建环境检查通过"
}

check_download_tool() {
    log_info "检查下载工具..."

    if command -v aria2c &> /dev/null; then
        log_info "检测到 aria2c，将使用 aria2 进行下载"
        return 0
    elif command -v wget &> /dev/null; then
        log_info "检测到 wget，将使用 wget 进行下载"
        return 1
    elif command -v curl &> /dev/null; then
        log_info "检测到 curl，将使用 curl 进行下载"
        return 2
    else
        log_error "未找到下载工具"
        log_error "请安装以下任一下载工具:"
        echo "  - aria2 (推荐): sudo apt install aria2 或 sudo dnf install aria2"
        echo "  - wget: sudo apt install wget 或 sudo dnf install wget"
        echo "  - curl: sudo apt install curl 或 sudo dnf install curl"
        exit 1
    fi
}

detect_version() {
    local default_version="1.0.0"

    if [ -d "${SCRIPT_DIR}/.git" ] && command -v git >/dev/null 2>&1; then
        local git_version=$(git describe --tags --abbrev=0 2>/dev/null || true)
        if [[ $git_version =~ ^v?([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
            echo "${BASH_REMATCH[1]}"
            return
        fi
    fi

    if [ -f "${SCRIPT_DIR}/include/defs.h" ]; then
        local match
        match=$(grep -E 'APP_VERSION' "${SCRIPT_DIR}/include/defs.h" | head -1)
        if [[ $match =~ ([0-9]+\.[0-9]+\.[0-9]+) ]]; then
            echo "${BASH_REMATCH[1]}"
            return
        fi
    fi

    if [ -f "${SCRIPT_DIR}/CMakeLists.txt" ]; then
        local match
        match=$(grep -E 'project.*VERSION' "${SCRIPT_DIR}/CMakeLists.txt" | head -1)
        if [[ $match =~ ([0-9]+\.[0-9]+\.[0-9]+) ]]; then
            echo "${BASH_REMATCH[1]}"
            return
        else
            match=$(grep -E 'set.*VERSION' "${SCRIPT_DIR}/CMakeLists.txt" | head -1)
            if [[ $match =~ ([0-9]+\.[0-9]+\.[0-9]+) ]]; then
                echo "${BASH_REMATCH[1]}"
                return
            fi
        fi
    fi

    echo "$default_version"
}

detect_architecture() {
    local arch=$(uname -m)
    
    case "$arch" in
        x86_64)
            echo "x86_64"
            ;;
        aarch64|arm64)
            echo "aarch64"
            ;;
        loongarch64)
            echo "loongarch64"
            ;;
        loong64)
            echo "loong64"
            ;;
        mips64)
            echo "mips64"
            ;;
        sw_64|sw64)
            echo "sw64"
            ;;
        *)
            log_error "未知的架构: $arch"
            echo "unknown"
            return 1
            ;;
    esac
}

validate_architecture() {
    local arch="$1"
    local valid_archs=("x86_64" "aarch64" "loong64" "loongarch64" "sw64" "mips64")
    
    for valid_arch in "${valid_archs[@]}"; do
        if [ "$arch" = "$valid_arch" ]; then
            return 0
        fi
    done
    
    log_error "不支持的架构: $arch"
    log_error "支持的架构列表: ${valid_archs[*]}"
    return 1
}

download_appimagetool() {
    local appimagetool_dir="${APPIMAGETOOL_DIR}"

    if [ ! -f "${appimagetool_dir}/appimagetool" ]; then
        log_info "下载 appimagetool..."
        mkdir -p "${appimagetool_dir}"

        local appimagetool_url="https://github.com/AppImage/AppImageKit/releases/download/${APPIMAGETOOL_VERSION}/appimagetool-x86_64.AppImage"

        local download_tool=$(check_download_tool)
        local download_result=$?

        case $download_result in
            0)
                aria2c -x 16 -s 16 -k 1M --max-tries=5 --retry-wait=2 "${appimagetool_url}" -d "${appimagetool_dir}" -o "appimagetool"
                ;;
            1)
                wget "${appimagetool_url}" -O "${appimagetool_dir}/appimagetool"
                ;;
            2)
                curl -L "${appimagetool_url}" -o "${appimagetool_dir}/appimagetool"
                ;;
        esac

        if [ $? -ne 0 ]; then
            log_error "下载 appimagetool 失败"
            exit 1
        fi

        chmod +x "${appimagetool_dir}/appimagetool"
        log_info "appimagetool 下载完成"
    fi
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

    # 检查是否需要交叉编译
    if [ -n "$target_arch" ] && is_cross_compiling "$target_arch"; then
        setup_cross_compile_env "$target_arch"
        log_info "使用交叉编译模式构建目标架构: $target_arch"
    fi

    mkdir -p "${build_dir}"
    cd "${build_dir}"

    # 准备CMake参数
    local cmake_args=("${SCRIPT_DIR}" -DCMAKE_BUILD_TYPE=Release)
    
    # 如果是交叉编译，添加工具链文件
    if [ -n "$target_arch" ] && is_cross_compiling "$target_arch"; then
        local toolchain_file="${SCRIPT_DIR}/cmake/toolchain-aarch64-linux-gnu.cmake"
        if [ -f "$toolchain_file" ]; then
            cmake_args+=(-DCMAKE_TOOLCHAIN_FILE="$toolchain_file")
            log_info "使用工具链文件: $toolchain_file"
        fi
    fi

    cmake "${cmake_args[@]}"
    make -j$(nproc)

    log_info "源码构建完成"
}

create_appdir() {
    local binary_source="$1"
    local binary_name="$2"
    local appdir="$3"
    local version="$4"

    log_info "创建 AppDir 结构..."

    mkdir -p "${appdir}"/{usr/bin,usr/lib,usr/share/applications,usr/share/icons/hicolor/128x128/apps}

    cp "${binary_source}/${binary_name}" "${appdir}/usr/bin/"

    mkdir -p "${appdir}/usr/share/ter-music"
    cp "${SCRIPT_DIR}/data/help-quickstart-zh.txt" "${appdir}/usr/share/ter-music/" 2>/dev/null || true
    cp "${SCRIPT_DIR}/data/help-quickstart-en.txt" "${appdir}/usr/share/ter-music/" 2>/dev/null || true

    cat > "${appdir}/AppRun" << 'EOF'
#!/bin/bash
SELF=$(readlink -f "$0")
HERE=${SELF%/*}
export PATH="${HERE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"

exec "${HERE}/usr/bin/ter-music" "$@"
EOF

    chmod +x "${appdir}/AppRun"

    cat > "${appdir}/${PROJECT_NAME}.desktop" << EOF
[Desktop Entry]
Name=Ter-Music
Comment=A terminal-based music player with ncurses interface
Exec=ter-music
Icon=ter-music
Terminal=true
Type=Application
Categories=Audio;AudioVideo;Player;
Keywords=Music;Player;Terminal;Console;
EOF

    cat > "${appdir}/AppInfo.xml" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<AppImage xmlns="http://www.appimage.org/1.0">
  <id>ter-music</id>
  <name>Ter-Music</name>
  <summary>A terminal-based music player with ncurses interface</summary>
  <description>
    <p>Ter-Music is a lightweight terminal-based music player for Linux systems.</p>
    <p>It uses FFmpeg for audio decoding, PulseAudio for audio output, and ncursesw for a beautiful text user interface.</p>
  </description>
  <version>${version}</version>
  <release>1</release>
  <categories>
    <category>Audio</category>
    <category>AudioVideo</category>
    <category>Player</category>
  </categories>
  <keywords>
    <keyword>Music</keyword>
    <keyword>Player</keyword>
    <keyword>Terminal</keyword>
    <keyword>Console</keyword>
  </keywords>
  <url type="homepage">https://github.com/YXZL985/ter-music</url>
  <url type="bugtracker">https://github.com/YXZL985/ter-music/issues</url>
  <url type="help">https://github.com/YXZL985/ter-music</url>
  <project_license>GPL-3.0-or-later</project_license>
  <developer_name>YXZL985</developer_name>
</AppImage>
EOF

    log_info "AppDir 结构创建完成"
}

copy_dependencies() {
    local appdir="$1"

    log_info "复制依赖库..."

    local binary="${appdir}/usr/bin/${PROJECT_NAME}"
    local lib_dir="${appdir}/usr/lib"
    local processed=()

    if [ ! -f "$binary" ]; then
        log_error "未找到二进制文件: $binary"
        exit 1
    fi

    process_dependency() {
        local dep="$1"

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

        if [ ! -f "${lib_dir}/${dep_name}" ]; then
            cp -L "$dep" "${lib_dir}/"
            log_info "  复制: $dep_name"
        fi

        local sub_deps=$(ldd "$dep" | grep -E "^\s+/" | awk '{print $3}')
        for sub_dep in $sub_deps; do
            process_dependency "$sub_dep"
        done
    }

    local deps=$(ldd "$binary" | grep -E "=>\s+/" | awk '{print $3}' | sort -u)

    for dep in $deps; do
        process_dependency "$dep"
    done

    log_info "依赖库复制完成（共 ${#processed[@]} 个库）"
}

create_icon() {
    local appdir="$1"

    log_info "复制应用图标..."

    cp "${SCRIPT_DIR}/resources/icons/hicolor/128x128/apps/ter-music.png" "${appdir}/ter-music.png"
    cp "${SCRIPT_DIR}/resources/icons/hicolor/128x128/apps/ter-music.png" "${appdir}/.DirIcon"
    cp "${SCRIPT_DIR}/resources/icons/hicolor/128x128/apps/ter-music.png" "${appdir}/usr/share/icons/hicolor/128x128/apps/ter-music.png"

    log_info "应用图标复制完成"
}

build_appimage() {
    local appdir="$1"
    local version="$2"
    local output_file="$3"
    local target_arch="$4"

    log_info "构建 AppImage..."

    local appimagetool_dir="${APPIMAGETOOL_DIR}"
    local appimagetool_binary="${appimagetool_dir}/squashfs-root/usr/bin/appimagetool"

    if [ ! -f "$appimagetool_binary" ]; then
        download_appimagetool
        chmod +x "${appimagetool_dir}/appimagetool"
        cd "${appimagetool_dir}"
        ./appimagetool --appimage-extract
        cd - > /dev/null
    fi

    export ARCH="$target_arch"

    if "$appimagetool_binary" --no-appstream "${appdir}" "${output_file}"; then
        chmod +x "${output_file}"
        log_info "AppImage 构建完成"
        copy_to_release "${output_file}"
        return 0
    else
        log_error "AppImage 构建失败"
        return 1
    fi
}

cleanup() {
    local keep_temp="$1"

    if [ "$keep_temp" != "true" ]; then
        log_clean "清理临时文件..."
        rm -rf "${TEMP_DIR}"
        log_clean "临时文件已清理"
    else
        log_info "保留临时文件: ${TEMP_DIR}"
    fi
}

show_summary() {
    local target_arch="$1"
    local output_file="$2"

    echo ""
    echo "=========================================="
    echo "AppImage 构建完成！"
    echo "=========================================="
    echo ""
    echo "目标架构: $target_arch"
    echo "输出目录: $(dirname "$output_file")/"
    echo ""
    echo "生成的 AppImage 包:"
    ls -lh "$output_file" 2>/dev/null || echo "  未找到 AppImage 包"
    echo ""
    echo "使用方法:"
    echo "  chmod +x ${output_file}"
    echo "  ./${output_file}"
    echo ""
    echo "或者直接运行:"
    echo "  ${output_file}"
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
    echo "Ter-Music AppImage 构建脚本"
    echo "=========================================="
    echo ""

    log_info "构建环境信息:"
    log_info "  操作系统: $(uname -s)"
    log_info "  内核版本: $(uname -r)"
    log_info "  主机架构: $(uname -m)"
    echo ""

    check_environment

    if [ -z "$version" ]; then
        version=$(detect_version)
        if [ "$version" != "1.0.0" ]; then
            log_info "从 Git/defs.h/CMakeLists.txt 检测到版本: $version"
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
    
    # 检查是否需要交叉编译
    if is_cross_compiling "$target_arch"; then
        log_info "检测到交叉编译模式: $(uname -m) -> $target_arch"
    fi
    
    check_dependencies "$target_arch"

    prepare_directories "$target_arch"

    local appdir="${TEMP_DIR}/AppDir"
    local output_file="${OUTPUT_DIR}/${target_arch}/${PROJECT_NAME}-${version}-${target_arch}.AppImage"

    if [ -n "$rpm_file" ]; then
        rpm_file=$(find_rpm_package "$rpm_file")
        log_info "使用RPM包: $rpm_file"

        local extract_dir="${TEMP_DIR}/rpm_extract"
        extract_rpm "$rpm_file" "$extract_dir"

        if [ -z "$version" ]; then
            version=$(basename "$rpm_file" | sed -E "s/${PROJECT_NAME}-([0-9]+\.[0-9]+\.[0-9]+).*/\1/")
            log_info "从RPM文件名提取版本: $version"
        fi

        create_appdir "$extract_dir/usr/bin" "$PROJECT_NAME" "$appdir" "$version"
        copy_dependencies "$appdir"
        create_icon "$appdir"
    else
        local build_dir="${TEMP_DIR}/build"
        build_from_source "$build_dir" "$target_arch"

        create_appdir "$build_dir" "$PROJECT_NAME" "$appdir" "$version"
        copy_dependencies "$appdir"
        create_icon "$appdir"
    fi

    if build_appimage "$appdir" "$version" "$output_file" "$target_arch"; then
        cleanup "$keep_temp"
        show_summary "$target_arch" "$output_file"
    else
        log_error "AppImage 构建失败，跳过清理和总结步骤"
        cleanup "$keep_temp"
        exit 1
    fi
}

main "$@"
