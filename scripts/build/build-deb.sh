#!/usr/bin/env bash
#
# Debian 10 容器构建 DEB：仅 FFmpeg 静态；libxml2 运行期 dlopen。
# 默认只出二进制 deb；--with-source / --with-debuginfo 按需追加。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=common.sh
. "${SCRIPT_DIR}/scripts/build/common.sh"

PACKAGE_NAME="cn.org.huansoft.ter-music"
DEFAULT_ARCH="amd64"
REPO_ROOT="$SCRIPT_DIR"

show_help() {
    cat <<EOF
用法: $0 -v VERSION [选项]

构建 ter-music 的 DEB（Debian 10 容器，仅 FFmpeg 静态）。

选项:
  -v, --version VERSION   必填；三段自动补 .0 为四段
  -a, --arch ARCH         仅支持 amd64（默认 amd64）
  -k, --keep-temp         保留 .tmp/deb
      --with-source       额外生成 .dsc/.orig.tar.gz/.debian.tar.xz
      --with-debuginfo    额外生成 -dbgsym deb
      --rebuild-image     强制重建 Debian 10 镜像
      --no-cache          docker build 不使用缓存
  -h, --help              显示帮助

输出:
  二进制: build/deb/amd64/
  源码:   build/deb/source/（仅 --with-source）
EOF
}

VERSION=""
ARCH="$DEFAULT_ARCH"
KEEP_TEMP=false
WITH_SOURCE=false
WITH_DEBUGINFO=false
REBUILD_IMAGE=false
NO_CACHE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        -v|--version) VERSION="${2:-}"; shift 2 ;;
        -a|--arch) ARCH="${2:-}"; shift 2 ;;
        -k|--keep-temp) KEEP_TEMP=true; shift ;;
        --with-source) WITH_SOURCE=true; shift ;;
        --with-debuginfo) WITH_DEBUGINFO=true; shift ;;
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

if [ "$ARCH" != "amd64" ]; then
    tm_log_error "unsupported architecture: $ARCH (only amd64)"
    exit 1
fi

# 宿主机入口：委托 Docker；容器内继续执行实际构建。
if ! tm_is_container; then
    xb_args=(
        -f scripts/docker/Dockerfile.debian10
        -n ter-music-debian10
        -s build-deb.sh
    )
    [ "$REBUILD_IMAGE" = true ] && xb_args+=(--rebuild)
    [ "$NO_CACHE" = true ] && xb_args+=(--no-cache)
    inner=(-v "$VERSION" -a "$ARCH")
    [ "$KEEP_TEMP" = true ] && inner+=(-k)
    [ "$WITH_SOURCE" = true ] && inner+=(--with-source)
    [ "$WITH_DEBUGINFO" = true ] && inner+=(--with-debuginfo)
    exec "${REPO_ROOT}/scripts/docker/docker-build.sh" "${xb_args[@]}" -- "${inner[@]}"
fi

tm_log_info "DEB build: version=${VERSION} arch=${ARCH} source=${WITH_SOURCE} debug=${WITH_DEBUGINFO}"

# 内置消音模型：下载/校验到 .tmp/models 缓存并复用；debian/rules 通过
# TER_MUSIC_BUNDLED_MODEL 拿绝对路径注入，模型不复制进源码树。
tm_ensure_model || exit 1
export TER_MUSIC_BUNDLED_MODEL="$TM_MODEL_PATH"
tm_log_info "bundled karaoke model: ${TER_MUSIC_BUNDLED_MODEL}"

WORK="${REPO_ROOT}/.tmp/deb"
SRC="${WORK}/${PACKAGE_NAME}-${VERSION}"
OUT_BIN="${REPO_ROOT}/build/deb/amd64"
OUT_SRC="${REPO_ROOT}/build/deb/source"

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT_BIN" "$OUT_SRC"

tm_stage_source "$SRC"
# 模型复制进 staging 的 .tm-bundled-model/：debian/rules 在无环境变量时回退到
# 该路径，--with-source 产出的源码包因此自包含（模型不会进入 git）。
mkdir -p "${SRC}/.tm-bundled-model"
cp -f -- "$TM_MODEL_PATH" "${SRC}/.tm-bundled-model/${TM_MODEL_NAME}"
cd "$SRC"

# 用 dch 在临时副本里追加当前版本条目；失败时用 Python 兜底。
export DEBEMAIL="yxzl666xx@outlook.com"
export DEBFULLNAME="HuanSoft"
if ! dch --newversion "${VERSION}-1" --distribution unstable \
        --force-distribution "Automated build: ${VERSION}" >/dev/null 2>&1; then
    python3 - "$SRC/debian/changelog" "$PACKAGE_NAME" "$VERSION" <<'PY'
import datetime
import sys

path, pkg, ver = sys.argv[1:4]
with open(path, encoding="utf-8") as fh:
    old = fh.read()
stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%a, %d %b %Y %H:%M:%S +0000")
entry = (f"{pkg} ({ver}-1) unstable; urgency=medium\n\n"
         f"  * Automated build: {ver}\n\n"
         f" -- HuanSoft <yxzl666xx@outlook.com>  {stamp}\n\n")
with open(path, "w", encoding="utf-8") as fh:
    fh.write(entry + old)
PY
fi

cd "$WORK"

if [ "$WITH_SOURCE" = true ]; then
    tm_log_info "phase 1: source package"
    base="$(basename "$SRC")"
    orig="${WORK}/${PACKAGE_NAME}_${VERSION}.orig.tar.gz"
    # 只排除 debian/：dpkg-source 会用上游 tarball 与工作树逐文件比对，
    # 少打包任何上游文件都会被判为“本地改动”而中止源码包构建。
    tar -czf "$orig" -C "$WORK" \
        --exclude="${base}/debian" \
        "$base"
    ( cd "$SRC" && dpkg-buildpackage -S -us -uc )
    found_src=0
    for f in "$WORK"/*.dsc "$WORK"/*.orig.tar.* "$WORK"/*.debian.tar.*; do
        [ -f "$f" ] || continue
        cp -f "$f" "$OUT_SRC/"
        tm_copy_release "$f"
        found_src=$((found_src + 1))
    done
    if [ "$found_src" -eq 0 ]; then
        tm_log_error "--with-source requested but no source package was produced"
        exit 1
    fi
fi

tm_log_info "phase 2: binary package"
export DEB_BUILD_OPTIONS="nocheck"
if [ "$WITH_DEBUGINFO" != true ]; then
    export DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS} noautodbgsym"
fi
( cd "$SRC" && dpkg-buildpackage -b -us -uc )

found_bin=0
found_dbg=0
while IFS= read -r -d '' deb; do
    name="$(basename "$deb")"
    case "$name" in
        *-dbgsym_*)
            if [ "$WITH_DEBUGINFO" = true ]; then
                cp -f "$deb" "$OUT_BIN/"
                tm_copy_release "$deb"
                found_dbg=$((found_dbg + 1))
            fi
            ;;
        *)
            cp -f "$deb" "$OUT_BIN/"
            tm_copy_release "$deb"
            found_bin=$((found_bin + 1))
            ;;
    esac
done < <(find "$WORK" -maxdepth 1 -name '*.deb' -type f -print0)

if [ "$found_bin" -eq 0 ]; then
    tm_log_error "no binary deb produced"
    exit 1
fi
if [ "$WITH_DEBUGINFO" = true ] && [ "$found_dbg" -eq 0 ]; then
    tm_log_error "--with-debuginfo requested but no -dbgsym package was produced"
    exit 1
fi

# ABI 校验：不能有 libav* / libxml2 的动态依赖。
main_deb="$(find "$OUT_BIN" -maxdepth 1 -name "${PACKAGE_NAME}_${VERSION}-1_${ARCH}.deb" -type f | head -n 1)"
if [ -z "$main_deb" ]; then
    tm_log_error "main deb not found in $OUT_BIN"
    exit 1
fi
depends="$(dpkg-deb -f "$main_deb" Depends)"
if ! printf '%s\n' "$depends" | grep -q 'libxml2'; then
    tm_log_error "main deb is missing libxml2 dependency alternative"
    exit 1
fi
if printf '%s\n' "$depends" | grep -q 'libav'; then
    tm_log_error "main deb has libav* runtime dependency (FFmpeg is not static)"
    exit 1
fi
extract="${WORK}/extract"
rm -rf "$extract"
dpkg-deb -x "$main_deb" "$extract"

# 安装布局（Linyaps 目录规范）：实体在 /opt/apps/<appid>/files，/usr 只有链接。
opt_root="${extract}/opt/apps/${PACKAGE_NAME}/files"
if [ ! -x "${opt_root}/bin/ter-music" ]; then
    tm_log_error "binary is not installed at /opt/apps/${PACKAGE_NAME}/files/bin/ter-music"
    exit 1
fi
if readelf -d "${opt_root}/bin/ter-music" | grep -E 'NEEDED.*(libav|libxml2)' >/dev/null; then
    tm_log_error "binary has libav*/libxml2 DT_NEEDED"
    exit 1
fi

# 内置模型 + 第三方 NOTICE 必须随包分发且内容正确。
model_path="${opt_root}/share/ter-music/models/uvr-mdxnet-3-9662.onnx"
if [ ! -f "$model_path" ]; then
    tm_log_error "deb is missing bundled karaoke model: ${model_path}"
    exit 1
fi
model_size="$(wc -c < "$model_path" | tr -d ' \n')"
if [ "$model_size" != "$TM_MODEL_SIZE" ]; then
    tm_log_error "bundled model size ${model_size} != expected ${TM_MODEL_SIZE}"
    exit 1
fi
model_sha="$(tm_sha256_of "$model_path")"
if [ "$model_sha" != "$TM_MODEL_SHA256" ]; then
    tm_log_error "bundled model sha256 ${model_sha} != expected ${TM_MODEL_SHA256}"
    exit 1
fi
if [ ! -f "${opt_root}/share/ter-music/models/NOTICE.md" ]; then
    tm_log_error "deb is missing third-party notice: ${opt_root}/share/ter-music/models/NOTICE.md"
    exit 1
fi
for data in \
    "${opt_root}/share/ter-music/lang/zh_CN.xml" \
    "${opt_root}/share/ter-music/lang/en_US.xml" \
    "${opt_root}/share/ter-music/lang/zh-PQ.xml" \
    "${opt_root}/share/ter-music/help/help-quickstart-zh_CN.txt" \
    "${opt_root}/share/ter-music/help/help-quickstart-en_US.txt" \
    "${opt_root}/share/ter-music/help/help-quickstart-zh-PQ.txt" \
    "${opt_root}/share/ter-music/layouts/layouts.json" \
    "${opt_root}/share/applications/ter-music.desktop"; do
    if [ ! -f "$data" ]; then
        tm_log_error "deb is missing packaged data file: ${data#${extract}}"
        exit 1
    fi
done

deb_listing_file="${WORK}/deb-listing.txt"
dpkg-deb -c "$main_deb" > "$deb_listing_file"
assert_deb_path() { # <正则> <说明>
    if ! grep -Eq "$1" "$deb_listing_file"; then
        tm_log_error "deb layout check failed: $2 (pattern: $1)"
        exit 1
    fi
}
assert_deb_path "^-.* \./opt/apps/${PACKAGE_NAME}/files/bin/ter-music$" \
    "实体二进制必须装在 /opt/apps/<appid>/files/bin"
assert_deb_path " \./usr/bin/ter-music -> .*opt/apps/${PACKAGE_NAME}/files/bin/ter-music$" \
    "/usr/bin/ter-music 必须是指向 opt 实体的符号链接"
assert_deb_path " \./usr/share/applications/ter-music\.desktop -> " \
    "desktop 必须是指向 opt 实体的符号链接"
assert_deb_path " \./usr/share/icons/hicolor/(32x32|48x48|128x128)/apps/ter-music\.png -> " \
    "PNG 图标必须是符号链接"
assert_deb_path " \./usr/share/icons/hicolor/scalable/apps/ter-music\.svg -> " \
    "SVG 图标必须是符号链接"
assert_deb_path "^-.* \./opt/apps/${PACKAGE_NAME}/files/share/ter-music/models/uvr-mdxnet-3-9662\.onnx$" \
    "内置模型必须以普通文件随包安装"
if grep -Eq "^-.* \./usr/bin/ter-music$" "$deb_listing_file"; then
    tm_log_error "deb still ships a real /usr/bin/ter-music file"
    exit 1
fi
if grep -Eq "^-.* \./usr/share/ter-music/" "$deb_listing_file"; then
    tm_log_error "deb still ships real data files under /usr/share/ter-music"
    exit 1
fi
tm_log_info "deb layout verified: /opt entity + /usr symlinks + bundled model + NOTICE"

tm_fix_ownership
if [ "$KEEP_TEMP" != true ]; then
    rm -rf "$WORK"
fi

echo ""
echo "=========================================="
echo "DEB 构建完成"
echo "=========================================="
echo "版本: ${VERSION}"
echo "二进制: ${OUT_BIN}/"
ls -lh "$OUT_BIN"/*.deb 2>/dev/null || true
if [ "$WITH_SOURCE" = true ]; then
    echo "源码: ${OUT_SRC}/"
    ls -lh "$OUT_SRC"/* 2>/dev/null || true
fi
