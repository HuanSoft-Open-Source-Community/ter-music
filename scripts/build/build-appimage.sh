#!/usr/bin/env bash
#
# 在 Debian 10（glibc 2.28）容器里构建 AppImage。
# 二进制仅 FFmpeg 静态；libxml2 连同依赖一起打包进 AppDir。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=common.sh
. "${SCRIPT_DIR}/scripts/build/common.sh"

PROJECT_NAME="ter-music"
REPO_ROOT="$SCRIPT_DIR"

show_help() {
    cat <<EOF
用法: $0 -v VERSION [选项]

构建 ter-music AppImage（Debian 10 / glibc 2.28）。

选项:
  -v, --version VERSION   必填；三段自动补 .0 为四段
  -a, --arch ARCH         仅支持 x86_64（默认 x86_64）
  -k, --keep-temp         保留 .tmp/appimage
      --rebuild-image     强制重建 Debian 10 镜像
      --no-cache          docker build 不使用缓存
  -h, --help              显示帮助

输出: build/appimage/x86_64/ter-music-<V>-x86_64.AppImage
EOF
}

VERSION=""
ARCH="x86_64"
KEEP_TEMP=false
REBUILD_IMAGE=false
NO_CACHE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        -v|--version) VERSION="${2:-}"; shift 2 ;;
        -a|--arch) ARCH="${2:-}"; shift 2 ;;
        -k|--keep-temp) KEEP_TEMP=true; shift ;;
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
        -f scripts/docker/Dockerfile.debian10
        -n ter-music-debian10
        -s build-appimage.sh
    )
    [ "$REBUILD_IMAGE" = true ] && xb_args+=(--rebuild)
    [ "$NO_CACHE" = true ] && xb_args+=(--no-cache)
    inner=(-v "$VERSION" -a "$ARCH")
    [ "$KEEP_TEMP" = true ] && inner+=(-k)
    exec "${REPO_ROOT}/scripts/docker/docker-build.sh" "${xb_args[@]}" -- "${inner[@]}"
fi

WORK="${REPO_ROOT}/.tmp/appimage"
BUILD_DIR="${WORK}/build"
APPDIR="${WORK}/AppDir"
OUT_DIR="${REPO_ROOT}/build/appimage/x86_64"
OUTPUT="${OUT_DIR}/${PROJECT_NAME}-${VERSION}-${ARCH}.AppImage"

rm -rf "$WORK"
mkdir -p "$BUILD_DIR" "$APPDIR" "$OUT_DIR"

tm_log_info "AppImage build: version=${VERSION} arch=${ARCH}"

(
    cd "$BUILD_DIR"
    cmake "$REPO_ROOT" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DSTATIC_LINKING=ON \
        -DTER_MUSIC_VERSION="$VERSION"
    make -j"$(nproc)"
    make install DESTDIR="$APPDIR"
)

# AppRun / desktop / AppInfo / icon
cat > "${APPDIR}/AppRun" <<'EOF'
#!/bin/bash
HERE=$(dirname "$(readlink -f "$0")")
export PATH="${HERE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH:-}"
exec "${HERE}/usr/bin/ter-music" "$@"
EOF
chmod +x "${APPDIR}/AppRun"

cat > "${APPDIR}/${PROJECT_NAME}.desktop" <<EOF
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

cat > "${APPDIR}/AppInfo.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<AppImage xmlns="http://www.appimage.org/1.0">
  <id>ter-music</id>
  <name>Ter-Music</name>
  <summary>A terminal-based music player with ncurses interface</summary>
  <version>${VERSION}</version>
  <release>1</release>
  <project_license>GPL-3.0-or-later</project_license>
  <developer_name>浣软科技（HuanSoft）</developer_name>
</AppImage>
EOF

cp -f "${REPO_ROOT}/resources/icons/hicolor/128x128/apps/ter-music.png" "${APPDIR}/ter-music.png"
cp -f "${REPO_ROOT}/resources/icons/hicolor/128x128/apps/ter-music.png" "${APPDIR}/.DirIcon"

# 递归复制动态库（跳过 glibc/loader），并额外打包 libxml2.so.2。
declare -A COPIED=()
LIB_DIR="${APPDIR}/usr/lib"
mkdir -p "$LIB_DIR"

copy_one_dep() {
    local dep="$1"
    [ -f "$dep" ] || return 0
    local name
    name="$(basename "$dep")"
    case "$name" in
        libc.so.*|ld-linux*|libpthread.so.*|libdl.so.*|librt.so.*|libm.so.*)
            return 0
            ;;
    esac
    if [ -n "${COPIED[$name]:-}" ]; then
        return 0
    fi
    COPIED[$name]=1
    cp -Lf "$dep" "${LIB_DIR}/${name}"
    local sub
    for sub in $(ldd "$dep" 2>/dev/null | awk '/=> \//{print $3}'); do
        copy_one_dep "$sub"
    done
}

copy_elf_deps() {
    local elf="$1"
    [ -f "$elf" ] || return 0
    local dep
    for dep in $(ldd "$elf" 2>/dev/null | awk '/=> \//{print $3}'); do
        copy_one_dep "$dep"
    done
}

copy_elf_deps "${APPDIR}/usr/bin/ter-music"

libxml2_path="$(find /usr/lib /lib -name 'libxml2.so.2' -type f 2>/dev/null | head -n 1)"
if [ -z "$libxml2_path" ]; then
    libxml2_path="$(find /usr/lib /lib -name 'libxml2.so.2' 2>/dev/null | head -n 1)"
fi
if [ -z "$libxml2_path" ]; then
    tm_log_error "libxml2.so.2 not found in build image"
    exit 1
fi
copy_one_dep "$libxml2_path"

APPIMAGETOOL="/opt/appimagetool/squashfs-root/usr/bin/appimagetool"
if [ ! -x "$APPIMAGETOOL" ]; then
    tm_log_error "appimagetool not found at $APPIMAGETOOL; 请用 --rebuild-image 重建镜像，或确认宿主 .cache/tools/appimagetool-x86_64.AppImage 可用"
    exit 1
fi

RUNTIME_FILE="/opt/appimagetool/runtime-x86_64"
if [ ! -s "$RUNTIME_FILE" ]; then
    tm_log_error "AppImage runtime not found at $RUNTIME_FILE; 请用 --rebuild-image 重建镜像，或确认宿主 .cache/tools/runtime-x86_64 可用"
    exit 1
fi

# Debian 10 的 squashfs-tools 4.3 不支持 -mkfs-time，镜像里已从源码装 4.7.5；
# runtime 随镜像提供，避免 appimagetool 联网下载失败。
# 新版 type2 runtime 只内建 zlib/zstd 解压支持，用 xz 打包会 mount 失败，
# 因此这里显式用 zstd。
ARCH="$ARCH" "$APPIMAGETOOL" --no-appstream --comp zstd \
    --runtime-file "$RUNTIME_FILE" "$APPDIR" "$OUTPUT"
chmod +x "$OUTPUT"

# 解包运行一次，验证 glibc 2.28 基线与数据文件可用。
VERIFY_DIR="${WORK}/verify"
mkdir -p "$VERIFY_DIR"
if ! (
    cd "$VERIFY_DIR"
    "$OUTPUT" --appimage-extract >/dev/null 2>&1
    ./squashfs-root/AppRun --version
); then
    tm_log_error "AppImage self-extract verification failed (runtime 与压缩算法不匹配？)"
    exit 1
fi

tm_copy_release "$OUTPUT"
tm_fix_ownership

if [ "$KEEP_TEMP" != true ]; then
    rm -rf "$WORK"
fi

echo ""
echo "=========================================="
echo "AppImage 构建完成"
echo "=========================================="
echo "产物: ${OUTPUT}"
ls -lh "$OUTPUT"
