#!/usr/bin/env bash
#
# 一键构建 ter-music 的 deb/rpm/linyaps/appimage/aur。
# 版本只由 -v 指定；RPM 默认构建 EL7/8/9/10 四包。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=common.sh
. "${SCRIPT_DIR}/scripts/build/common.sh"

REPO_ROOT="$SCRIPT_DIR"
VERSION=""
ARCH="amd64"
TYPES=(deb rpm linyaps appimage aur)
RPM_EL="all"
KEEP_TEMP=false
WITH_SOURCE=false
WITH_DEBUGINFO=false
REBUILD_IMAGES=false
FAIL_FAST=false
LOG_FILE=""
PASS=0
FAIL=0
FAILED_TYPES=()

show_help() {
    cat <<EOF
用法: $0 -v VERSION [选项]

一键构建 ter-music 打包产物。

选项:
  -v, --version VERSION   必填；三段自动补 .0 为四段
  -a, --arch ARCH         仅支持 amd64（默认 amd64）
  -t, --types LIST        deb,rpm,linyaps,appimage,aur（默认全部）
      --rpm-el LIST       7,8,9,10 或 all（默认 all）
  -k, --keep-temp         保留各脚本临时目录
      --with-source       给 deb/rpm 追加源码包
      --with-debuginfo    给 deb/rpm 追加调试包
      --rebuild-images    强制重建所有 Docker 镜像
      --fail-fast         任一类型失败立即停止
  -h, --help              显示帮助
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        -v|--version) VERSION="${2:-}"; shift 2 ;;
        -a|--arch) ARCH="${2:-}"; shift 2 ;;
        -t|--types)
            IFS=',' read -r -a TYPES <<< "${2:-}"
            shift 2
            ;;
        --rpm-el) RPM_EL="${2:-}"; shift 2 ;;
        -k|--keep-temp) KEEP_TEMP=true; shift ;;
        --with-source) WITH_SOURCE=true; shift ;;
        --with-debuginfo) WITH_DEBUGINFO=true; shift ;;
        --rebuild-images) REBUILD_IMAGES=true; shift ;;
        --fail-fast) FAIL_FAST=true; shift ;;
        *)
            tm_log_error "unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

VERSION="$(tm_normalize_version_or_die "$VERSION")" || exit 1
if [ "$ARCH" != "amd64" ]; then
    tm_log_error "unsupported architecture: $ARCH (only amd64)"
    exit 1
fi
if [ "${#TYPES[@]}" -eq 0 ]; then
    tm_log_error "no package type selected"
    exit 1
fi
for type in "${TYPES[@]}"; do
    case "$type" in
        deb|rpm|linyaps|appimage|aur) ;;
        *) tm_log_error "unsupported type: $type"; exit 1 ;;
    esac
done

if [ "$WITH_SOURCE" = true ] || [ "$WITH_DEBUGINFO" = true ]; then
    has_deb_rpm=false
    for type in "${TYPES[@]}"; do
        if [ "$type" = "deb" ] || [ "$type" = "rpm" ]; then
            has_deb_rpm=true
        fi
    done
    if [ "$has_deb_rpm" = false ]; then
        tm_log_warn "--with-source/--with-debuginfo only apply to deb/rpm; ignoring"
        WITH_SOURCE=false
        WITH_DEBUGINFO=false
    fi
fi

mkdir -p "${REPO_ROOT}/.tmp/logs"
LOG_FILE="${REPO_ROOT}/.tmp/logs/launch-auto-build-$(date +%Y%m%d-%H%M%S).log"
{
    echo "ter-music auto build"
    echo "version: ${VERSION}"
    echo "arch: ${ARCH}"
    echo "types: ${TYPES[*]}"
    echo "rpm-el: ${RPM_EL}"
    echo "source: ${WITH_SOURCE} debug: ${WITH_DEBUGINFO}"
    echo "start: $(date '+%Y-%m-%d %H:%M:%S')"
} | tee -a "$LOG_FILE"

run_cmd() {
    "$@" 2>&1 | tee -a "$LOG_FILE"
    return "${PIPESTATUS[0]}"
}

run_type() {
    local type="$1"
    case "$type" in
        deb)
            local args=(-v "$VERSION" -a amd64)
            [ "$KEEP_TEMP" = true ] && args+=(-k)
            [ "$WITH_SOURCE" = true ] && args+=(--with-source)
            [ "$WITH_DEBUGINFO" = true ] && args+=(--with-debuginfo)
            [ "$REBUILD_IMAGES" = true ] && args+=(--rebuild-image)
            run_cmd "${REPO_ROOT}/scripts/build/build-deb.sh" "${args[@]}"
            ;;
        rpm)
            local args=(-v "$VERSION" -a x86_64 --el "$RPM_EL")
            [ "$KEEP_TEMP" = true ] && args+=(-k)
            [ "$WITH_SOURCE" = true ] && args+=(--with-source)
            [ "$WITH_DEBUGINFO" = true ] && args+=(--with-debuginfo)
            [ "$REBUILD_IMAGES" = true ] && args+=(--rebuild-image)
            run_cmd "${REPO_ROOT}/scripts/build/build-rpm.sh" "${args[@]}"
            ;;
        linyaps)
            local args=(-v "$VERSION" -a x86_64)
            [ "$KEEP_TEMP" = true ] && args+=(-k)
            run_cmd "${REPO_ROOT}/scripts/build/build-linyaps.sh" "${args[@]}"
            ;;
        appimage)
            local args=(-v "$VERSION" -a x86_64)
            [ "$KEEP_TEMP" = true ] && args+=(-k)
            [ "$REBUILD_IMAGES" = true ] && args+=(--rebuild-image)
            run_cmd "${REPO_ROOT}/scripts/build/build-appimage.sh" "${args[@]}"
            ;;
        aur)
            local args=(-v "$VERSION" -a x86_64)
            [ "$KEEP_TEMP" = true ] && args+=(-k)
            [ "$REBUILD_IMAGES" = true ] && args+=(--rebuild-image)
            run_cmd "${REPO_ROOT}/scripts/build/build-aur.sh" "${args[@]}"
            ;;
    esac
}

for type in "${TYPES[@]}"; do
    echo ""
    echo "══════ ${type} ══════"
    set +e
    run_type "$type"
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        PASS=$((PASS + 1))
        echo "✓ ${type}"
    else
        FAIL=$((FAIL + 1))
        FAILED_TYPES+=("$type")
        echo "✗ ${type}"
        if [ "$FAIL_FAST" = true ]; then
            break
        fi
    fi
done

echo ""
echo "=========================================="
echo "构建完成"
echo "=========================================="
echo "版本: ${VERSION}"
echo "成功: ${PASS}  失败: ${FAIL}"
if [ "${#FAILED_TYPES[@]}" -gt 0 ]; then
    echo "失败类型: ${FAILED_TYPES[*]}"
fi
echo "日志: ${LOG_FILE}"
echo "release: ${REPO_ROOT}/build/release/"
ls -lh "${REPO_ROOT}/build/release/" 2>/dev/null || true

[ "$FAIL" -eq 0 ]
