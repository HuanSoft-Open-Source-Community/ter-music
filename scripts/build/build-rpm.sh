#!/bin/bash

set -e

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
SCRIPT_DIR="$(pwd)"
PROJECT_NAME="ter-music"
DEFAULT_VERSION="2.1.0"
OUTPUT_DIR="${SCRIPT_DIR}/build/rpm"
TEMP_DIR="${SCRIPT_DIR}/.rpmbuild_temp"

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

将 ter-music 项目打包成标准的 Fedora RPM 包

选项:
    -h, --help          显示此帮助信息
    -v, --version VERSION  指定版本号（默认：自动检测）
    -a, --arch ARCH     指定目标架构（默认：自动检测）
    -k, --keep-temp     保留临时构建文件（用于调试）
    --with-debuginfo    生成 debuginfo 包（默认不生成）

    --container         在 Rocky Linux 容器中构建 RPM（解决跨发行版兼容性问题）
    --static            构建通用静态链接 RPM，单包兼容 EL8/9/10（自动启用 --container）
    --el-version VERSION  指定目标 EL 版本: 8、9 或 10（默认：9，需配合 --container 使用）

示例:
    $0                  使用自动检测的版本号和架构构建 RPM
    $0 -v 1.2.3         使用指定版本号 1.2.3 构建 RPM
    $0 --keep-temp      构建后保留临时文件
    $0 --with-debuginfo 生成 debuginfo 包

输出:
    RPM 包将输出到: ${OUTPUT_DIR}/<arch>/

EOF
}

check_dependencies() {
    local target_arch="${1:-}"
    local static_build="${2:-false}"

    log_info "检查构建依赖..."
    
    local missing_deps=()
    local optional_missing=()
    local is_debian_based=false
    
    # 检测是否为 Debian/Ubuntu/Deepin 系统
    if [ -f /etc/debian_version ] || command -v dpkg &> /dev/null; then
        is_debian_based=true
    fi
    
    if ! command -v rpmbuild &> /dev/null; then
        if [ "$is_debian_based" = true ]; then
            missing_deps+=("rpm")
            missing_deps+=("rpm-build")
        else
            missing_deps+=("rpm-build")
        fi
    fi
    
    # 检查本地编译器
    if ! command -v gcc &> /dev/null; then
        missing_deps+=("gcc")
    fi
    
    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
    fi
    
    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi
    
    if ! command -v tar &> /dev/null; then
        missing_deps+=("tar")
    fi
    
    if ! command -v pkg-config &> /dev/null; then
        missing_deps+=("pkg-config")
    fi
    
    # 检查开发库依赖
    if [ "$is_debian_based" = true ]; then
        # Debian/Ubuntu/Deepin 系统：检查 deb 包
        local deb_dev_libs=(
            "libavcodec-dev"
            "libavfilter-dev"
            "libavformat-dev"
            "libavutil-dev"
            "libswresample-dev"
            "libswscale-dev"
            "libncurses-dev"
            "libpulse-dev"
            "libcurl4-openssl-dev"
            "libpng-dev"
            "libjpeg-dev"
            "libxml2-dev"
            "libsqlite3-dev"
            "zlib1g-dev"
        )

        for lib in "${deb_dev_libs[@]}"; do
            if ! dpkg -l "$lib" 2>/dev/null | grep -q "^ii"; then
                missing_deps+=("$lib")
            fi
        done
    else
        # RPM 系统：检查 rpm 包
        local dev_libs=()
        if [ "$static_build" = "true" ]; then
            # 静态构建：FFmpeg 从源码编译，不需要 ffmpeg-devel
            dev_libs=(
                "ncurses-devel"
                "pulseaudio-libs-devel"
                "libpng-devel"
                "libjpeg-turbo-devel"
                "libxml2-devel"
                "libcurl-devel"
                "sqlite-devel"
                "zlib-devel"
            )
        else
            dev_libs=(
                "ffmpeg-free-devel"
                "ncurses-devel"
                "pulseaudio-libs-devel"
                "libpng-devel"
                "libjpeg-turbo-devel"
                "libxml2-devel"
                "libcurl-devel"
                "sqlite-devel"
                "zlib-devel"
            )
        fi

        for lib in "${dev_libs[@]}"; do
            if ! rpm -q "$lib" &> /dev/null; then
                # 尝试替代包名（不同发行版可能有不同的包名）
                case "$lib" in
                    ffmpeg-free-devel)
                        if ! rpm -q "ffmpeg-devel" &> /dev/null; then
                            missing_deps+=("ffmpeg-free-devel 或 ffmpeg-devel")
                        fi
                        ;;
                    pulseaudio-libs-devel)
                        if ! rpm -q "libpulse-devel" &> /dev/null; then
                            missing_deps+=("pulseaudio-libs-devel 或 libpulse-devel")
                        fi
                        ;;
                    libpng-devel)
                        if ! rpm -q "libpng-devel" &> /dev/null && ! rpm -q "libpng16-devel" &> /dev/null; then
                            missing_deps+=("libpng-devel 或 libpng16-devel")
                        fi
                        ;;
                    libjpeg-turbo-devel)
                        if ! rpm -q "libjpeg-devel" &> /dev/null && ! rpm -q "libjpeg62-turbo-devel" &> /dev/null; then
                            missing_deps+=("libjpeg-turbo-devel 或 libjpeg-devel 或 libjpeg62-turbo-devel")
                        fi
                        ;;
                    *)
                        missing_deps+=("$lib")
                        ;;
                esac
            fi
        done
    fi
    
    # 检查可选依赖（用于生成 debuginfo）
    if ! command -v eu-strip &> /dev/null; then
        optional_missing+=("elfutils")
    fi
    
    if [ ${#missing_deps[@]} -gt 0 ]; then
        log_error "缺少以下必需的构建工具:"
        for dep in "${missing_deps[@]}"; do
            echo "  - $dep"
        done
        echo ""
        log_error "请使用以下命令安装缺失的工具:"
        if [ "$is_debian_based" = true ]; then
            echo "  sudo apt install ${missing_deps[*]}"
        else
            echo "  sudo dnf install ${missing_deps[*]}"
        fi
        exit 1
    fi
    
    if [ ${#optional_missing[@]} -gt 0 ]; then
        log_error "警告：缺少以下可选工具（debuginfo 包将无法生成）:"
        for dep in "${optional_missing[@]}"; do
            echo "  - $dep"
        done
        echo ""
        log_info "如需生成 debuginfo 包，请安装:"
        if [ "$is_debian_based" = true ]; then
            echo "  sudo apt install ${optional_missing[*]}"
        else
            echo "  sudo dnf install ${optional_missing[*]}"
        fi
        echo ""
        read -p "是否继续构建（不生成 debuginfo）？[Y/n] " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]] && [ -n "$REREPLY" ]]; then
            exit 1
        fi
    fi
    
    log_info "所有构建依赖已满足"
}

detect_version() {
    local version="$DEFAULT_VERSION"

    if [ -d "${SCRIPT_DIR}/.git" ] && command -v git >/dev/null 2>&1; then
        local git_version=$(git describe --tags --abbrev=0 2>/dev/null || true)
        if [[ $git_version =~ ^v?([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
            version="${BASH_REMATCH[1]}"
            echo "$version"
            return
        fi
    fi

    if [ -f "${SCRIPT_DIR}/include/org.yxzl.ter-music/types.h" ]; then
        local match
        match=$(grep -E 'APP_VERSION' "${SCRIPT_DIR}/include/org.yxzl.ter-music/types.h" | head -1)
        if [[ $match =~ ([0-9]+\.[0-9]+\.[0-9]+) ]]; then
            version="${BASH_REMATCH[1]}"
            echo "$version"
            return
        fi
    fi

    echo "$version"
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

prepare_directories() {
    local target_arch="$1"
    log_info "准备构建目录..."
    
    mkdir -p "${OUTPUT_DIR}/${target_arch}"
    
    rm -rf "${TEMP_DIR}"
    mkdir -p "${TEMP_DIR}"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
    
    log_clean "已清理并创建构建目录"
}

generate_spec_file() {
    local version="$1"
    local no_debuginfo="$2"
    local target_arch="$3"
    local static_build="${4:-false}"
    local spec_file="${TEMP_DIR}/SPECS/${PROJECT_NAME}.spec"

    log_info "生成 RPM spec file (目标架构: $target_arch)..."

    # 根据是否禁用 debuginfo 设置宏
    local debuginfo_macro=""
    if [ "$no_debuginfo" = "true" ]; then
        debuginfo_macro="%global debug_package %{nil}"
    fi

    # 生成 spec（已不再需要交叉编译工具链）
    local cmake_extra_args=""
    local ffmpeg_build_requires=""
    if [ "$static_build" = "true" ]; then
        cmake_extra_args="-DSTATIC_LINKING=ON"
        # 静态构建时 FFmpeg 从源码编译，移除 spec 中的 BuildRequires
        ffmpeg_build_requires="# FFmpeg built from source (static)"
    else
        ffmpeg_build_requires="BuildRequires:  pkgconfig(libavcodec)
BuildRequires:  pkgconfig(libavformat)
BuildRequires:  pkgconfig(libavutil)
BuildRequires:  pkgconfig(libswresample)
BuildRequires:  pkgconfig(libavfilter)
BuildRequires:  pkgconfig(libswscale)"
    fi

    cat > "$spec_file" << EOF
${debuginfo_macro}
Name:           ${PROJECT_NAME}
Version:        ${version}
Release:        1%{?dist}
Summary:        A terminal-based music player with ncurses interface
License:        GPL-3.0-or-later
URL:            https://github.com/HuanSoft-Open-Source-Community/ter-music
Source0:        %{name}-%{version}.tar.gz

# Target Architecture: ${target_arch}

BuildRequires:  gcc, make, cmake, pkg-config
BuildRequires:  pkgconfig(libcurl)
${ffmpeg_build_requires}
BuildRequires:  pkgconfig(libxml-2.0)
BuildRequires:  pkgconfig(libpulse) >= 10.0
BuildRequires:  pkgconfig(ncursesw) >= 6.0
BuildRequires:  pkgconfig(libpng) >= 1.6
BuildRequires:  pkgconfig(libjpeg)
BuildRequires:  pkgconfig(dbus-1) >= 1.0
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  pkgconfig(zlib)

# Runtime library dependencies are auto-generated by rpmbuild from soname
# (libavcodec.so.60()(64bit), libpulse.so.0()(64bit), etc.),
# which works across Fedora and RHEL without manual package name mapping.

%description
Ter-Music is a lightweight terminal-based music player for Linux systems.
It uses FFmpeg for audio decoding, PulseAudio for audio output, and ncursesw
for a beautiful text user interface.

Features:
- Support for multiple audio formats (MP3, WAV, FLAC, OGG, M4A, AAC, WMA, APE, OPUS)
- LRC lyrics synchronization display
- Multiple playback modes (sequential, single loop, list loop, random)
- Playlist management
- Favorites collection
- Playback history
- Customizable color themes
- Keyboard shortcuts
- Real-time progress bar

%prep
%setup -q

%build
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release ${cmake_extra_args}
make %{?_smp_mflags}

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}%{_bindir}
mkdir -p %{buildroot}%{_datadir}/applications
mkdir -p %{buildroot}%{_datadir}/icons/hicolor/32x32/apps
mkdir -p %{buildroot}%{_datadir}/icons/hicolor/48x48/apps
mkdir -p %{buildroot}%{_datadir}/icons/hicolor/128x128/apps
mkdir -p %{buildroot}%{_datadir}/icons/hicolor/scalable/apps
cd build
install -m 755 %{name} %{buildroot}%{_bindir}/%{name}
install -m 644 ../data/applications/%{name}.desktop %{buildroot}%{_datadir}/applications/%{name}.desktop
install -m 644 ../resources/icons/hicolor/32x32/apps/%{name}.png %{buildroot}%{_datadir}/icons/hicolor/32x32/apps/%{name}.png
install -m 644 ../resources/icons/hicolor/48x48/apps/%{name}.png %{buildroot}%{_datadir}/icons/hicolor/48x48/apps/%{name}.png
install -m 644 ../resources/icons/hicolor/128x128/apps/%{name}.png %{buildroot}%{_datadir}/icons/hicolor/128x128/apps/%{name}.png
install -m 644 ../resources/icons/hicolor/scalable/apps/%{name}.svg %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/%{name}.svg
mkdir -p %{buildroot}%{_datadir}/ter-music/help
install -m 644 ../data/help/help-quickstart-zh_CN.txt %{buildroot}%{_datadir}/ter-music/help/help-quickstart-zh_CN.txt
install -m 644 ../data/help/help-quickstart-en_US.txt %{buildroot}%{_datadir}/ter-music/help/help-quickstart-en_US.txt

%files
%{_bindir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.*
%{_datadir}/ter-music/*

%changelog
* $(LC_ALL=C date +'%a %b %d %Y') Packager <packager@example.com> - ${version}-1
- Initial package
EOF

    log_info "Spec 文件已生成: $spec_file"
}

create_source_tarball() {
    local version="$1"
    local tarball_name="${PROJECT_NAME}-${version}.tar.gz"
    local tarball_path="${TEMP_DIR}/SOURCES/${tarball_name}"
    
    log_info "创建源码压缩包..."
    
    if [ -d "${SCRIPT_DIR}/.git" ] && command -v git >/dev/null 2>&1 && [ "${SKIP_GIT_ARCHIVE:-0}" != "1" ]; then
        git -C "${SCRIPT_DIR}" archive --format=tar.gz --prefix="${PROJECT_NAME}-${version}/" HEAD > "$tarball_path"
        if [ $? -eq 0 ]; then
            log_info "源码压缩包已创建: $tarball_path (使用 git archive)"
            return
        fi
        log_info "git archive 失败，回退到手动复制..."
    fi
    
    local source_dir="${TEMP_DIR}/source_package"
    local package_dir="${source_dir}/${PROJECT_NAME}-${version}"
    
    rm -rf "$source_dir"
    mkdir -p "$package_dir"
    
    local files=("src" "include" "data" "resources" "cmake" "CMakeLists.txt" "docs/README.md" "LICENSE")
    local missing_files=()
    
    for file in "${files[@]}"; do
        if [ -e "${SCRIPT_DIR}/${file}" ]; then
            cp -r "${SCRIPT_DIR}/${file}" "$package_dir/"
        else
            missing_files+=("$file")
        fi
    done
    
    if [ ${#missing_files[@]} -gt 0 ]; then
        log_error "缺少必要的源文件: ${missing_files[*]}"
        return 1
    fi
    
    tar -czf "$tarball_path" -C "$source_dir" "${PROJECT_NAME}-${version}"
    
    log_info "源码压缩包已创建: $tarball_path"
}

build_rpm() {
    local target_arch="$1"
    local static_build="${2:-false}"
    log_info "开始构建 RPM 包 (目标架构: $target_arch)..."
    
    export RPM_TOPDIR="${TEMP_DIR}"
    
    # 检测是否为 Debian/Ubuntu/Deepin 系统
    local is_debian_based=false
    if [ -f /etc/debian_version ] || command -v dpkg &> /dev/null; then
        is_debian_based=true
    fi
    
    # 构建 rpmbuild 命令参数
    local rpmbuild_args=(
        -ba "${TEMP_DIR}/SPECS/${PROJECT_NAME}.spec"
        --define "_topdir ${TEMP_DIR}"
        --define "_sourcedir ${TEMP_DIR}/SOURCES"
        --define "_specdir ${TEMP_DIR}/SPECS"
        --define "_builddir ${TEMP_DIR}/BUILD"
        --define "_rpmdir ${TEMP_DIR}/RPMS"
        --define "_srcrpmdir ${TEMP_DIR}/SRPMS"
        --target "$target_arch"
    )
    
    # 静态构建或 Debian 系统上跳过 RPM 依赖检查
    # 静态构建：环境由 Dockerfile 保证；Debian：手动检查了 deb 包
    if [ "$static_build" = "true" ] || [ "$is_debian_based" = true ]; then
        log_info "使用 --nodeps 跳过 RPM 依赖检查"
        rpmbuild_args+=(--nodeps)
    fi
    
    # 执行 rpmbuild 命令并检查退出状态
    if rpmbuild "${rpmbuild_args[@]}"; then
        log_info "RPM 包构建完成"
        return 0
    else
        log_error "RPM 包构建失败"
        return 1
    fi
}

fix_ownership() {
    if [ -z "${HOST_UID:-}" ] || [ -z "${HOST_GID:-}" ]; then
        log_info "HOST_UID/HOST_GID 未设置，跳过文件所有权修复"
        return 0
    fi

    # Non-root users cannot chown (EPERM). When the container runs with
    # --user mapping, files already belong to the correct UID — skip chown
    # and just ensure readable/writable permissions instead.
    if [ "$(id -u)" != "0" ]; then
        log_info "非 root 用户，跳过 chown，确保文件权限可读写..."
        if [ -d "${OUTPUT_DIR}" ]; then
            chmod -R u+rwX "${OUTPUT_DIR}" 2>/dev/null || true
        fi
        local release_dir="${SCRIPT_DIR}/build/release"
        if [ -d "$release_dir" ]; then
            chmod -R u+rwX "$release_dir" 2>/dev/null || true
        fi
        if [ -d "${TEMP_DIR}" ]; then
            chmod -R u+rwX "${TEMP_DIR}" 2>/dev/null || true
        fi
        return 0
    fi

    log_info "修复文件所有权为 ${HOST_UID}:${HOST_GID}..."

    local ok=0
    local fail=0

    if chown -R "${HOST_UID}:${HOST_GID}" "${OUTPUT_DIR}"; then
        ok=$((ok + 1))
    else
        log_error "chown 失败: ${OUTPUT_DIR}"
        fail=$((fail + 1))
    fi

    local release_dir="${SCRIPT_DIR}/build/release"
    if [ -d "$release_dir" ]; then
        if chown -R "${HOST_UID}:${HOST_GID}" "$release_dir"; then
            ok=$((ok + 1))
        else
            log_error "chown 失败: $release_dir"
            fail=$((fail + 1))
        fi
    fi

    if [ -d "${TEMP_DIR}" ]; then
        if chown -R "${HOST_UID}:${HOST_GID}" "${TEMP_DIR}"; then
            ok=$((ok + 1))
        else
            log_error "chown 失败: ${TEMP_DIR}"
            fail=$((fail + 1))
        fi
    fi

    if [ "$fail" -gt 0 ]; then
        log_warn "文件所有权修复: $ok 成功, $fail 失败"
    else
        log_info "文件所有权修复完成 ($ok 个目录)"
    fi
}

collect_results() {
    local target_arch="$1"
    log_info "收集构建结果..."
    
    local found_rpms=0
    local rpm_files=()
    local output_dir="${OUTPUT_DIR}/${target_arch}"
    
    # 收集所有 RPM 包
    while IFS= read -r -d '' rpm_file; do
        rpm_files+=("$rpm_file")
    done < <(find "${TEMP_DIR}/RPMS" "${TEMP_DIR}/SRPMS" -name "*.rpm" -type f -print0)
    
    for rpm_file in "${rpm_files[@]}"; do
        cp "$rpm_file" "${output_dir}/"
        local filename=$(basename "$rpm_file")
        log_info "已复制: $filename -> ${output_dir}/"
        # 同时复制到 release 目录
        copy_to_release "$rpm_file"
        ((found_rpms++))
    done
    
    if [ $found_rpms -eq 0 ]; then
        log_error "未找到任何构建好的 RPM 包"
        return 1
    fi
    
    log_info "构建结果已收集到: ${output_dir}/"
    return 0
}

cleanup() {
    local keep_temp="$1"
    
    if [ "$keep_temp" != "true" ]; then
        log_clean "清理临时文件..."
        # Ensure all temp files are writable before removal (some may be
        # created by rpmbuild/fakeroot with restrictive permissions)
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
    local output_dir="${OUTPUT_DIR}/${target_arch}"
    
    echo ""
    echo "=========================================="
    echo "RPM 构建完成！"
    echo "=========================================="
    echo ""
    echo "目标架构: $target_arch"
    echo "输出目录: ${output_dir}/"
    echo ""
    echo "生成的 RPM 包:"
    ls -lh "${output_dir}"/*.rpm 2>/dev/null || echo "  未找到 RPM 包"
    echo ""
    echo "安装命令:"
    echo "  sudo dnf install ${output_dir}/${PROJECT_NAME}-*.rpm"
    echo ""
}

main() {
    local version="$DEFAULT_VERSION"
    local version_explicitly_set=false
    local keep_temp="false"
    local no_debuginfo="true"
    local target_arch=""
    local use_container="false"
    local use_static="false"
    local el_version="9"

    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -v|--version)
                version="$2"
                version_explicitly_set=true
                shift 2
                ;;
            -k|--keep-temp)
                keep_temp="true"
                shift
                ;;
            -a|--arch)
                target_arch="$2"
                shift 2
                ;;
            --no-debuginfo)
                no_debuginfo="true"
                shift
                ;;
            --with-debuginfo)
                no_debuginfo="false"
                shift
                ;;
            --container)
                use_container="true"
                shift
                ;;
            --el-version)
                el_version="$2"
                if [ "$el_version" != "8" ] && [ "$el_version" != "9" ] && [ "$el_version" != "10" ]; then
                    log_error "不支持的 EL 版本: $el_version（仅支持 8、9 或 10）"
                    exit 1
                fi
                shift 2
                ;;
            --static)
                use_static="true"
                use_container="true"
                shift
                ;;
            --static-build)
                use_static="true"
                # 容器内部调用时使用，不再触发 --container 逻辑
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
    echo "Ter-Music RPM 构建脚本"
    echo "=========================================="
    echo ""

    log_info "构建环境信息:"
    log_info "  操作系统: $(uname -s)"
    log_info "  内核版本: $(uname -r)"
    log_info "  主机架构: $(uname -m)"
    echo ""

    if [ "$version_explicitly_set" = "false" ]; then
        version=$(detect_version)
        log_info "自动检测到版本: $version"
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

    # 容器构建模式：委托给 docker-build.sh
    if [ "$use_container" = "true" ]; then
        if ! command -v docker &> /dev/null; then
            log_error "Docker 未安装，无法使用容器构建模式"
            log_info "请先安装 Docker：sudo apt install docker.io"
            exit 1
        fi

        local dockerfile image_name build_args=()
        if [ "$use_static" = "true" ]; then
            log_info "进入容器构建模式（静态链接，单包兼容 EL8/9/10）..."
            dockerfile="scripts/docker/Dockerfile.rpm-static"
            image_name="ter-music-rpm-static"
        else
            log_info "进入容器构建模式（Rocky Linux ${el_version}）..."
            dockerfile="scripts/docker/Dockerfile.rpm"
            image_name="ter-music-rpm-el${el_version}"
            build_args=(--build-arg "EL_VERSION=${el_version}")
        fi

        local xb_args=(
            -s "build-rpm.sh"
            -f "$dockerfile"
            -n "$image_name"
        )

        # 透传 build-arg
        for arg in "${build_args[@]}"; do
            xb_args+=("$arg")
        done

        # 构造传递给内部 build-rpm.sh 的参数
        local inner_args=()
        inner_args+=("-a" "$target_arch")
        inner_args+=("-v" "$version")
        [ "$use_static" = "true" ] && inner_args+=(--static-build)
        [ "$no_debuginfo" = "false" ] && inner_args+=(--with-debuginfo)
        [ "$keep_temp" = "true" ] && inner_args+=(--keep-temp)
        if [ ${#inner_args[@]} -gt 0 ]; then
            xb_args+=("--" "${inner_args[@]}")
        fi

        log_info "委托给 docker-build.sh: ${xb_args[*]}"
        exec "${SCRIPT_DIR}/scripts/docker/docker-build.sh" "${xb_args[@]}"
    fi

    check_dependencies "$target_arch" "$use_static"

    prepare_directories "$target_arch"
    generate_spec_file "$version" "$no_debuginfo" "$target_arch" "$use_static"
    if ! create_source_tarball "$version"; then
        log_error "创建源码压缩包失败"
        cleanup "$keep_temp"
        exit 1
    fi

    if build_rpm "$target_arch" "$use_static"; then
        if collect_results "$target_arch"; then
            fix_ownership
            cleanup "$keep_temp"
            show_summary "$target_arch"
        else
            log_error "收集构建结果失败"
            cleanup "$keep_temp"
            exit 1
        fi
    else
        log_error "RPM 构建过程失败"
        cleanup "$keep_temp"
        exit 1
    fi
}

main "$@"
