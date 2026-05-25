#!/bin/bash

set -e

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
SCRIPT_DIR="$(pwd)"
PROJECT_NAME="ter-music"
DEFAULT_VERSION="1.0.0"
OUTPUT_DIR="${SCRIPT_DIR}/build/deb"
TEMP_DIR="${SCRIPT_DIR}/.debbuild_temp"
SOURCE_OUTPUT_DIR="${OUTPUT_DIR}/source"

log_info()  { echo "[INFO] $1"; }
log_error() { echo "[ERROR] $1" >&2; }
log_clean() { echo "[CLEAN] $1"; }
log_warn()  { echo "[WARN] $1" >&2; }

copy_to_release() {
    local source_file="$1"
    local release_dir="${SCRIPT_DIR}/build/release"
    if [ -f "$source_file" ]; then
        mkdir -p "$release_dir"
        cp "$source_file" "$release_dir/"
        log_info "构建结果已复制到: ${release_dir}/$(basename "$source_file")"
    fi
}

is_cross_compiling() {
    local host_arch=$(uname -m)
    local target_arch="$1"
    local normalized_host="$host_arch"
    local normalized_target="$target_arch"
    [ "$normalized_host" = "x86_64" ] && normalized_host="amd64"
    [ "$normalized_target" = "x86_64" ] && normalized_target="amd64"
    [ "$normalized_host" = "aarch64" ] && normalized_host="arm64"
    [ "$normalized_target" = "aarch64" ] && normalized_target="arm64"
    [ "$normalized_host" != "$normalized_target" ]
}

get_cross_compile_prefix() {
    case "$1" in
        arm64|aarch64) echo "aarch64-linux-gnu" ;;
        *) echo "" ;;
    esac
}

check_cross_compile_deps() {
    local target_arch="$1"
    local arch_prefix=$(get_cross_compile_prefix "$target_arch")
    [ -z "$arch_prefix" ] && return 0

    log_info "检查交叉编译工具链..."
    local missing=()
    ! command -v ${arch_prefix}-gcc &>/dev/null && missing+=("gcc-${arch_prefix}")
    ! command -v ${arch_prefix}-g++ &>/dev/null && missing+=("g++-${arch_prefix}")
    ! command -v ${arch_prefix}-ar &>/dev/null && missing+=("binutils-${arch_prefix}")

    if [ ${#missing[@]} -gt 0 ]; then
        log_error "缺少交叉编译工具链: ${missing[*]}"
        echo "  sudo apt install ${missing[*]}"
        return 1
    fi
    log_info "交叉编译工具链检查通过"
}

setup_cross_compile_env() {
    local target_arch="$1"
    local arch_prefix=$(get_cross_compile_prefix "$target_arch")
    [ -z "$arch_prefix" ] && return 0

    log_info "设置交叉编译环境..."
    export CC=${arch_prefix}-gcc
    export CXX=${arch_prefix}-g++
    export AR=${arch_prefix}-ar
    export STRIP=${arch_prefix}-strip
    export LD=${arch_prefix}-ld
    export PKG_CONFIG_PATH=/usr/lib/${arch_prefix}/pkgconfig
    export PKG_CONFIG_LIBDIR=/usr/lib/${arch_prefix}/pkgconfig
}

show_help() {
    cat << EOF
用法: $0 [选项]

将 ter-music 项目打包成标准的 Debian DEB 包

选项:
    -h, --help              显示此帮助信息
    -v, --version VERSION   指定版本号（默认：自动检测）
    -a, --arch ARCH         指定目标架构（默认：自动检测）
    -k, --keep-temp         保留临时构建文件（用于调试）
    --with-source           同时生成源码包（默认不生成）
    --with-debuginfo        生成 debuginfo 包（默认不生成）
    --container             在 Docker 容器中构建 DEB（推荐方式）
    --debian-version VERSION 指定 Debian 版本: 10, 11, 12 或 13（默认 12，需配合 --container）
    --static                静态链接 FFmpeg，消除 soname 依赖，单包兼容多个 Debian 版本

支持的架构:
    amd64 arm64 loong64 loongarch64 sw64 mips64el

示例:
    $0                                    # 自动检测版本和架构构建 DEB
    $0 -v 1.2.3 -a arm64                 # 指定版本和架构
    $0 --container                        # 在容器中构建（推荐）
    $0 --container --debian-version 10    # 在 Debian 10 容器中构建
    $0 --static                           # 静态链接 FFmpeg，跨版本兼容
    $0 --with-source --with-debuginfo     # 同时生成源码包和 debuginfo 包

EOF
}

check_dependencies() {
    local target_arch="${1:-}"
    log_info "检查构建依赖..."

    local missing=()

    if ! command -v dpkg-buildpackage &>/dev/null; then
        missing+=("dpkg-dev")
    fi
    if ! command -v fakeroot &>/dev/null; then
        missing+=("fakeroot")
    fi
    if ! command -v dch &>/dev/null; then
        missing+=("devscripts")
    fi

    if [ -n "$target_arch" ] && is_cross_compiling "$target_arch"; then
        if ! check_cross_compile_deps "$target_arch"; then
            exit 1
        fi
    else
        ! command -v gcc &>/dev/null && missing+=("gcc")
        ! command -v make &>/dev/null && missing+=("make")
    fi

    ! command -v cmake &>/dev/null && missing+=("cmake")

    if [ ${#missing[@]} -gt 0 ]; then
        log_error "缺少以下构建工具:"
        printf '  - %s\n' "${missing[@]}"
        echo ""
        log_error "请安装: sudo apt install ${missing[*]}"
        exit 1
    fi
    log_info "所有构建依赖已满足"
}

detect_version() {
    local version="$DEFAULT_VERSION"

    if [ -d "${SCRIPT_DIR}/.git" ] && command -v git >/dev/null 2>&1; then
        local git_version=$(git describe --tags --abbrev=0 2>/dev/null || true)
        if [[ $git_version =~ ^v?([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
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
        fi
    fi

    echo "$version"
}

detect_architecture() {
    local arch=$(uname -m)
    case "$arch" in
        x86_64)         echo "amd64" ;;
        aarch64|arm64)  echo "arm64" ;;
        loongarch64)    echo "loongarch64" ;;
        loong64)        echo "loong64" ;;
        mips64)         echo "mips64el" ;;
        sw_64|sw64)     echo "sw64" ;;
        *)
            log_error "未知架构: $arch"
            return 1
            ;;
    esac
}

validate_architecture() {
    local arch="$1"
    for v in amd64 arm64 loong64 loongarch64 sw64 mips64el; do
        [ "$arch" = "$v" ] && return 0
    done
    log_error "不支持的架构: $arch"
    return 1
}

prepare_directories() {
    local target_arch="$1"
    log_info "准备构建目录..."
    mkdir -p "${OUTPUT_DIR}/${target_arch}" "${SOURCE_OUTPUT_DIR}"
    rm -rf "${TEMP_DIR}"
    mkdir -p "${TEMP_DIR}"
    log_clean "已清理并创建构建目录"
}

# Core build function: delegate to dpkg-buildpackage using the debian/ directory
build_via_dpkg() {
    local version="$1"
    local target_arch="$2"
    local build_source="$3"
    local build_debuginfo="$4"
    local static_build="${5:-false}"

    local build_root="${TEMP_DIR}/dpkgbuild"
    local source_dir="${build_root}/${PROJECT_NAME}-${version}"
    mkdir -p "$build_root"

    log_info "创建源码压缩包 (.orig.tar.gz)..."

    # Step 1: Create .orig.tar.gz via git archive or fallback
    local orig_tarball="${build_root}/${PROJECT_NAME}_${version}.orig.tar.gz"
    if [ -d "${SCRIPT_DIR}/.git" ] && command -v git >/dev/null 2>&1; then
        if ! git -C "${SCRIPT_DIR}" archive --format=tar.gz \
            --prefix="${PROJECT_NAME}-${version}/" \
            HEAD > "$orig_tarball" 2>/dev/null || [ ! -s "$orig_tarball" ]; then
            log_warn "git archive 失败，尝试添加 safe.directory..."
            git config --global --add safe.directory "${SCRIPT_DIR}" 2>/dev/null || true
            git -C "${SCRIPT_DIR}" archive --format=tar.gz \
                --prefix="${PROJECT_NAME}-${version}/" \
                HEAD > "$orig_tarball"
        fi
    else
        # Fallback: tar from file list (no git available)
        local fallback_dir="${TEMP_DIR}/fallback_source"
        rm -rf "$fallback_dir"
        mkdir -p "${fallback_dir}/${PROJECT_NAME}-${version}"
        cp -a "${SCRIPT_DIR}"/. "${fallback_dir}/${PROJECT_NAME}-${version}/"
        rm -rf "${fallback_dir}/${PROJECT_NAME}-${version}"/.git \
               "${fallback_dir}/${PROJECT_NAME}-${version}"/build \
               "${fallback_dir}/${PROJECT_NAME}-${version}"/.tmp \
               "${fallback_dir}/${PROJECT_NAME}-${version}"/.debbuild_temp 2>/dev/null || true
        tar -czf "$orig_tarball" -C "$fallback_dir" "${PROJECT_NAME}-${version}"
    fi
    log_info "源码压缩包已创建: $orig_tarball"

    # Step 2: Extract source
    cd "$build_root"
    tar xzf "$orig_tarball"

    # Step 3: Copy debian/ (catches uncommitted packaging changes)
    if [ ! -d "${SCRIPT_DIR}/debian" ]; then
        log_error "项目缺少 debian/ 目录"
        return 1
    fi
    rm -rf "${source_dir}/debian"
    cp -a "${SCRIPT_DIR}/debian" "${source_dir}/debian"

    # Step 5: Update changelog if version differs
    cd "${source_dir}"
    local changelog_version
    changelog_version=$(dpkg-parsechangelog -SVersion 2>/dev/null | cut -d- -f1 || echo "")
    if [ -n "$changelog_version" ] && [ "$version" != "$changelog_version" ]; then
        log_info "更新 changelog 版本: $changelog_version -> $version"
        dch -v "${version}-1" "Automated build: version ${version}"             --force-distribution --no-auto-nmu 2>/dev/null ||         log_warn "dch 更新版本失败，使用原有 changelog"
    fi

    # Step 6: Phase 1 — generate source package (BEFORE static patches)
    # Source package captures clean upstream to avoid dpkg-source detecting
    # modifications from static linking patches against the .orig.tar.gz
    if [ "$build_source" = "true" ]; then
        log_info "Phase 1: 生成源码包..."
        local src_dpkg_args=(--build=source --no-sign)

        if [ -n "$target_arch" ] && is_cross_compiling "$target_arch" ]; then
            local dpkg_arch
            case "$target_arch" in
                amd64)       dpkg_arch="amd64" ;;
                arm64)       dpkg_arch="arm64" ;;
                loong64)     dpkg_arch="loong64" ;;
                loongarch64) dpkg_arch="loongarch64" ;;
                sw64)        dpkg_arch="sw64" ;;
                mips64el)    dpkg_arch="mips64el" ;;
                *)           dpkg_arch="$target_arch" ;;
            esac
            src_dpkg_args+=(--host-arch "$dpkg_arch")
        fi

        if ! dpkg-buildpackage "${src_dpkg_args[@]}"; then
            log_error "源码包构建失败"
            return 1
        fi
        log_info "源码包生成完成"
    fi

    # Step 7: For static builds, patch debian/rules to add -DSTATIC_LINKING=ON
    # These patches modify CMakeLists.txt (upstream file), so they must be applied
    # AFTER source package generation to avoid dpkg-source detecting changes
    if [ "$static_build" = "true" ]; then
        log_info "启用静态链接模式（仅 FFmpeg 静态）..."
        sed -i 's|-DCMAKE_BUILD_TYPE=Release|-DCMAKE_BUILD_TYPE=Release -DSTATIC_LINKING=ON|'             "${source_dir}/debian/rules"
        # Remove FFmpeg dev packages from Build-Depends (static libs provided via pkg-config)
        sed -i '/^ libavcodec-dev,$/d; /^ libavfilter-dev,$/d; /^ libavformat-dev,$/d; /^ libavutil-dev,$/d; /^ libswresample-dev,$/d; /^ libswscale-dev,$/d'             "${source_dir}/debian/control"
        # Fix CMakeLists.txt: save/restore CMAKE_FIND_LIBRARY_SUFFIXES around FFmpeg block
        # so .a preference doesn't leak to ncurses/pthread find_library calls (prevents
        # _dl_pagesize and libtinfo undefined reference errors)
        sed -i '/^    set(CMAKE_FIND_LIBRARY_SUFFIXES /i\    set(_saved_find_library_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")'             "${source_dir}/CMakeLists.txt"
        # sed append order: the LAST sed's text appears CLOSEST after the match line
        # So unset runs first in time → its text ends up last → correct: set restore before unset
        sed -i '/^    unset(ENV{PKG_CONFIG_ARGN})$/a\    unset(_saved_find_library_suffixes)'             "${source_dir}/CMakeLists.txt"
        sed -i '/^    unset(ENV{PKG_CONFIG_ARGN})$/a\    set(CMAKE_FIND_LIBRARY_SUFFIXES "${_saved_find_library_suffixes}")'             "${source_dir}/CMakeLists.txt"
    fi

    # Step 8: Set DEB_BUILD_OPTIONS
    export DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-} nocheck"
    if [ "$build_debuginfo" != "true" ]; then
        export DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS} nostrip"
    fi

    # Step 9: Set up cross-compilation environment
    if [ -n "$target_arch" ] && is_cross_compiling "$target_arch"; then
        setup_cross_compile_env "$target_arch"
    fi

    # Step 10: Phase 2 — build binary package (with static patches applied)
    log_info "Phase 2: 构建二进制包..."
    local bin_dpkg_args=(--build=binary --no-sign)

    if [ -n "$target_arch" ] && is_cross_compiling "$target_arch" ]; then
        local dpkg_arch
        case "$target_arch" in
            amd64)       dpkg_arch="amd64" ;;
            arm64)       dpkg_arch="arm64" ;;
            loong64)     dpkg_arch="loong64" ;;
            loongarch64) dpkg_arch="loongarch64" ;;
            sw64)        dpkg_arch="sw64" ;;
            mips64el)    dpkg_arch="mips64el" ;;
            *)           dpkg_arch="$target_arch" ;;
        esac
        bin_dpkg_args+=(--host-arch "$dpkg_arch")
        log_info "交叉编译模式: $(uname -m) -> $dpkg_arch"
    fi

    if ! dpkg-buildpackage "${bin_dpkg_args[@]}"; then
        log_error "二进制包构建失败"
        return 1
    fi

    log_info "dpkg-buildpackage 构建完成"
    return 0
}

collect_results() {
    local version="$1"
    local target_arch="$2"
    local build_source="$3"
    local build_root="${TEMP_DIR}/dpkgbuild"
    local output_dir="${OUTPUT_DIR}/${target_arch}"

    log_info "收集构建结果..."
    mkdir -p "$output_dir"

    local found=0

    # Collect binary .deb packages
    while IFS= read -r -d '' deb; do
        cp "$deb" "$output_dir/"
        local name=$(basename "$deb")
        log_info "已复制: $name -> ${output_dir}/"
        copy_to_release "$deb"
        ((found++))
    done < <(find "${build_root}" -maxdepth 1 -name "*.deb" -type f -print0)

    # Collect source packages
    if [ "$build_source" = "true" ]; then
        mkdir -p "${SOURCE_OUTPUT_DIR}"
        local src_patterns=("*.dsc" "*.debian.tar.*" "*.orig.tar.*" "*.buildinfo" "*.changes")
        for pattern in "${src_patterns[@]}"; do
            while IFS= read -r -d '' f; do
                cp "$f" "${SOURCE_OUTPUT_DIR}/"
                log_info "已复制源码包: $(basename "$f")"
            done < <(find "${build_root}" -maxdepth 1 -name "$pattern" -type f -print0 2>/dev/null || true)
        done
    fi

    if [ $found -eq 0 ]; then
        log_error "未找到构建好的 DEB 包"
        return 1
    fi
    log_info "构建结果已收集到: ${output_dir}/"
    return 0
}

cleanup() {
    if [ "$1" != "true" ]; then
        log_clean "清理临时文件..."
        rm -rf "${TEMP_DIR}"
        log_clean "临时文件已清理"
    else
        log_info "保留临时文件: ${TEMP_DIR}"
    fi
}

fix_ownership() {
    if [ -z "${HOST_UID:-}" ] || [ -z "${HOST_GID:-}" ]; then
        log_info "HOST_UID/HOST_GID 未设置，跳过文件所有权修复"
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

show_summary() {
    local target_arch="$1"
    local output_dir="${OUTPUT_DIR}/${target_arch}"

    echo ""
    echo "=========================================="
    echo "DEB 构建完成！"
    echo "=========================================="
    echo ""
    echo "目标架构: $target_arch"
    echo "输出目录: ${output_dir}/"
    echo ""
    echo "生成的包:"
    ls -lh "${output_dir}"/*.deb 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}' || echo "  未找到 DEB 包"
    if [ -d "${SOURCE_OUTPUT_DIR}" ] && ls "${SOURCE_OUTPUT_DIR}"/*.dsc &>/dev/null 2>&1; then
        echo ""
        echo "源码包目录: ${SOURCE_OUTPUT_DIR}/"
        ls -lh "${SOURCE_OUTPUT_DIR}"/*.dsc 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'
    fi
    echo ""
    echo "安装命令:"
    echo "  sudo dpkg -i ${output_dir}/${PROJECT_NAME}_*_${target_arch}.deb"
    echo "  如果缺少依赖，请运行: sudo apt install -f"
    echo ""
}

main() {
    local version="$DEFAULT_VERSION"
    local keep_temp="false"
    local build_source="false"
    local build_debuginfo="false"
    local target_arch=""
    local use_container="false"
    local debian_version="12"
    local static_build="false"

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
            -k|--keep-temp)
                keep_temp="true"
                shift
                ;;
            --with-source)
                build_source="true"
                shift
                ;;
            --with-debuginfo)
                build_debuginfo="true"
                shift
                ;;
            --container)
                use_container="true"
                shift
                ;;
            --debian-version)
                debian_version="$2"
                shift 2
                ;;
            --static)
                static_build="true"
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
    echo "Ter-Music DEB 构建脚本"
    echo "=========================================="
    echo ""

    log_info "构建环境信息:"
    log_info "  操作系统: $(uname -s)"
    log_info "  内核版本: $(uname -r)"
    log_info "  主机架构: $(uname -m)"
    echo ""

    if [ "$version" = "$DEFAULT_VERSION" ]; then
        version=$(detect_version)
        log_info "检测到版本: $version"
    else
        log_info "使用指定版本: $version"
    fi

    if [ -z "$target_arch" ]; then
        target_arch=$(detect_architecture)
        log_info "自动检测到架构: $target_arch"
    else
        validate_architecture "$target_arch"
        log_info "使用指定架构: $target_arch"
    fi

    # Container build mode: delegate to cross-build.sh
    # Static builds always use container mode (Debian 10 for widest compat)
    if [ "$static_build" = "true" ]; then
        if command -v docker &>/dev/null; then
            log_info "进入静态构建模式（Debian 10 容器，FFmpeg 从源码编译）..."
            local dockerfile="scripts/cross-compile/Dockerfile.deb-static"
            local image_name="ter-music-deb-static"

            local xb_args=(
                -s "build-deb.sh"
                -f "$dockerfile"
                -n "$image_name"
                -a "$target_arch"
            )

            local inner_args=(--static)
            [ "$version" != "$DEFAULT_VERSION" ] && inner_args+=("-v" "$version")
            [ "$build_source" = "true" ] && inner_args+=(--with-source)
            [ "$build_debuginfo" = "true" ] && inner_args+=(--with-debuginfo)
            [ "$keep_temp" = "true" ] && inner_args+=(--keep-temp)
            if [ ${#inner_args[@]} -gt 0 ]; then
                xb_args+=("--" "${inner_args[@]}")
            fi

            log_info "委托给 cross-build.sh: ${xb_args[*]}"
            exec "${SCRIPT_DIR}/scripts/cross-compile/cross-build.sh" "${xb_args[@]}"
        else
            log_info "静态构建模式：Docker 不可用，直接构建（容器内环境）"
        fi
    fi

    if [ "$use_container" = "true" ]; then
        if ! command -v docker &>/dev/null; then
            log_error "Docker 未安装，无法使用容器构建模式"
            log_info "请先安装 Docker：sudo apt install docker.io"
            exit 1
        fi

        log_info "进入容器构建模式（Debian ${debian_version}）..."
        local dockerfile="scripts/cross-compile/Dockerfile.deb"
        local image_name="ter-music-deb"

        local xb_args=(
            -s "build-deb.sh"
            -f "$dockerfile"
            -n "$image_name"
            -a "$target_arch"
            --build-arg "DEBIAN_VERSION=${debian_version}"
        )

        local inner_args=()
        [ "$version" != "$DEFAULT_VERSION" ] && inner_args+=("-v" "$version")
        [ "$build_source" = "true" ] && inner_args+=(--with-source)
        [ "$build_debuginfo" = "true" ] && inner_args+=(--with-debuginfo)
        [ "$keep_temp" = "true" ] && inner_args+=(--keep-temp)
        if [ ${#inner_args[@]} -gt 0 ]; then
            xb_args+=("--" "${inner_args[@]}")
        fi

        log_info "委托给 cross-build.sh: ${xb_args[*]}"
        exec "${SCRIPT_DIR}/scripts/cross-compile/cross-build.sh" "${xb_args[@]}"
    fi

    check_dependencies "$target_arch"
    prepare_directories "$target_arch"

    local output_files=()

    if build_via_dpkg "$version" "$target_arch" "$build_source" "$build_debuginfo" "$static_build"; then
        collect_results "$version" "$target_arch" "$build_source"
        fix_ownership
        cleanup "$keep_temp"
        show_summary "$target_arch"
    else
        log_error "DEB 构建过程失败"
        cleanup "$keep_temp"
        exit 1
    fi
}

main "$@"
