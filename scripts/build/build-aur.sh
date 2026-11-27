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

# 内置消音模型：下载/校验到 .tmp/models 缓存并复用。PKGBUILD 的 source 数组
# 里有同名模型（正式 makepkg 会下载并用 sha256sums 校验）；本地验收用
# --noextract，这里把缓存文件预置到 $srcdir，省掉一次 29MiB 下载。
tm_ensure_model || exit 1
cp -f -- "$TM_MODEL_PATH" "${SRC_STAGE}/${TM_MODEL_NAME}"
tm_log_info "bundled karaoke model staged: ${SRC_STAGE}/${TM_MODEL_NAME}"

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

pkg_file="$(find "$PKG_DIR" -maxdepth 1 -name '*.pkg.tar.zst' -type f -print -quit)"
if [ -z "$pkg_file" ]; then
    tm_log_error "makepkg produced no .pkg.tar.zst"
    exit 1
fi

# 包内布局（Linyaps 目录规范）：实体在 /opt/apps/<appid>/files，
# /usr 下只有符号链接；模型与 NOTICE 必须随包分发。
pkg_listing="$(pacman -Qlp "$pkg_file" | awk '{print $2}')"
for required in \
    "opt/apps/${PACKAGE_NAME}/files/bin/ter-music" \
    "opt/apps/${PACKAGE_NAME}/files/share/ter-music/models/uvr-mdxnet-3-9662.onnx" \
    "opt/apps/${PACKAGE_NAME}/files/share/ter-music/models/NOTICE.md" \
    "opt/apps/${PACKAGE_NAME}/files/share/ter-music/lang/zh_CN.xml" \
    "opt/apps/${PACKAGE_NAME}/files/share/ter-music/layouts/layouts.json" \
    "opt/apps/${PACKAGE_NAME}/files/share/applications/ter-music.desktop" \
    "usr/bin/ter-music" \
    "usr/share/applications/ter-music.desktop" \
    "usr/share/icons/hicolor/32x32/apps/ter-music.png"; do
    if ! grep -qx "$required" <<< "$pkg_listing"; then
        tm_log_error "AUR package is missing required path: ${required}"
        exit 1
    fi
done
# pacman -Qlp 不显示链接类型，解包后用 -L 断言 /usr 下不是实体文件。
pkg_extract="${WORK}/pkg-extract"
mkdir -p "$pkg_extract"
tar --zstd -xf "$pkg_file" -C "$pkg_extract"
opt_root="${pkg_extract}/opt/apps/${PACKAGE_NAME}/files"
model_path="${opt_root}/share/ter-music/models/uvr-mdxnet-3-9662.onnx"
if [ "$(wc -c < "$model_path" | tr -d ' \n')" != "$TM_MODEL_SIZE" ]; then
    tm_log_error "AUR package bundled model size mismatch"
    exit 1
fi
if [ "$(tm_sha256_of "$model_path")" != "$TM_MODEL_SHA256" ]; then
    tm_log_error "AUR package bundled model sha256 mismatch"
    exit 1
fi
for link in \
    "${pkg_extract}/usr/bin/ter-music" \
    "${pkg_extract}/usr/share/applications/ter-music.desktop" \
    "${pkg_extract}/usr/share/icons/hicolor/32x32/apps/ter-music.png" \
    "${pkg_extract}/usr/share/icons/hicolor/48x48/apps/ter-music.png" \
    "${pkg_extract}/usr/share/icons/hicolor/128x128/apps/ter-music.png" \
    "${pkg_extract}/usr/share/icons/hicolor/scalable/apps/ter-music.svg"; do
    if [ ! -L "$link" ]; then
        tm_log_error "AUR package path is not a symlink: ${link#${pkg_extract}}"
        exit 1
    fi
done
if [ -e "${pkg_extract}/usr/bin/ter-music" ] && [ ! -L "${pkg_extract}/usr/bin/ter-music" ]; then
    tm_log_error "AUR package still ships a real /usr/bin/ter-music file"
    exit 1
fi

if [ "$INSTALL_TEST" = true ]; then
    tm_log_info "install and smoke test (as root)"
    pacman -U --noconfirm "$pkg_file"
    installed_root="/opt/apps/${PACKAGE_NAME}/files"
    installed_model="${installed_root}/share/ter-music/models/uvr-mdxnet-3-9662.onnx"
    if [ ! -x "${installed_root}/bin/ter-music" ]; then
        tm_log_error "installed layout: ${installed_root}/bin/ter-music missing"
        exit 1
    fi
    if [ ! -f "${installed_model}" ] || [ "$(wc -c < "$installed_model" | tr -d ' \n')" != "$TM_MODEL_SIZE" ]; then
        tm_log_error "installed layout: bundled model missing or wrong size: ${installed_model}"
        exit 1
    fi
    if [ ! -f "${installed_root}/share/ter-music/models/NOTICE.md" ]; then
        tm_log_error "installed layout: models/NOTICE.md missing"
        exit 1
    fi
    if [ ! -L /usr/bin/ter-music ] ||
       [ "$(readlink -f /usr/bin/ter-music)" != "${installed_root}/bin/ter-music" ]; then
        tm_log_error "/usr/bin/ter-music is not a symlink to ${installed_root}/bin/ter-music"
        exit 1
    fi
    if [ ! -L /usr/share/applications/ter-music.desktop ] ||
       [ ! -L /usr/share/icons/hicolor/128x128/apps/ter-music.png ]; then
        tm_log_error "installed /usr/share symlinks are missing"
        exit 1
    fi
    if ! ter-music --help >/dev/null; then
        tm_log_error "installed package: ter-music --help failed"
        exit 1
    fi
    installed_version="$(ter-music version | awk '{print $2}')"
    if [ "$installed_version" != "v${VERSION}" ]; then
        tm_log_error "installed version '${installed_version}' != v${VERSION}"
        exit 1
    fi
    # 卸载安全性：链接与 /opt 实体都由本包拥有，卸载后应一并消失。
    pacman -R --noconfirm "$PACKAGE_NAME"
    if [ -e /usr/bin/ter-music ] || [ -e "${installed_root}/bin/ter-music" ]; then
        tm_log_error "uninstall left files behind: /usr/bin/ter-music or ${installed_root}"
        exit 1
    fi
    tm_log_info "install/uninstall smoke test passed (symlinks + /opt entity)"
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
