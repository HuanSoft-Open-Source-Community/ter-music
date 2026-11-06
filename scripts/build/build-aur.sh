#!/usr/bin/env bash
#
# AUR 构建 + 安装验收（Arch 容器）。不投递、不打 tag。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=common.sh
. "${SCRIPT_DIR}/scripts/build/common.sh"

PACKAGE_NAME="cn.org.huansoft.ter-music"
REPO_ROOT="$SCRIPT_DIR"

show_help() {
    cat <<EOF
用法: $0 -v VERSION [选项]

在 Arch 容器里 makepkg 并安装验收 AUR 包（不投递）。

选项:
  -v, --version VERSION   必填；三段自动补 .0 为四段
  -a, --arch ARCH         仅支持 x86_64（默认 x86_64）
  -k, --keep-temp         保留 .tmp/aur
      --no-install-test   只构建，不做 pacman -U 安装验收
      --rebuild-image     强制重建 Arch 镜像
      --no-cache          docker build 不使用缓存
  -h, --help              显示帮助

输出: build/aur/x86_64/cn.org.huansoft.ter-music-<V>-1-x86_64.pkg.tar.zst
EOF
}

VERSION=""
ARCH="x86_64"
KEEP_TEMP=false
INSTALL_TEST=true
REBUILD_IMAGE=false
NO_CACHE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        -v|--version) VERSION="${2:-}"; shift 2 ;;
        -a|--arch) ARCH="${2:-}"; shift 2 ;;
        -k|--keep-temp) KEEP_TEMP=true; shift ;;
        --no-install-test) INSTALL_TEST=false; shift ;;
        --rebuild-image) REBUILD_IMAGE=true; shift ;;
        --no-cache) NO_CACHE=true; shift ;;
        *)
            tm_log_error "unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

VERSION="$(tm_normalize_version_or_die "$VERSION")" || exit 1
if [ "$ARCH" != "x86_64" ]; then
    tm_log_error "unsupported architecture: $ARCH (only x86_64)"
    exit 1
fi

if ! tm_is_container; then
    xb_args=(
        -f scripts/docker/Dockerfile.aur
        -n ter-music-aur
        -s build-aur.sh
        --root
    )
    [ "$REBUILD_IMAGE" = true ] && xb_args+=(--rebuild)
    [ "$NO_CACHE" = true ] && xb_args+=(--no-cache)
    inner=(-v "$VERSION" -a "$ARCH")
    [ "$KEEP_TEMP" = true ] && inner+=(-k)
    [ "$INSTALL_TEST" = false ] && inner+=(--no-install-test)
    exec "${REPO_ROOT}/scripts/docker/docker-build.sh" "${xb_args[@]}" -- "${inner[@]}"
fi

WORK="${REPO_ROOT}/.tmp/aur"
SRCDIR="${WORK}/src"
OUT_DIR="${REPO_ROOT}/build/aur/x86_64"
PKG_DIR="${WORK}/pkg"

rm -rf "$WORK"
mkdir -p "$SRCDIR" "$OUT_DIR" "$PKG_DIR"

# 容器以 root 运行；即使 makepkg/pacman 失败，也要把产物目录交还宿主用户。
restore_owner() {
    if [ -n "${HOST_UID:-}" ] && [ -n "${HOST_GID:-}" ]; then
        chown -R "${HOST_UID}:${HOST_GID}" \
            "${REPO_ROOT}/build" "$WORK" 2>/dev/null || true
    fi
}
trap restore_owner EXIT

tm_log_info "AUR build: version=${VERSION} arch=${ARCH} install_test=${INSTALL_TEST}"

# makepkg 的 $srcdir 固定为 $BUILDDIR/<pkgbase>/src，源码必须放在这里，
# 否则 build()/prepare() 里的 cd "$srcdir/ter-music" 会失败。
SRC_STAGE="${WORK}/build/${PACKAGE_NAME}/src"
mkdir -p "$SRC_STAGE"
tm_stage_source "${SRC_STAGE}/ter-music"

# 本地验收用 --noextract，makepkg 不会执行 PKGBUILD 的 prepare()；
# 这里手工做同一条头文件改写（Arch 的 ncurses 头文件直接在 /usr/include）。
find "${SRC_STAGE}/ter-music" \( -name '*.c' -o -name '*.h' \) \
    -exec sed -i 's|ncursesw/||g' {} +

# 本地验收用 --noextract：把源码直接放到 $srcdir，避免依赖网络/tag。
cp -f "${REPO_ROOT}/PKGBUILD" "${WORK}/PKGBUILD"
sed -i \
    -e "s/^pkgver=.*/pkgver=${VERSION}/" \
    -e 's/^source=.*/source=()/' \
    -e 's/^sha256sums=.*/sha256sums=()/' \
    "${WORK}/PKGBUILD"

chown -R builder:builder "$WORK"

tm_log_info "makepkg (as builder)"
set +e
su -s /bin/bash builder -c \
    "cd '${WORK}' && HOME=/home/builder PKGDEST='${PKG_DIR}' SRCDEST='${SRCDIR}' BUILDDIR='${WORK}/build' makepkg -f --noextract --nodeps --noconfirm"
makepkg_rc=$?
set -e
if [ "$makepkg_rc" -ne 0 ]; then
    tm_log_error "makepkg failed"
    exit 1
fi

pkg_file="$(find "$PKG_DIR" -maxdepth 1 -name '*.pkg.tar.zst' -type f | head -n 1)"
if [ -z "$pkg_file" ]; then
    tm_log_error "makepkg produced no .pkg.tar.zst"
    exit 1
fi

if [ "$INSTALL_TEST" = true ]; then
    tm_log_info "install and smoke test (as root)"
    pacman -U --noconfirm "$pkg_file"
    if ! ter-music --help >/dev/null; then
        tm_log_error "installed package: ter-music --help failed"
        exit 1
    fi
    installed_version="$(ter-music version | awk '{print $2}')"
    if [ "$installed_version" != "v${VERSION}" ]; then
        tm_log_error "installed version '${installed_version}' != v${VERSION}"
        exit 1
    fi
fi

# .SRCINFO 漂移检查：用原始 PKGBUILD 生成，和仓库里的文件比较。
mkdir -p "${WORK}/srcinfo"
cp -f "${REPO_ROOT}/PKGBUILD" "${WORK}/srcinfo/PKGBUILD"
chown -R builder:builder "${WORK}/srcinfo"
su -s /bin/bash builder -c \
    "cd '${WORK}/srcinfo' && HOME=/home/builder makepkg --printsrcinfo > '${WORK}/SRCINFO.generated'"
if ! diff -u "${REPO_ROOT}/.SRCINFO" "${WORK}/SRCINFO.generated"; then
    tm_log_error ".SRCINFO is out of date; regenerate it with makepkg --printsrcinfo"
    exit 1
fi

cp -f "$pkg_file" "$OUT_DIR/"
tm_copy_release "$pkg_file"
tm_fix_ownership

if [ "$KEEP_TEMP" != true ]; then
    rm -rf "$WORK"
fi

echo ""
echo "=========================================="
echo "AUR 验收完成"
echo "=========================================="
echo "产物: ${OUT_DIR}/$(basename "$pkg_file")"
ls -lh "${OUT_DIR}/"*.pkg.tar.zst 2>/dev/null || true
