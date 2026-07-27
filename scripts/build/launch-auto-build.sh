#!/bin/bash
#
# launch-auto-build.sh — 一键构建 ter-music 全平台包
#
# 核心设计：分两阶段执行
#   阶段一：构建所有需要的 Docker 镜像（如果尚未构建）
#   阶段二：依次构建所有包类型
#
# 支持交互式和 CLI 两种模式
#

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
SCRIPT_DIR="$(pwd)"
PROJECT_NAME="ter-music"
DEFAULT_VERSION="2.1.0"

# ── 颜色输出 ──────────────────────────────────────────────
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ── 日志设置 ──────────────────────────────────────────────
LOG_FILE=""

setup_logging() {
    local log_dir="$(pwd)/.tmp/logs"
    mkdir -p "$log_dir"
    LOG_FILE="${log_dir}/launch-auto-build-$(date +%Y%m%d-%H%M%S).log"
    {
        echo "════════════════════════════════════════════"
        echo "  Ter-Music Auto-Build Log"
        echo "  启动时间: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "  工作目录: $(pwd)"
        echo "════════════════════════════════════════════"
    } > "$LOG_FILE"
    log_info "日志文件: ${LOG_FILE}"

}

# 执行命令并将输出同时显示到终端和写入日志文件
run_cmd() {
    local -a cmd=("$@")
    if [ -n "$LOG_FILE" ]; then
        "${cmd[@]}" 2>&1 | tee -a "$LOG_FILE"
        return ${PIPESTATUS[0]}
    else
        "${cmd[@]}"
        return $?
    fi
}

log_ts() {
    date '+%Y-%m-%d %H:%M:%S'
}

# 写一行到日志文件（自动去除 ANSI 颜色码）
log_append() {
    [ -n "$LOG_FILE" ] && echo "$(log_ts) $*" >> "$LOG_FILE"
}

# 普通输出：同时写到终端和日志
log_print() {
    echo -e "$@"
    if [ -n "$LOG_FILE" ]; then
        echo -e "$(log_ts) $@" | sed 's/\x1b\[[0-9;]*m//g' >> "$LOG_FILE"
    fi
}

log_info()  { local m="$1"; echo -e "${GREEN}[$(log_ts)] [INFO]${NC} $m"; log_append "[INFO] $m"; }
log_warn()  { local m="$1"; echo -e "${YELLOW}[$(log_ts)] [WARN]${NC} $m" >&2; log_append "[WARN] $m"; }
log_error() { local m="$1"; echo -e "${RED}[$(log_ts)] [ERROR]${NC} $m" >&2; log_append "[ERROR] $m"; }
log_step()  { echo -e "\n${CYAN}[$(log_ts)] ══════ $1 ══════${NC}"; log_append "══════ $1 ══════"; }
log_ok()    { echo -e "  ${GREEN}✓${NC} $1"; log_append "✓ $1"; }
log_fail()  { echo -e "  ${RED}✗${NC} $1"; log_append "✗ $1"; }
log_skip()  { echo -e "  ${YELLOW}–${NC} $1"; log_append "– $1"; }

# ── 参数默认值 ────────────────────────────────────────────
VERSION=""
ARCHS=()
TYPES=()              # 空 = 全部
KEEP_TEMP="false"
SKIP_IMAGES="false"
REBUILD_IMAGES="false"
SKIP_BUILDS="false"
FAIL_FAST="false"
NO_DOCKER="false"
INTERACTIVE="false"

# ── 构建矩阵 (由 generate_build_matrix 填充) ─────────────
JOB_ARCH=()
JOB_TYPE=()
JOB_METHOD=()          # "container" 或 "native"
JOB_IMAGE=()
JOB_DOCKERFILE=()
JOB_BUILD_ARGS=()      # 透传给 docker build 的 --build-arg
JOB_INNER_ARGS=()      # 透传给内部构建脚本的参数
JOB_STATUS=()          # "" / "OK" / "FAIL" / "SKIP"

# ── 版本检测 ──────────────────────────────────────────────
detect_version() {
    local version="$DEFAULT_VERSION"

    if [ -d "${SCRIPT_DIR}/.git" ] && command -v git >/dev/null 2>&1; then
        local git_version
        git_version=$(git describe --tags --abbrev=0 2>/dev/null || true)
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

    echo "$version"
}

# ── 架构检测 (返回 deb 命名风格: amd64 / arm64) ──────────
detect_architecture() {
    local arch
    arch=$(uname -m)
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

# ── 架构名归一化 ──────────────────────────────────────────
# deb 脚本用 amd64 / arm64
deb_arch() {
    case "$1" in
        amd64|x86_64)           echo "amd64" ;;
        arm64|aarch64)          echo "arm64" ;;
        *)                      echo "$1" ;;
    esac
}

# rpm 脚本用 x86_64 / arm64
rpm_arch() {
    case "$1" in
        amd64|x86_64)           echo "x86_64" ;;
        arm64|aarch64)          echo "arm64" ;;
        *)                      echo "$1" ;;
    esac
}

# linyaps / appimage / portable 用 x86_64 / aarch64
native_arch() {
    case "$1" in
        amd64|x86_64)           echo "x86_64" ;;
        arm64|aarch64)          echo "aarch64" ;;
        *)                      echo "$1" ;;
    esac
}

# ── 帮助 ──────────────────────────────────────────────────
show_help() {
    cat << 'EOF'
用法: launch-auto-build.sh [选项]

一键构建 ter-music 所有包类型。

选项:
    -h, --help                显示此帮助信息
    -v, --version VERSION     指定版本号（默认：自动检测）
    -a, --arch ARCH           目标架构: amd64（逗号分隔，默认：amd64）
    -t, --types TYPES         包类型: deb,rpm,linyaps,appimage,portable（逗号分隔，默认：全部）
    -k, --keep-temp           保留临时构建文件（用于调试）

    --skip-images             跳过 Docker 镜像预构建阶段
    --rebuild-images          强制重新构建所有 Docker 镜像
    --skip-builds             仅构建 Docker 镜像，跳过包构建
    --fail-fast               遇构建失败立即停止（默认：继续）
    --no-docker               跳过所有依赖 Docker 的构建（deb/rpm）

交互模式:
    不带任何参数运行即进入交互模式，会提示输入各项配置。

支持的架构: amd64
支持的包类型: deb rpm linyaps appimage portable

EOF
}

# ── 参数解析 ──────────────────────────────────────────────
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -h|--help)
                show_help
                exit 0
                ;;
            -v|--version)
                VERSION="$2"
                shift 2
                ;;
            -a|--arch)
                IFS=',' read -ra parts <<< "$2"
                for p in "${parts[@]}"; do
                    p="$(echo "$p" | xargs)"  # trim
                    case "$p" in
                        amd64|x86_64)  ARCHS+=("amd64") ;;
                        *)
                            log_error "不支持的架构: $p（仅支持 amd64）"
                            exit 1
                            ;;
                    esac
                done
                shift 2
                ;;
            -t|--types)
                IFS=',' read -ra parts <<< "$2"
                for p in "${parts[@]}"; do
                    p="$(echo "$p" | xargs)"
                    case "$p" in
                        deb|rpm|linyaps|appimage|portable)
                            # 去重
                            local found_dup=false
                            for t in "${TYPES[@]}"; do
                                [ "$t" = "$p" ] && found_dup=true
                            done
                            if ! $found_dup; then
                                TYPES+=("$p")
                            fi
                            ;;
                        *)
                            log_error "不支持的包类型: $p（支持: deb,rpm,linyaps,appimage,portable）"
                            exit 1
                            ;;
                    esac
                done
                shift 2
                ;;
            -k|--keep-temp)
                KEEP_TEMP="true"
                shift
                ;;
            --skip-images)
                SKIP_IMAGES="true"
                shift
                ;;
            --rebuild-images)
                REBUILD_IMAGES="true"
                shift
                ;;
            --skip-builds)
                SKIP_BUILDS="true"
                shift
                ;;
            --fail-fast)
                FAIL_FAST="true"
                shift
                ;;
            --no-docker)
                NO_DOCKER="true"
                shift
                ;;
            --interactive)
                INTERACTIVE="true"
                shift
                ;;
            *)
                log_error "未知选项: $1"
                echo "用法: launch-auto-build.sh [选项]"
                echo "试试 --help 获取更多信息。"
                exit 1
                ;;
        esac
    done
}

# ── 交互模式 ──────────────────────────────────────────────
interactive_mode() {
    echo -e "\n${BOLD}Ter-Music 自动构建系统 — 交互模式${NC}"
    echo "=========================================="
    echo ""

    # 版本
    local detected
    detected=$(detect_version)
    read -r -p "版本号 [${detected}]: " input
    VERSION="${input:-$detected}"

    # 架构
    read -r -p "目标架构（支持 amd64）[amd64]: " input
    if [ -n "$input" ]; then
        ARCHS=()
        IFS=',' read -ra parts <<< "$input"
        for p in "${parts[@]}"; do
            p="$(echo "$p" | xargs)"
            case "$p" in
                amd64|x86_64)      ARCHS+=("amd64") ;;
                *)                  log_warn "忽略未知架构: $p" ;;
            esac
        done
    fi
    [ ${#ARCHS[@]} -eq 0 ] && ARCHS=("amd64")

    # 包类型
    echo "包类型（逗号分隔, 支持: deb,rpm,linyaps,appimage,portable）[全部]: "
    read -r -p "> " input
    if [ -n "$input" ]; then
        TYPES=()
        IFS=',' read -ra parts <<< "$input"
        for p in "${parts[@]}"; do
            p="$(echo "$p" | xargs)"
            case "$p" in
                deb|rpm|linyaps|appimage|portable)
                    local found=false
                    for t in "${TYPES[@]}"; do
                        [ "$t" = "$p" ] && found=true
                    done
                    if ! $found; then
                        TYPES+=("$p")
                    fi
                    ;;
                *)  log_warn "忽略未知包类型: $p" ;;
            esac
        done
    fi

    echo ""
    echo "=========================================="
    log_print "${BOLD}构建计划${NC}"
    echo "------------------------------------------"
    log_print "  版本:          ${VERSION}"
    log_print "  架构:          ${ARCHS[*]}"
    if [ ${#TYPES[@]} -gt 0 ]; then
        log_print "  包类型:        ${TYPES[*]}"
    else
        log_print "  包类型:        全部"
    fi
    echo "=========================================="
    echo ""
    read -r -p "确认开始构建？[Y/n] " input
    case "$input" in
        n|N|no|NO)  echo "已取消"; exit 0 ;;
        *)          echo "" ;;
    esac
}

# ── 构建矩阵生成 ──────────────────────────────────────────
generate_build_matrix() {
    local archs=("${ARCHS[@]}")
    local types=("${TYPES[@]}")
    [ ${#types[@]} -eq 0 ] && types=("deb" "rpm" "linyaps" "appimage" "portable")

    for arch in "${archs[@]}"; do
        for pkg_type in "${types[@]}"; do
            # 如果 --no-docker，跳过 deb/rpm（它们依赖 Docker）
            if [ "$NO_DOCKER" = "true" ] && { [ "$pkg_type" = "deb" ] || [ "$pkg_type" = "rpm" ]; }; then
                log_skip "${arch} ${pkg_type}: --no-docker 已跳过"
                continue
            fi

            case "$arch:$pkg_type" in
                amd64:deb)
                    JOB_ARCH+=("amd64")
                    JOB_TYPE+=("deb")
                    JOB_METHOD+=("container")
                    JOB_IMAGE+=("ter-music-deb-static")
                    JOB_DOCKERFILE+=("scripts/cross-compile/Dockerfile.deb-static")
                    JOB_BUILD_ARGS+=("")
                    JOB_INNER_ARGS+=("--static --with-source -v ${VERSION}")
                    JOB_STATUS+=("")
                    ;;
                amd64:rpm)
                    JOB_ARCH+=("amd64")
                    JOB_TYPE+=("rpm")
                    JOB_METHOD+=("container")
                    JOB_IMAGE+=("ter-music-rpm-static")
                    JOB_DOCKERFILE+=("scripts/cross-compile/Dockerfile.rpm-static")
                    JOB_BUILD_ARGS+=("")
                    JOB_INNER_ARGS+=("--static-build -v ${VERSION}")
                    JOB_STATUS+=("")
                    ;;
                amd64:linyaps)
                    JOB_ARCH+=("amd64")
                    JOB_TYPE+=("linyaps")
                    JOB_METHOD+=("container")
                    JOB_IMAGE+=("ter-music-uab-builder")
                    JOB_DOCKERFILE+=("scripts/cross-compile/Dockerfile.uab")
                    JOB_BUILD_ARGS+=("")
                    JOB_INNER_ARGS+=("-v ${VERSION} -a x86_64 --in-container")
                    JOB_STATUS+=("")
                    ;;
                amd64:appimage)
                    JOB_ARCH+=("amd64")
                    JOB_TYPE+=("appimage")
                    JOB_METHOD+=("native")
                    JOB_IMAGE+=("")
                    JOB_DOCKERFILE+=("")
                    JOB_BUILD_ARGS+=("")
                    JOB_INNER_ARGS+=("-v ${VERSION} -a x86_64")
                    JOB_STATUS+=("")
                    ;;
                amd64:portable)
                    JOB_ARCH+=("amd64")
                    JOB_TYPE+=("portable")
                    JOB_METHOD+=("native")
                    JOB_IMAGE+=("")
                    JOB_DOCKERFILE+=("")
                    JOB_BUILD_ARGS+=("")
                    JOB_INNER_ARGS+=("-v ${VERSION} -a x86_64")
                    JOB_STATUS+=("")
                    ;;
                *)
                    log_skip "${arch} ${pkg_type}: 暂不支持的构建组合"
                    ;;
            esac
        done
    done
}

# ── Docker 镜像预构建 ─────────────────────────────────────
build_docker_images() {
    # 收集去重的镜像定义
    local -a img_names=()
    local -a img_files=()
    local -a img_args=()

    for ((i=0; i<${#JOB_TYPE[@]}; i++)); do
        [ "${JOB_METHOD[$i]}" != "container" ] && continue
        [ "${JOB_STATUS[$i]}" = "SKIP" ] && continue

        local name="${JOB_IMAGE[$i]}"
        local file="${JOB_DOCKERFILE[$i]}"
        local build_args="${JOB_BUILD_ARGS[$i]}"

        # 去重
        local found=false
        for ((j=0; j<${#img_names[@]}; j++)); do
            if [ "${img_names[$j]}" = "$name" ] && [ "${img_files[$j]}" = "$file" ] && [ "${img_args[$j]}" = "$build_args" ]; then
                found=true; break
            fi
        done
        if ! $found; then
            img_names+=("$name")
            img_files+=("$file")
            img_args+=("$build_args")
        fi
    done

    [ ${#img_names[@]} -eq 0 ] && return

    log_step "阶段一：Docker 镜像预构建"
    echo ""

    local total=${#img_names[@]}
    local count=0

    for ((i=0; i<total; i++)); do
        count=$((count + 1))
        local name="${img_names[$i]}"
        local file="${img_files[$i]}"
        local build_args="${img_args[$i]}"

        local progress_label="[${count}/${total}] 构建镜像: ${name}"
        log_print "  ${progress_label} ..."

        if [ "$REBUILD_IMAGES" = "true" ]; then
            docker rmi "$name" 2>/dev/null || true
        elif docker image inspect "$name" &>/dev/null; then
            log_print "  ${GREEN}已存在${NC}"
            continue
        fi

        # 构建镜像
        local -a docker_cmd=(docker build)
        if [ -n "$build_args" ]; then
            # shellcheck disable=SC2206
            docker_cmd+=($build_args)
        fi
        docker_cmd+=(-f "$SCRIPT_DIR/$file" -t "$name" "$SCRIPT_DIR")

        if run_cmd "${docker_cmd[@]}"; then
            log_print "  ${GREEN}完成${NC}"
        else
            log_print "${RED}失败${NC}"
            # 标记所有依赖此镜像的 job 为 SKIP
            for ((j=0; j<${#JOB_TYPE[@]}; j++)); do
                if [ "${JOB_IMAGE[$j]}" = "$name" ] && [ "${JOB_STATUS[$j]}" != "OK" ]; then
                    JOB_STATUS[$j]="SKIP"
                fi
            done
            if [ "$FAIL_FAST" = "true" ]; then
                log_error "Docker 镜像构建失败，退出"
                exit 1
            fi
        fi
    done
    echo ""
}

# ── 执行单个构建 ─────────────────────────────────────────
execute_single_build() {
    local idx="$1"
    local arch="${JOB_ARCH[$idx]}"
    local pkg_type="${JOB_TYPE[$idx]}"
    local method="${JOB_METHOD[$idx]}"
    local image="${JOB_IMAGE[$idx]}"
    local dockerfile="${JOB_DOCKERFILE[$idx]}"
    local build_args="${JOB_BUILD_ARGS[$idx]}"
    local inner_args="${JOB_INNER_ARGS[$idx]}"

    # 构建一个标签，用于日志显示
    local label="${arch} ${pkg_type}"
    [ -n "$image" ] && label="${label} [${image}]"

    if [ "$method" = "container" ]; then
        # ── 容器构建：调用 cross-build.sh ──
        local -a xb_args=()

        # -s 构建脚本名
        xb_args+=(-s "build-${pkg_type}.sh")
        # -f Dockerfile 路径（相对项目根）
        xb_args+=(-f "$dockerfile")
        # -n 镜像名
        xb_args+=(-n "$image")

        # -a 架构名（各脚本的命名不同）
        case "$pkg_type" in
            deb)    xb_args+=(-a "$(deb_arch "$arch")") ;;
            rpm)    xb_args+=(-a "$(rpm_arch "$arch")") ;;
            *)      xb_args+=(-a "$arch") ;;
        esac

        # linyaps 需要特权模式（ll-builder 需要 user namespace）
        [ "$pkg_type" = "linyaps" ] && xb_args+=(-p)

        # 透传 docker build args（如 --build-arg）
        if [ -n "$build_args" ]; then
            # 按空格分割（只有一个 --build-arg KEY=VALUE 字符串）
            # 注意：如果 build_args 含空格会被当作多个参数
            # 这里我们用 eval 级别的拆分
            eval "local -a extra_args=($build_args)"
            for arg in "${extra_args[@]}"; do
                xb_args+=("$arg")
            done
        fi

        # 透传 keep-temp
        [ "$KEEP_TEMP" = "true" ] && inner_args="$inner_args --keep-temp"

        # 内部构建脚本参数（-- 分隔）
        if [ -n "$inner_args" ]; then
            xb_args+=(--)
            eval "local -a inner_parts=($inner_args)"
            for arg in "${inner_parts[@]}"; do
                xb_args+=("$arg")
            done
        fi

        log_info "执行: ${SCRIPT_DIR}/scripts/cross-compile/cross-build.sh ${xb_args[*]}"

        run_cmd "${SCRIPT_DIR}/scripts/cross-compile/cross-build.sh" "${xb_args[@]}"
        return $?

    else
        # ── 本地构建：直接调用构建脚本 ──
        local script_path="${SCRIPT_DIR}/scripts/build/build-${pkg_type}.sh"
        local -a native_args=()

        # inner_args 已经包含了 -v 和 -a
        eval "local -a inner_parts=($inner_args)"
        for arg in "${inner_parts[@]}"; do
            native_args+=("$arg")
        done

        [ "$KEEP_TEMP" = "true" ] && native_args+=(-k)

        log_info "执行: ${script_path} ${native_args[*]}"

        run_cmd "${script_path}" "${native_args[@]}"
        return $?
    fi
}

# ── 查找构建产物路径（用于退出码非零时二次确认） ──────────
build_artifact_path() {
    local arch="$1"
    local pkg_type="$2"

    case "$pkg_type" in
        deb)
            local darch
            darch=$(deb_arch "$arch")
            echo "${SCRIPT_DIR}/build/deb/${darch}/ter-music_${VERSION}-1_${darch}.deb"
            ;;
        rpm)
            local rarch
            rarch=$(rpm_arch "$arch")
            echo "${SCRIPT_DIR}/build/rpm/${rarch}/ter-music-${VERSION}-1.el9.${rarch}.rpm"
            ;;
        linyaps)
            local narch
            narch=$(native_arch "$arch")
            echo "${SCRIPT_DIR}/build/linyaps/${narch}/org.yxzl.ter-music_${VERSION}_${narch}.uab"
            ;;
        appimage)
            local narch
            narch=$(native_arch "$arch")
            echo "${SCRIPT_DIR}/build/appimage/${narch}/ter-music-${VERSION}-${narch}.AppImage"
            ;;
        portable)
            local narch
            narch=$(native_arch "$arch")
            echo "${SCRIPT_DIR}/build/portable/${narch}/ter-music-${VERSION}-portable-${narch}.tar.gz"
            ;;
        *)
            echo ""
            ;;
    esac
}

# ── 执行所有构建 ──────────────────────────────────────────
execute_builds() {
    local total=0
    # 计算有效 job 数（非 SKIP）
    for ((i=0; i<${#JOB_TYPE[@]}; i++)); do
        [ "${JOB_STATUS[$i]}" != "SKIP" ] && total=$((total + 1))
    done

    [ $total -eq 0 ] && { log_info "没有需要构建的包"; return; }

    log_step "阶段二：包构建"
    echo ""

    local count=0
    local success_count=0
    local fail_count=0

    for ((i=0; i<${#JOB_TYPE[@]}; i++)); do
        [ "${JOB_STATUS[$i]}" = "SKIP" ] && continue

        count=$((count + 1))
        local arch="${JOB_ARCH[$i]}"
        local pkg_type="${JOB_TYPE[$i]}"

        echo ""
        log_print "${BOLD}[${count}/${total}] 构建 ${arch} ${pkg_type}${NC}"
        log_print "──────────────────────────────────────────"

        set +e
        if execute_single_build "$i"; then
            JOB_STATUS[$i]="OK"
            success_count=$((success_count + 1))
            log_ok "${arch} ${pkg_type} 构建成功"
        else
            # 退出码非零时，二次校验产物文件是否存在
            local artifact
            artifact=$(build_artifact_path "$arch" "$pkg_type")
            if [ -n "$artifact" ] && [ -f "$artifact" ]; then
                JOB_STATUS[$i]="OK"
                success_count=$((success_count + 1))
                log_warn "${arch} ${pkg_type} 脚本退出码非零，但产物文件已存在，视为成功"
            else
                JOB_STATUS[$i]="FAIL"
                fail_count=$((fail_count + 1))
                log_fail "${arch} ${pkg_type} 构建失败"
                if [ "$FAIL_FAST" = "true" ]; then
                    log_error "遇构建失败，退出"
                    break
                fi
            fi
        fi
        set -e
    done

    echo ""
    BUILD_SUCCESS=$success_count
    BUILD_FAIL=$fail_count
    BUILD_TOTAL=$total
}

# ── 构建结果汇总 ──────────────────────────────────────────
show_summary() {
    echo ""
    log_step "构建完成"
    echo ""

    log_print "  版本: ${VERSION}"
    log_print ""

    # 按架构分组显示
    local archs_shown=()
    for ((i=0; i<${#JOB_TYPE[@]}; i++)); do
        local arch="${JOB_ARCH[$i]}"
        # 检查是否已显示过
        local shown=false
        for a in "${archs_shown[@]}"; do
            [ "$a" = "$arch" ] && shown=true
        done
        if $shown; then
            continue
        fi
        archs_shown+=("$arch")

        log_print "${BOLD}  [${arch}]${NC}"
        for ((j=0; j<${#JOB_TYPE[@]}; j++)); do
            [ "${JOB_ARCH[$j]}" != "$arch" ] && continue
            local pkg_type="${JOB_TYPE[$j]}"
            local status="${JOB_STATUS[$j]}"
            case "$status" in
                OK)   log_ok "${pkg_type}" ;;
                FAIL) log_fail "${pkg_type}" ;;
                SKIP) log_skip "${pkg_type}（已跳过）" ;;
                *)    log_skip "${pkg_type}（未执行）" ;;
            esac
        done
        echo ""
    done

    # 总体统计
    local ok=0
    local fail=0
    local skip=0
    for ((i=0; i<${#JOB_STATUS[@]}; i++)); do
        case "${JOB_STATUS[$i]}" in
            OK)   ok=$((ok + 1)) ;;
            FAIL) fail=$((fail + 1)) ;;
            SKIP) skip=$((skip + 1)) ;;
        esac
    done
    local total=$((ok + fail + skip))

    log_print "  ────────────────────────────────────────"
    log_print "  ${GREEN}成功: ${ok}${NC}  ${RED}失败: ${fail}${NC}  ${YELLOW}跳过: ${skip}${NC}  总计: ${total}"
    log_print ""

    # 检查 release 目录
    if [ -d "${SCRIPT_DIR}/build/release" ]; then
        log_print "  构建产物目录: ${SCRIPT_DIR}/build/release/"
        ls -lh "${SCRIPT_DIR}/build/release/" 2>/dev/null | awk 'NR>1{print "    " $NF " (" $5 ")"}' >> "$LOG_FILE" 2>/dev/null || true
        ls -lh "${SCRIPT_DIR}/build/release/" 2>/dev/null | awk 'NR>1{print "    " $NF " (" $5 ")"}' || true
    fi
    log_print ""
}

# ── 主入口 ────────────────────────────────────────────────
main() {
    # 解析参数
    parse_args "$@"

    echo ""
    echo -e "${BOLD}╔══════════════════════════════════════╗${NC}"
    echo -e "${BOLD}║     Ter-Music 自动构建系统           ║${NC}"
    echo -e "${BOLD}╚══════════════════════════════════════╝${NC}"
    echo ""

    # 初始化日志（之后所有输出同时写入日志文件）
    setup_logging

    # 没有参数 → 交互模式
    if [ $# -eq 0 ]; then
        INTERACTIVE="true"
    fi

    if [ "$INTERACTIVE" = "true" ]; then
        interactive_mode
    fi

    # 自动检测版本
    if [ -z "$VERSION" ]; then
        VERSION=$(detect_version)
        log_info "自动检测到版本: ${VERSION}"
    fi

    # 验证版本号格式（防止注入）
    if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        log_error "无效的版本号格式: $VERSION（必须为 X.Y.Z 格式）"
        exit 1
    fi

    # 默认架构 = amd64
    if [ ${#ARCHS[@]} -eq 0 ]; then
        ARCHS=("amd64")
        log_info "默认架构: ${ARCHS[*]}"
    fi

    echo ""
    log_print "  版本:          ${VERSION}"
    log_print "  架构:          ${ARCHS[*]}"
    if [ ${#TYPES[@]} -gt 0 ]; then
        log_print "  包类型:        ${TYPES[*]}"
    else
        log_print "  包类型:        全部"
    fi
    echo "  Docker 镜像:   $([ "$SKIP_IMAGES" = "true" ] && echo "跳过" || echo "自动")"
    echo ""

    # 检查基础依赖
    if ! command -v docker &>/dev/null && [ "$NO_DOCKER" = "false" ]; then
        # 检查是否需要 Docker
        local need_docker=false
        [ ${#TYPES[@]} -eq 0 ] && need_docker=true
        for t in "${TYPES[@]}"; do
            [ "$t" = "deb" ] || [ "$t" = "rpm" ] && need_docker=true
        done
        if [ "$need_docker" = "true" ]; then
            log_warn "Docker 未安装。容器构建（deb/rpm）将被跳过。"
            log_warn "安装命令: sudo apt install docker.io"
            NO_DOCKER="true"
        fi
    fi

    # 生成构建矩阵
    generate_build_matrix

    if [ ${#JOB_TYPE[@]} -eq 0 ]; then
        log_warn "没有匹配的构建任务"
        exit 0
    fi

    # 阶段一：Docker 镜像预构建
    if [ "$SKIP_IMAGES" = "false" ] && [ "$NO_DOCKER" = "false" ]; then
        build_docker_images
    fi

    # 阶段二：包构建
    if [ "$SKIP_BUILDS" = "false" ]; then
        execute_builds
    else
        # 跳过构建时，将所有未执行的 job 标记为 SKIP
        for ((i=0; i<${#JOB_STATUS[@]}; i++)); do
            if [ -z "${JOB_STATUS[$i]}" ]; then
                JOB_STATUS[$i]="SKIP"
            fi
        done
    fi

    # 汇总
    show_summary

    # 日志结尾
    log_append "════════════════════════════════════════"
    log_append "构建结束"

    # 如果存在失败，非零退出
    local fail_count=0
    for s in "${JOB_STATUS[@]}"; do
        [ "$s" = "FAIL" ] && fail_count=$((fail_count + 1))
    done
    [ $fail_count -gt 0 ] && exit 1 || exit 0
}

main "$@"
