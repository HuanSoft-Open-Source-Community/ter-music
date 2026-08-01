#!/bin/bash

set -e

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
SCRIPT_DIR="$(pwd)"
PROJECT_NAME="ter-music"
APP_ID="org.yxzl.ter-music"
OUTPUT_DIR="${SCRIPT_DIR}/build/linyaps"
TEMP_DIR="${SCRIPT_DIR}/.linyaps_temp"

# 确保路径是绝对路径的函数
ensure_absolute_path() {
    local path="$1"
    if [[ "$path" != /* ]]; then
        path="$(pwd)/$path"
    fi
    echo "$path"
}

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

直接从源码构建 ter-music Linyaps（如意玲珑）软件包

选项:
    -h, --help          显示此帮助信息
    -v, --version VERSION  指定版本号（默认：自动检测）
    -a, --arch ARCH     指定目标架构（默认：自动检测）
    -k, --keep-temp     保留临时构建文件（用于调试）
    --in-container     在 Docker 容器内运行（跳过依赖检查）

示例:
    $0                  使用自动检测版本和架构构建 Linyaps 包
    $0 -v 1.1.2         使用指定版本号构建 Linyaps 包
    $0 --keep-temp      构建后保留临时文件

输出:
    Linyaps 包将输出到: ${OUTPUT_DIR}/<arch>/

EOF
}

check_dependencies() {
    local target_arch="${1:-}"
    
    log_info "检查构建依赖..."

    local missing_deps=()

    if ! command -v ll-builder &> /dev/null; then
        missing_deps+=("linglong-builder (ll-builder)")
    fi

    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi

    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
    fi

    if [ ${#missing_deps[@]} -gt 0 ]; then
        log_error "缺少以下构建工具:"
        for dep in "${missing_deps[@]}"; do
            echo "  - $dep"
        done
        echo ""
        log_error "请使用以下命令安装缺失的工具:"
        echo "  Debian/Ubuntu 系: sudo apt install linglong-builder cmake make"
        echo "  RPM 系 (Fedora/openEuler): sudo dnf install linglong-builder cmake make"
        exit 1
    fi

    log_info "所有构建依赖已满足"
}

detect_version() {
    local default_version="2.1.0"

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

prepare_linyaps_structure() {
    local temp_dir="$1"
    local app_id="$2"

    log_info "准备 Linyaps 项目目录结构..."

    PROJECT_ROOT_OUTPUT="${temp_dir}/${app_id}"

    mkdir -p "$PROJECT_ROOT_OUTPUT/${PROJECT_NAME}"

    log_info "复制源码到构建目录..."
    rsync -a --exclude=.git --exclude=build --exclude=.linyaps_temp \
          "${SCRIPT_DIR}/" "${PROJECT_ROOT_OUTPUT}/${PROJECT_NAME}/"

    log_info "目录结构创建完成"
}

generate_linglong_yaml() {
    local project_root="$1"
    local app_id="$2"
    local version="$3"
    local target_arch="$4"

    log_info "生成 linglong.yaml 配置文件..."

    # Linyaps要求版本号为四段式，如果用户提供的是三段式，添加一个.0
    local linyaps_version="$version"
    if [[ "$linyaps_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        linyaps_version="${linyaps_version}.0"
    fi

    cat > "${project_root}/linglong.yaml" << EOF
# SPDX-License-Identifier: LGPL-3.0-or-later
version: "1"

package:
  id: ${app_id}
  name: ${PROJECT_NAME}
  version: ${linyaps_version}
  kind: app
  description: |
    ter-music 是一个基于 ncurses 的轻量级终端音乐播放器
  architecture: ${target_arch}

command:
  - /opt/apps/${app_id}/files/bin/${PROJECT_NAME}

base: org.deepin.base/25.2.2

sources:
  - kind: file
    name: ${PROJECT_NAME}
    url: ./${PROJECT_NAME}

build: |
  cd ${PROJECT_NAME}
  mkdir -p build
  cd build
  cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=\${PREFIX} -DCMAKE_INSTALL_RPATH='\$ORIGIN/../lib'
  make -j\$(nproc)
  make install
  mkdir -p \${PREFIX}/lib
  for dep in libblas.so.3 liblapack.so.3 libmpg123.so.0; do
    dep_path=\$(find /usr/lib /lib \\( -type f -o -type l \\) -name "\${dep}" 2>/dev/null | head -n 1)
    if [ -z "\${dep_path}" ]; then
      echo "required runtime library not found: \${dep}" >&2
      exit 1
    fi
    install -Dm755 "\$(readlink -f "\${dep_path}")" "\${PREFIX}/lib/\${dep}"
  done

buildext:
  apt:
    build_depends:
      - build-essential
      - cmake
      - pkg-config
      - libncurses-dev
      - libavformat-dev
      - libavcodec-dev
      - libswresample-dev
      - libtag1-dev
      - libpulse-dev
      - libavutil-dev
      - libavdevice-dev
      - libavfilter-dev
      - libswscale-dev
      - libpostproc-dev
      - libblas3
      - liblapack3
      - libpng-dev
      - libjpeg-dev
      - libxml2-dev
      - libcurl4-openssl-dev
      - libsqlite3-dev
      - libdbus-1-dev
      - libasound2-dev
    depends:
      - libavformat60
      - libavcodec60
      - libswresample4
      - libavutil58
      - libavfilter9
      - libtag1v5
      - libpulse0
      - libblas3
      - liblapack3
      - libgfortran5
      - libpng16-16
      - libjpeg62-turbo
      - libxml2
      - libcurl4
      - libswscale7
      - libmpg123-0
      - libvorbis0a
      - libvorbisenc2
      - libvorbisfile3
      - libopus0
      - libtheora0
      - libx264-160
      - libx265-199
      - libvpx7
      - libzstd1
      - liblzma5
EOF

    log_info "linglong.yaml 已生成: ${project_root}/linglong.yaml"
}

build_linyaps() {
    local project_root="$1"

    log_info "执行 ll-builder 构建..."
    cd "$project_root"

    # 捕获 ll-builder 输出以便分析失败原因
    local build_log="${TEMP_DIR}/ll-builder-output.log"

    # 运行 ll-builder（保留 stderr 以显示构建过程）
    # 使用 --skip-fetch-source 因为源码已在本地，无需从网络拉取
    set +e
    ll-builder build --skip-fetch-source 2>&1 | tee "$build_log"
    local rc=${PIPESTATUS[0]}
    set -e

    if [ $rc -eq 0 ]; then
        log_info "Linyaps 容器构建完成"
        return 0
    fi

    # 构建返回非零，分析失败原因：
    # Docker 容器内 OverlayFS 无法嵌套挂载，Runtime Check 阶段会失败，
    # 但编译、安装和提交可能已经成功。区分"编译失败"和"Runtime Check 失败"。
    # 注意：ll-builder 输出的 [Commit Contents] 区块与 committing/complete
    # 位于不同行（grep 逐行匹配），不能要求同一行内共存。
    if grep -q '\[Commit Contents\]' "$build_log" 2>/dev/null && \
       grep -q 'committing' "$build_log" 2>/dev/null && \
       grep -qE 'Runtime check failed|stage runtime check error|OverlayFS mount failed' "$build_log" 2>/dev/null; then
        log_warn "编译和提交成功，但 Runtime Check 失败（Docker 内 OverlayFS 限制，可忽略）"
        log_warn "继续执行 UAB 导出..."
        return 0
    fi

    log_error "Linyaps 构建失败"
    return 1
}

export_uab() {
    local project_root="$1"
    local output_dir="$2"
    local app_id="$3"
    local version="$4"
    local target_arch="$5"

    log_info "导出 UAB 格式包..."

    # Linyaps 会自动处理版本号，不需要我们添加四段式到输出文件名
    local linyaps_version="$version"
    if [[ "$linyaps_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        linyaps_version="${linyaps_version}.0"
    fi

    mkdir -p "$output_dir"

    # 导出 UAB 需要 --ref 参数
    # 必须使用绝对路径，ll-builder 不支持相对路径
    local temp_uab="${project_root}/output.uab"
    local final_uab="${output_dir}/${app_id}_${version}_${target_arch}.uab"
    local ref="main:${app_id}/${linyaps_version}/${target_arch}"

    # 确保使用绝对路径（ll-builder 要求）
    temp_uab=$(cd "$project_root" && pwd)/output.uab
    final_uab=$(cd "$output_dir" && pwd)/${app_id}_${version}_${target_arch}.uab
    output_dir=$(cd "$output_dir" && pwd)

    # 分别导出 UAB 和 layer，两者都需成功；UAB 缺失即判失败
    local uab_ok=false
    local layer_ok=false

    # ── 1) 导出 UAB ──
    log_info "导出 UAB 格式包..."
    if ll-builder export --ref "$ref" -o "$temp_uab" && [ -s "$temp_uab" ]; then
        mv "$temp_uab" "$final_uab"
        chmod 644 "$final_uab" 2>/dev/null || true
        log_info "UAB 包导出完成: $final_uab"
        copy_to_release "$final_uab"
        EXPORTED_UAB_FILE="$final_uab"
        uab_ok=true
    else
        log_error "UAB 导出失败"
        rm -f "$temp_uab"
    fi

    # ── 2) 导出 layer ──
    log_info "导出 layer 格式包..."
    if ll-builder export --layer; then
        local layer_file
        layer_file=$(find "$project_root" -maxdepth 1 -name "*_binary.layer" | head -1)
        if [ -n "$layer_file" ]; then
            local final_layer
            final_layer="$(cd "$output_dir" && pwd)/$(basename "$layer_file")"
            mv "$layer_file" "$final_layer"
            chmod 644 "$final_layer" 2>/dev/null || true
            log_info "layer 包导出完成: $final_layer"
            copy_to_release "$final_layer"
            EXPORTED_LAYER_FILE="$final_layer"
            layer_ok=true
        fi
    fi
    if [ "$layer_ok" = false ]; then
        log_error "layer 导出失败"
    fi

    # UAB 必须成功；UAB 缺失即使 layer 已成功也判 linyaps 任务失败
    if [ "$uab_ok" = false ]; then
        log_error "UAB 导出失败，linyaps 任务判定为失败（即使 layer 可能已成功）"
        return 1
    fi

    return 0
}

# ── 修复容器内构建产物的所有权 ────────────────────────────
# Docker 容器以 root 运行时产物属于 root，通过 chown 恢复为宿主用户
fix_output_ownership() {
    if [ -z "${HOST_UID:-}" ] || [ -z "${HOST_GID:-}" ]; then
        return 0
    fi

    # Non-root users cannot chown; just ensure permissions are sane
    if [ "$(id -u)" != "0" ]; then
        log_info "非 root 用户，确保构建产物权限可读写..."
        if [ -d "${OUTPUT_DIR}" ]; then
            chmod -R u+rwX "${OUTPUT_DIR}" 2>/dev/null || true
        fi
        if [ -d "${SCRIPT_DIR}/build/release" ]; then
            chmod -R u+rwX "${SCRIPT_DIR}/build/release" 2>/dev/null || true
        fi
        return 0
    fi

    # Root in privileged container: chown files to host user
    if [ "${HOST_UID}" != "0" ]; then
        log_info "修复构建产物所有权为宿主用户 (${HOST_UID}:${HOST_GID})..."
        if [ -d "${OUTPUT_DIR}" ]; then
            chown -R "${HOST_UID}:${HOST_GID}" "${OUTPUT_DIR}" 2>/dev/null || \
                chmod -R u+rwX,go+rX "${OUTPUT_DIR}" 2>/dev/null || true
        fi
        if [ -d "${SCRIPT_DIR}/build/release" ]; then
            chown -R "${HOST_UID}:${HOST_GID}" "${SCRIPT_DIR}/build/release" 2>/dev/null || \
                chmod -R u+rwX,go+rX "${SCRIPT_DIR}/build/release" 2>/dev/null || true
        fi
        # 修复 .cache/linglong（Docker 挂载持久化 OSTree repo），避免 root-owned 文件
        # 阻止后续容器启动时 repo 初始化
        local linglong_cache="${SCRIPT_DIR}/.cache/linglong"
        if [ -d "$linglong_cache" ]; then
            chown -R "${HOST_UID}:${HOST_GID}" "$linglong_cache" 2>/dev/null || \
                chmod -R u+rwX,go+rX "$linglong_cache" 2>/dev/null || true
        fi
    fi
}

cleanup() {
    local keep_temp="$1"

    if [ "$keep_temp" != "true" ]; then
        log_clean "清理临时文件..."
        # Ensure temp files are writable before removal (ll-builder may create
        # files with restrictive permissions inside the container)
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
    local uab_file="$2"
    local layer_file="$3"

    echo ""
    echo "=========================================="
    echo "Linyaps 构建完成！"
    echo "=========================================="
    echo ""
    echo "目标架构: $target_arch"
    echo "输出目录: $(dirname "$uab_file")/"
    echo ""
    echo "生成的 Linyaps 包:"
    ls -lh "$uab_file" 2>/dev/null || echo "  未找到 UAB 包"
    ls -lh "$layer_file" 2>/dev/null || echo "  未找到 layer 包"
    echo ""
    echo "安装命令:"
    if [ -n "$uab_file" ] && [ -f "$uab_file" ]; then
        echo "  ll-cli install $uab_file"
    fi
    echo ""
    echo "运行命令:"
    echo "  ll-cli run $APP_ID"
    echo ""
}

main() {
    local version=""
    local keep_temp="false"
    local target_arch=""
    local in_container="false"

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
            --in-container)
                in_container="true"
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
    echo "Ter-Music Linyaps 构建脚本"
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
            log_info "从 Git/CMakeLists.txt 检测到版本: $version"
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
    
    if [ "$in_container" = "true" ]; then
        log_info "容器内构建模式，跳过依赖检查"
    else
        check_dependencies "$target_arch"
    fi

    mkdir -p "${OUTPUT_DIR}/${target_arch}"

    if [ -d "${TEMP_DIR}" ]; then
        chmod -R u+rwX "${TEMP_DIR}" 2>/dev/null || true
        rm -rf "${TEMP_DIR}" || true
    fi
    mkdir -p "${TEMP_DIR}"
    # 调用函数，通过全局变量返回结果
    prepare_linyaps_structure "$TEMP_DIR" "$APP_ID"
    local project_root="$PROJECT_ROOT_OUTPUT"

    generate_linglong_yaml "$project_root" "$APP_ID" "$version" "$target_arch"

    if build_linyaps "$project_root"; then
        if export_uab "$project_root" "${OUTPUT_DIR}/${target_arch}" "$APP_ID" "$version" "$target_arch"; then
            fix_output_ownership
            cleanup "$keep_temp"
            show_summary "$target_arch" "${EXPORTED_UAB_FILE:-}" "${EXPORTED_LAYER_FILE:-}"
        else
            fix_output_ownership
            cleanup "$keep_temp"
            log_error "UAB 导出失败"
            exit 1
        fi
    else
        log_error "Linyaps 构建过程失败"
        cleanup "$keep_temp"
        exit 1
    fi
}

main "$@"
