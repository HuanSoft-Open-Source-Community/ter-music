#!/usr/bin/env bash
#
# 静态化 ONNX Runtime：构建 PIC 静态库（合并后的 libonnxruntime.a），
# 供 deb / rpm / AppImage 构建在**没有系统 libonnxruntime** 的环境里内置消音。
#
# 背景（2026-11-27 发布前审计 B3）：
#   - 模型随包内置，但运行库外置：Debian 只有 trixie(13)+ 提供
#     libonnxruntime1.21，Debian 10/11/12 与 EL7–10 都没有；AppImage 也不自带。
#     结果"装得上、卖点用不了"。
#   - 上游 microsoft/onnxruntime 与 download.onnxruntime.ai 自 2026-11 已 404，
#     不能作为构建依赖；因此源码取自 Debian pool 的 1.21.0+dfsg orig.tar.xz，
#     并按 .dsc 的 Checksums-Sha256 校验（见下方 *_SHA256 常量）。
#
# 产物（.cache/tools/onnxruntime-static/，不进 git）：
#   libonnxruntime.a        合并后的单一静态库（PIC，glibc 2.17 符号上限）
#   cxx/libstdc++.a         链接期需要的 C++ 运行时归档（见下）
#   deps-manifest.txt        合并了哪些 ORT 子库/第三方库、顺序、大小
#   PROVENANCE.txt           源码 sha256、工具链与 glibc 兼容结论、构建参数
#   build.log / cmake-configure.log  完整日志（失败时优先看这两个）
#
# 为什么额外带 libstdc++.a：ORT 是 C++，ter-music 是纯 C。用 EL7 的
# devtoolset-11 构建可以得到 glibc 2.17 上限的静态库，但目标文件引用的是
# GCC 11 的 libstdc++ 符号（GLIBCXX_3.4.29）。Debian 10 的系统 libstdc++
# 只到 3.4.25，所以最终链接必须静态带上这份归档；否则 debian10/EL7 包都链不上。
# 它只参与链接，不进入发布产物（不进 DT_NEEDED）。
#
# 用法:
#   scripts/build/build-onnxruntime-static.sh              # 缺产物才构建（幂等）
#   scripts/build/build-onnxruntime-static.sh --force      # 强制重建库（复用源/依赖缓存）
#   scripts/build/build-onnxruntime-static.sh --clean      # 删除构建缓存与产物后退出
#   scripts/build/build-onnxruntime-static.sh --rebuild-image   # 强制重建构建镜像
#   scripts/build/build-onnxruntime-static.sh --flavor debian10  # 降级：Debian 10 工具链
#
# 依赖：docker（宿主）。本机若缺 docker，脚本给出可读错误并退出。
#
# shellcheck shell=bash disable=SC2317

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=../build/common.sh
. "${SCRIPT_DIR}/scripts/build/common.sh"

# ── 固定事实（改动前先读上面的背景）──────────────────────────────
ORT_VERSION="1.21.0"
ORT_DEBIAN_REV="1"
ORT_ORIG_NAME="onnxruntime_${ORT_VERSION}+dfsg.orig.tar.xz"
ORT_DEBIAN_NAME="onnxruntime_${ORT_VERSION}+dfsg-${ORT_DEBIAN_REV}.debian.tar.xz"
ORT_DSC_NAME="onnxruntime_${ORT_VERSION}+dfsg-${ORT_DEBIAN_REV}.dsc"
ORT_POOL_URL="https://deb.debian.org/debian/pool/main/o/onnxruntime"
# Debian 包的 Checksums-Sha256（取自 onnxruntime_1.21.0+dfsg-1.dsc）。
ORT_ORIG_SHA256="32df1f82bd6b07b18372c0f387133214905cbde7a586b3ad83f0ed0075f80273"
ORT_ORIG_SIZE=37970624
ORT_DEBIAN_SHA256="8bd81c87dea02ed3d27cc479a4fb1c1ae7f2f5bd5fc8c573cb4d5c8a052889bf"
ORT_DSC_SHA256="a5bb75f32a2e2e18b36d36e5f1737517a252aee4aefe7362624a0f152cda08ea"

# CMake 3.31.6：ORT 1.21 要求 >= 3.28，Debian 10 自带 3.13、EL7 的 cmake3 是
# 3.17，都不够。官方 linux-x86_64 二进制需要 GLIBC_2.17 / GLIBCXX_3.4.19
# （= CentOS 7 基线），两个构建镜像都能直接跑。这里用 pypi 的
# manylinux_2_17_x86_64 wheel（阿里云镜像，几十 MB，秒级），解包出 cmake/data。
CMAKE_VERSION="3.31.6"
CMAKE_WHEEL_SHA256="1c8b05df0602365da91ee6a3336fe57525b137706c4ab5675498f662ae1dbcec"
CMAKE_WHEEL_URL="https://mirrors.aliyun.com/pypi/packages/59/e8/096984b89133681533650b9078c5ed1c5c9b534e869b5487f22d4de1935c/cmake-${CMAKE_VERSION}-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl"

# Eigen：ORT 的 cmake/deps.txt 指向 gitlab.com，实测 403（不可依赖）。
# 换成 Debian 的 eigen3 3.4.0 orig 源码（同一上游 3.4.0），用
# FETCHCONTENT_SOURCE_DIR_EIGEN 直接喂给 FetchContent，不再联网。
EIGEN_VERSION="3.4.0"
EIGEN_NAME="eigen3_${EIGEN_VERSION}.orig.tar.bz2"
EIGEN_SHA256="b4c198460eba6f28d34894e3a5710998818515104d6e74e5cc331ce31e46e626"  # pragma: allowlist secret
EIGEN_POOL_URL="https://deb.debian.org/debian/pool/main/e/eigen3/${EIGEN_NAME}"

DEFAULT_FLAVOR="centos7"

# ── 路径 ──────────────────────────────────────────────────────────
SOURCES_DIR="${TM_REPO_ROOT}/.cache/sources"
CACHE_ROOT="${TM_REPO_ROOT}/.cache/tools/onnxruntime-static"
WORK_DIR="${CACHE_ROOT}/work"
ORT_SRC="${WORK_DIR}/onnxruntime-${ORT_VERSION}"
BUILD_DIR="${WORK_DIR}/build"
DEPS_DIR="${WORK_DIR}/deps"
EIGEN_SRC="${DEPS_DIR}/eigen-${EIGEN_VERSION}"
STAMP_DIR="${WORK_DIR}/.stamps"
DOCKER_CONTEXT="${TM_REPO_ROOT}/.cache/onnxruntime-build/context"
OUT_LIB="${CACHE_ROOT}/libonnxruntime.a"
OUT_CXX_DIR="${CACHE_ROOT}/cxx"
OUT_CXX_LIB="${OUT_CXX_DIR}/libstdc++.a"
PROVENANCE="${CACHE_ROOT}/PROVENANCE.txt"
DEPS_MANIFEST="${CACHE_ROOT}/deps-manifest.txt"
BUILD_LOG="${CACHE_ROOT}/build.log"
CONFIG_LOG="${CACHE_ROOT}/cmake-configure.log"
# 供 CMake / Dockerfile / docker-build.sh 复用的约定路径。
ORT_STATIC_INSTALL_DIR="/usr/local/lib/onnxruntime-static"

show_help() {
    cat <<'EOF'
用法: build-onnxruntime-static.sh [选项]

构建 ter-music 内置消音用的 ONNX Runtime 静态库（合并后的 libonnxruntime.a）。

选项:
  --force            强制重新构建静态库（源/依赖/镜像缓存仍然复用）
  --clean            删除 .cache/tools/onnxruntime-static 下的构建缓存与产物后退出
  --ensure           仅当产物缺失/无效时构建（默认行为，docker-build.sh 内部调用）
  --flavor FLAVOR    构建工具链：centos7（默认，glibc 2.17）| debian10（glibc 2.28，降级）
  --rebuild-image    强制重建构建镜像
  --skip-build       复用已有 build 目录，只重做合并/自检（调试用）
  --in-container     内部使用：在构建容器内执行真正的 cmake 构建
  -h, --help         显示帮助

产物: .cache/tools/onnxruntime-static/{libonnxruntime.a,cxx/libstdc++.a,PROVENANCE.txt}

示例:
  ./scripts/build/build-onnxruntime-static.sh              # 幂等：有产物就直接复用
  ./scripts/build/build-onnxruntime-static.sh --force      # 重新构建
EOF
}

FLAVOR="${TER_MUSIC_ORT_FLAVOR:-$DEFAULT_FLAVOR}"
FORCE=false
CLEAN=false
REBUILD_IMAGE=false
SKIP_BUILD=false
IN_CONTAINER=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        --force) FORCE=true; shift ;;
        --clean) CLEAN=true; shift ;;
        --ensure) shift ;;
        --flavor) FLAVOR="${2:-}"; shift 2 ;;
        --rebuild-image) REBUILD_IMAGE=true; shift ;;
        --skip-build) SKIP_BUILD=true; shift ;;
        --in-container) IN_CONTAINER=true; shift ;;
        *)
            tm_log_error "unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

case "$FLAVOR" in
    centos7) IMAGE_NAME="ter-music-onnxruntime-build:centos7" ;;
    debian10) IMAGE_NAME="ter-music-onnxruntime-build:debian10" ;;
    *)
        tm_log_error "unsupported flavor: $FLAVOR (expected centos7 or debian10)"
        exit 1
        ;;
esac

# ── 工具函数 ──────────────────────────────────────────────────────

# 已下载文件是否与期望 sha256 一致（大小不符直接返回失败，省一次全量哈希）。
tm_ort_file_ok() {
    local path="${1:-}"
    local want_sha="${2:-}"
    local want_size="${3:-0}"
    [ -f "$path" ] || return 1
    if [ "${want_size:-0}" -gt 0 ] && [ "$(tm_file_size "$path")" != "$want_size" ]; then
        return 1
    fi
    [ "$(tm_sha256_of "$path")" = "$want_sha" ]
}

# tm_ort_fetch <path> <url> [sha256] [size]
tm_ort_fetch() {
    local path="${1:-}"
    local url="${2:-}"
    local want_sha="${3:-}"
    local want_size="${4:-0}"
    if [ -n "$want_sha" ] && tm_ort_file_ok "$path" "$want_sha" "$want_size"; then
        tm_log_info "cached: $(basename "$path")"
        return 0
    fi
    mkdir -p "$(dirname "$path")"
    rm -f "$path"
    local tmp="${path}.part.$$"
    tm_log_info "downloading: ${url}"
    if ! tm_download_to "$url" "$tmp"; then
        rm -f "$tmp"
        tm_log_error "download failed: ${url}"
        return 1
    fi
    if [ -n "$want_sha" ] && ! tm_ort_file_ok "$tmp" "$want_sha" "$want_size"; then
        local got_sha got_size
        got_sha="$(tm_sha256_of "$tmp" 2>/dev/null || true)"
        got_size="$(tm_file_size "$tmp")"
        rm -f "$tmp"
        tm_log_error "checksum mismatch for $(basename "$path"): size=${got_size} sha256=${got_sha:-?}"
        tm_log_error "expected size=${want_size:-?} sha256=${want_sha}"
        return 1
    fi
    mv -f "$tmp" "$path"
}

# ORT 源码包（orig + debian + dsc）：sha256 全部锚定 Debian 包的 .dsc。
tm_ort_ensure_sources() {
    mkdir -p "$SOURCES_DIR"
    tm_ort_fetch "${SOURCES_DIR}/${ORT_ORIG_NAME}" \
        "${ORT_POOL_URL}/${ORT_ORIG_NAME}" "$ORT_ORIG_SHA256" "$ORT_ORIG_SIZE" || return 1
    tm_ort_fetch "${SOURCES_DIR}/${ORT_DEBIAN_NAME}" \
        "${ORT_POOL_URL}/${ORT_DEBIAN_NAME}" "$ORT_DEBIAN_SHA256" || return 1
    tm_ort_fetch "${SOURCES_DIR}/${ORT_DSC_NAME}" \
        "${ORT_POOL_URL}/${ORT_DSC_NAME}" "$ORT_DSC_SHA256" || return 1

    # .dsc 里同时列了两份 Checksums-Sha256：交叉核对一次，避免常量写错。
    if command -v awk >/dev/null 2>&1; then
        local listed
        listed="$(awk '/^Checksums-Sha256:/{f=1;next} /^[A-Za-z-]+:/{f=0} f && NF>=3 {print $3" "$1}' \
            "${SOURCES_DIR}/${ORT_DSC_NAME}")"
        if ! printf '%s\n' "$listed" | grep -qx "${ORT_ORIG_NAME} ${ORT_ORIG_SHA256}"; then
            tm_log_error ".dsc does not list expected orig sha256: ${ORT_ORIG_SHA256}"
            return 1
        fi
    fi
    tm_log_info "source tarballs verified against ${ORT_DSC_NAME}"
}

# ── 第三方依赖预取（github.com 归档下载在本环境不可用，codeload 可用）──
#
# ORT 的 cmake/deps.txt 用 https://github.com/<org>/<repo>/archive/...zip；
# 2026-11 实测 github.com 的归档跳转会挂死（curl 直接超时，返回 000），而
# codeload.github.com 有 2MB/s 以上。这里在宿主把真正需要的依赖预取到
# .cache/sources/ort-deps/（按 deps.txt 里的 SHA1 校验），解包到 work/deps/，
# 构建容器里用 FETCHCONTENT_SOURCE_DIR_<NAME> 指过去；并开
# FETCHCONTENT_FULLY_DISCONNECTED，漏掉哪个依赖会立刻报错而不是静默挂住。
#
# 只预取 CPU-only 静态构建实际会用到的依赖（其余属于 CUDA/XNNPACK/training
# /WebGPU 等分支）。列表格式：<FetchContent 名>:<deps.txt 键>。
ORT_DEP_MAP=(
    "abseil_cpp:abseil_cpp"
    "re2:re2"
    "utf8_range:utf8_range"
    "Protobuf:protobuf"
    "date:date"
    "mp11:mp11"
    "nlohmann_json:json"
    "pytorch_cpuinfo:pytorch_cpuinfo"
    "GSL:microsoft_gsl"
    "safeint:safeint"
    "flatbuffers:flatbuffers"
    "onnx:onnx"
)
ORT_DEPS_CACHE="${SOURCES_DIR}/ort-deps"

# 从 deps.txt 取 <key> 的 "url sha1"。
tm_ort_dep_lookup() {
    awk -F';' -v key="${1:-}" '$1==key {print $2" "$3; exit}' "${ORT_SRC}/cmake/deps.txt"
}

# github 归档 URL 换成 codeload（同一份内容，实测唯一稳定可达的路径）。
tm_ort_codeload_url() {
    local url="${1:-}"
    case "$url" in
        https://github.com/*/archive/refs/tags/*.zip)
            printf 'https://codeload.github.com/%s\n' \
                "$(printf '%s' "${url#https://github.com/}" |
                    sed -E 's#^([^/]+/[^/]+)/archive/refs/tags/(.+)\.zip$#\1/zip/refs/tags/\2#')"
            ;;
        https://github.com/*/archive/*.zip)
            printf 'https://codeload.github.com/%s\n' \
                "$(printf '%s' "${url#https://github.com/}" |
                    sed -E 's#^([^/]+/[^/]+)/archive/(.+)\.zip$#\1/zip/\2#')"
            ;;
        *) printf '%s\n' "$url" ;;
    esac
}

# 解包 zip 到 dest（去掉压缩包唯一的顶层目录；zip 不保留执行位，给 .sh 补上）。
tm_ort_unzip_stripped() {
    local zip="${1:-}"
    local dest="${2:-}"
    python3 - "$zip" "$dest" <<'PY'
import os
import stat
import sys
import zipfile

zip_path, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zip_path) as zf:
    names = [n for n in zf.namelist() if n and not n.startswith("__MACOSX/")]
    roots = {n.split("/", 1)[0] for n in names}
    strip = len(roots) == 1
    for name in names:
        if strip:
            rel = name.split("/", 1)[1] if "/" in name else ""
        else:
            rel = name
        if not rel:
            continue
        target = os.path.join(dest, rel)
        if rel.endswith("/"):
            os.makedirs(target, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with zf.open(name) as src, open(target, "wb") as dst:
            dst.write(src.read())
        if rel.endswith(".sh"):
            mode = os.stat(target).st_mode
            os.chmod(target, mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
PY
}

# 说明：ORT 对 protobuf / flatbuffers 有 PATCH_COMMAND，但都是从 Git 归档源
# 打 MSVC 开关 / -Wno-error 的补丁（Linux 上无影响）；用
# FETCHCONTENT_SOURCE_DIR 直接指源时 FetchContent 会跳过下载与打补丁，
# 这里显式记录该事实，避免后来者以为补丁被吞了。
tm_ort_prefetch_deps() {
    mkdir -p "$DEPS_DIR" "$ORT_DEPS_CACHE"
    local entry fetch_name dep_key meta url sha1 codeload_url zip dest marker
    for entry in "${ORT_DEP_MAP[@]}"; do
        fetch_name="${entry%%:*}"
        dep_key="${entry##*:}"
        dest="${DEPS_DIR}/${fetch_name}"
        meta="$(tm_ort_dep_lookup "$dep_key")"
        if [ -z "$meta" ]; then
            tm_log_error "dependency '${dep_key}' not found in cmake/deps.txt"
            return 1
        fi
        url="${meta% *}"
        sha1="${meta##* }"
        marker="${dep_key}:${sha1}"
        if [ -f "${dest}/.prefetched" ] && [ "$(cat "${dest}/.prefetched")" = "$marker" ]; then
            continue
        fi

        zip="${ORT_DEPS_CACHE}/${dep_key}.zip"
        if [ -f "$zip" ] && [ "$(sha1sum "$zip" | awk '{print $1}')" = "$sha1" ]; then
            tm_log_info "cached dep: ${dep_key}"
        else
            rm -f "$zip"
            codeload_url="$(tm_ort_codeload_url "$url")"
            tm_log_info "fetching dep ${dep_key}: ${codeload_url}"
            if ! tm_download_to "$codeload_url" "${zip}.part.$$"; then
                tm_log_error "failed to download ${dep_key} from ${codeload_url}"
                rm -f "${zip}.part.$$"
                return 1
            fi
            if [ "$(sha1sum "${zip}.part.$$" | awk '{print $1}')" != "$sha1" ]; then
                tm_log_error "SHA1 mismatch for ${dep_key}: expected ${sha1}"
                rm -f "${zip}.part.$$"
                return 1
            fi
            mv -f "${zip}.part.$$" "$zip"
        fi

        rm -rf "$dest"
        mkdir -p "$dest"
        tm_ort_unzip_stripped "$zip" "$dest"
        printf '%s\n' "$marker" > "${dest}/.prefetched"
        tm_log_info "dep ready: ${fetch_name} -> ${dest}"
    done
    tm_log_info "third-party sources prefetched (${#ORT_DEP_MAP[@]} deps)"
}

# 构建容器里把预取源复制成 build 目录内的独立副本：ORT 部分依赖带
# PATCH_COMMAND（哪怕当前用不上），指向共享缓存会被就地改写，不好重跑。
tm_ort_prepare_dep_sources() {
    local dep_src="${BUILD_DIR}/dep-src"
    mkdir -p "$dep_src"
    local entry fetch_name
    for entry in "${ORT_DEP_MAP[@]}"; do
        fetch_name="${entry%%:*}"
        [ -d "${dep_src}/${fetch_name}" ] && continue
        if [ ! -d "${DEPS_DIR}/${fetch_name}" ]; then
            tm_log_error "prefetched dependency missing: ${DEPS_DIR}/${fetch_name}"
            return 1
        fi
        cp -a "${DEPS_DIR}/${fetch_name}" "${dep_src}/${fetch_name}"
    done
    if [ ! -d "${dep_src}/eigen" ]; then
        cp -a "$EIGEN_SRC" "${dep_src}/eigen"
    fi
    tm_log_info "dependency sources staged under ${dep_src}"
}

# Eigen 源码：解包到 work/deps/eigen-3.4.0（FetchContent 直接引用）。
tm_ort_ensure_eigen() {    tm_ort_fetch "${SOURCES_DIR}/${EIGEN_NAME}" "$EIGEN_POOL_URL" "$EIGEN_SHA256" || return 1
    if [ -f "${EIGEN_SRC}/Eigen/Core" ] || [ -f "${EIGEN_SRC}/CMakeLists.txt" ]; then
        tm_log_info "eigen source ready: ${EIGEN_SRC}"
        return 0
    fi
    tm_log_info "extracting ${EIGEN_NAME} -> ${DEPS_DIR}"
    mkdir -p "$DEPS_DIR"
    rm -rf "$EIGEN_SRC"
    tar -xf "${SOURCES_DIR}/${EIGEN_NAME}" -C "$DEPS_DIR"
    local extracted
    extracted="$(find "$DEPS_DIR" -maxdepth 1 -type d -name 'eigen*' ! -name "eigen-${EIGEN_VERSION}" | head -1)"
    if [ -n "$extracted" ]; then
        rm -rf "$EIGEN_SRC"
        mv "$extracted" "$EIGEN_SRC"
    fi
    if [ ! -f "${EIGEN_SRC}/CMakeLists.txt" ]; then
        tm_log_error "eigen source layout unexpected: ${EIGEN_SRC}"
        return 1
    fi
    tm_log_info "eigen source ready: ${EIGEN_SRC}"
}

# CMake 官方二进制：从 pypi wheel 解出 cmake/data，重打包成 Dockerfile 需要的
# 单文件 tar.gz（保持官方 tarball 的顶层布局：bin/ share/ ...）。
tm_ort_ensure_cmake() {
    local tarball="${DOCKER_CONTEXT}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz"
    local wheel="${SOURCES_DIR}/cmake-${CMAKE_VERSION}-manylinux2014-x86_64.whl"
    if [ -s "$tarball" ] && tar -tzf "$tarball" >/dev/null 2>&1; then
        tm_log_info "cached: $(basename "$tarball")"
        return 0
    fi
    tm_ort_fetch "$wheel" "$CMAKE_WHEEL_URL" "$CMAKE_WHEEL_SHA256" || return 1
    mkdir -p "$DOCKER_CONTEXT"
    local tmpdir
    tmpdir="$(mktemp -d)"
    # wheel 是 zip：python3 解出 cmake/data，再用 tar 重打包（zip 不保留执行位）。
    python3 - "$wheel" "$tmpdir" <<'PY'
import sys
import zipfile

wheel, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(wheel) as zf:
    for name in zf.namelist():
        if not name.startswith("cmake/data/"):
            continue
        rel = name[len("cmake/data/"):]
        if not rel:
            continue
        target = sys.argv[2] + "/" + rel
        import os
        if name.endswith("/"):
            os.makedirs(target, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with zf.open(name) as src, open(target, "wb") as dst:
            dst.write(src.read())
PY
    if [ ! -x "${tmpdir}/bin/cmake" ]; then
        chmod +x "${tmpdir}"/bin/* 2>/dev/null || true
    fi
    if [ ! -f "${tmpdir}/bin/cmake" ]; then
        tm_log_error "cmake wheel layout unexpected (no bin/cmake)"
        rm -rf "$tmpdir"
        return 1
    fi
    chmod +x "${tmpdir}"/bin/* 2>/dev/null || true
    local tmp_tar="${tarball}.part.$$"
    tar -czf "$tmp_tar" -C "$tmpdir" --owner=0 --group=0 .
    mv -f "$tmp_tar" "$tarball"
    rm -rf "$tmpdir"
    tm_log_info "cmake ${CMAKE_VERSION} packaged for image: ${tarball}"
}

# 给 ORT 源码打 Debian 的兼容补丁（幂等）。
#
# 为什么需要：ORT 1.21 的 element_wise_ops.cc 用 Eigen 的
# `min/max<Eigen::PropagateNaN>`，这个模板参数只存在于 ORT cmake/deps.txt
# 钉的 Eigen 上游提交（1d8b82b…，3.4.0 之后），而 Debian 的 eigen3 3.4.0
# 发行版没有它；gitlab.com 在本环境 403，拿不到那个提交。Debian 自己的
# onnxruntime 包就是用这个补丁编过的（debian/patches/
# disable-propagate-nan-in-eigen3.patch），这里同样采用。
# 其余 debian/patches 要么是 system-*（我们用 ORT 自带 deps）、要么是
# Windows/ARM/ppc64le/pybind11 相关，与本次 CPU 静态构建无关。
tm_ort_apply_debian_patches() {
    local marker="${STAMP_DIR}/debian-patches-applied"
    local debian_dir="${WORK_DIR}/debian-src"
    local patch_name="disable-propagate-nan-in-eigen3.patch"
    local patch_file="${debian_dir}/debian/patches/${patch_name}"

    if [ ! -f "$patch_file" ]; then
        mkdir -p "$debian_dir"
        tm_log_info "extracting ${ORT_DEBIAN_NAME} -> ${debian_dir}"
        rm -rf "$debian_dir"
        mkdir -p "$debian_dir"
        tar -xf "${SOURCES_DIR}/${ORT_DEBIAN_NAME}" -C "$debian_dir"
    fi
    if [ ! -f "$patch_file" ]; then
        tm_log_error "missing Debian patch: ${patch_file}"
        return 1
    fi
    if [ -f "$marker" ] && [ "$(cat "$marker")" = "$patch_name" ]; then
        return 0
    fi
    if patch -p1 --dry-run -d "$ORT_SRC" -i "$patch_file" >/dev/null 2>&1; then
        tm_log_info "applying Debian patch: ${patch_name}"
        patch -p1 -d "$ORT_SRC" -i "$patch_file" >/dev/null || return 1
    elif patch -p1 -R --dry-run -d "$ORT_SRC" -i "$patch_file" >/dev/null 2>&1; then
        tm_log_info "Debian patch already applied: ${patch_name}"
    else
        tm_log_error "Debian patch does not apply cleanly: ${patch_name}"
        return 1
    fi
    mkdir -p "$STAMP_DIR"
    printf '%s\n' "$patch_name" > "$marker"
}

# ORT 源码解包（work/onnxruntime-1.21.0）。
tm_ort_ensure_extracted() {
    local stamp="${STAMP_DIR}/extracted"
    if [ -f "$stamp" ] && [ -f "${ORT_SRC}/cmake/CMakeLists.txt" ] &&
       [ "$(cat "$stamp" 2>/dev/null)" = "$ORT_ORIG_SHA256" ]; then
        tm_log_info "source tree ready: ${ORT_SRC}"
        tm_ort_apply_debian_patches || return 1
        return 0
    fi
    tm_log_info "extracting ${ORT_ORIG_NAME} -> ${WORK_DIR}"
    mkdir -p "$WORK_DIR"
    rm -rf "$ORT_SRC"
    tar -xf "${SOURCES_DIR}/${ORT_ORIG_NAME}" -C "$WORK_DIR"
    if [ ! -f "${ORT_SRC}/cmake/CMakeLists.txt" ]; then
        tm_log_error "unexpected ONNX Runtime source layout: ${ORT_SRC}"
        return 1
    fi
    mkdir -p "$STAMP_DIR"
    printf '%s\n' "$ORT_ORIG_SHA256" > "$stamp"
    tm_ort_apply_debian_patches || return 1
    tm_log_info "source tree ready: ${ORT_SRC}"
}

tm_ort_ensure_image() {
    local docker_cmd=(env "DOCKER_CONFIG=${TM_REPO_ROOT}/.cache/docker-buildx" docker)
    if [ "$REBUILD_IMAGE" = false ] && "${docker_cmd[@]}" image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
        tm_log_info "using existing build image: ${IMAGE_NAME}"
        return 0
    fi
    tm_log_info "building ONNX Runtime build image ${IMAGE_NAME} (flavor=${FLAVOR})"
    if [ "$REBUILD_IMAGE" = true ]; then
        "${docker_cmd[@]}" rmi -f "$IMAGE_NAME" >/dev/null 2>&1 || true
    fi
    if ! "${docker_cmd[@]}" build \
            -f "${TM_REPO_ROOT}/scripts/docker/Dockerfile.onnxruntime-static" \
            -t "$IMAGE_NAME" \
            --build-arg "ORT_BUILD_FLAVOR=${FLAVOR}" \
            --build-arg "CMAKE_VERSION=${CMAKE_VERSION}" \
            "$DOCKER_CONTEXT"; then
        tm_log_error "failed to build image ${IMAGE_NAME}"
        return 1
    fi
    tm_log_info "build image ready: ${IMAGE_NAME}"
}

# 产物是否可用：静态库存在、能列出成员、含 OrtGetApiBase，且来源/工具链匹配。
tm_ort_artifacts_ok() {
    [ -s "$OUT_LIB" ] || return 1
    [ -s "$OUT_CXX_LIB" ] || return 1
    [ -s "$PROVENANCE" ] || return 1
    grep -qx "source_sha256=${ORT_ORIG_SHA256}" "$PROVENANCE" 2>/dev/null || return 1
    grep -qx "flavor=${FLAVOR}" "$PROVENANCE" 2>/dev/null || return 1
    if command -v nm >/dev/null 2>&1; then
        nm -g --defined-only "$OUT_LIB" 2>/dev/null | grep -qw 'OrtGetApiBase' || return 1
    fi
    return 0
}

tm_ort_clean() {
    tm_log_info "removing ${CACHE_ROOT} (build cache + artifacts)"
    rm -rf "$CACHE_ROOT"
    tm_log_info "removed; source tarballs in .cache/sources are kept"
}

# ── 容器内：真正的构建 ────────────────────────────────────────────

# 找出需要合并的静态库：ORT 的各个子目标 + 第三方依赖，排除测试/基准设施。
tm_ort_collect_archives() {
    find "$BUILD_DIR" -name '*.a' -type f \
        -not -path '*/CMakeFiles/*' \
        -not -name 'libgtest*.a' \
        -not -name 'libgmock*.a' \
        -not -name 'libbenchmark*.a' \
        -not -name 'libonnxruntime_test*.a' \
        -not -name 'libonnxruntime_perf_test*.a' \
        -not -name 'libprotobuf.a' \
        -not -name 'libprotoc.a' \
        | sort
}

# 合并：ar -M 把多个归档拼成一个（保留全部成员，链接器按需拉取）。
tm_ort_merge_archives() {
    local out="${1:-}"
    shift
    local script_file="${BUILD_DIR}/merge-archive.mri"
    {
        echo "CREATE ${out}"
        local archive
        for archive in "$@"; do
            echo "ADDLIB ${archive}"
        done
        echo "SAVE"
        echo "END"
    } > "$script_file"
    ar -M < "$script_file"
}

# 自检：用合并后的单一归档链接一个最小 C 程序（走 C API 静态入口），
# 模拟 ter-music 的真实链接方式：cc + libonnxruntime.a + libstdc++.a。
tm_ort_probe_link() {
    local lib="${1:-}"
    local workdir="${2:-}"
    local probe_c="${workdir}/probe_ort.c"
    local probe_bin="${workdir}/probe_ort"
    cat > "$probe_c" <<'PROBE'
#include <stdio.h>
#include "onnxruntime_c_api.h"

int main(void)
{
    const OrtApiBase *base = OrtGetApiBase();
    if (!base || !base->GetApi || !base->GetVersionString)
        return 1;
    const OrtApi *api = base->GetApi(ORT_API_VERSION);
    if (!api)
        return 2;
    printf("onnxruntime %s (api %d)\n", base->GetVersionString(), ORT_API_VERSION);
    return 0;
}
PROBE
    local include_dir="${ORT_SRC}/include/onnxruntime/core/session"
    if ! cc -O0 -g0 -o "$probe_bin" "$probe_c" \
            -I"$include_dir" \
            "$lib" "${OUT_CXX_LIB}" -lm -lpthread -ldl; then
        tm_log_error "probe link against merged archive failed"
        return 1
    fi
    local out
    if ! out="$("$probe_bin")"; then
        tm_log_error "probe binary failed to run: ${out}"
        return 1
    fi
    tm_log_info "probe link/run OK: ${out}"
    printf '%s\n' "$out" > "${workdir}/probe-version.txt"
}

# 二进制里是否有 libonnxruntime 的 DT_NEEDED（静态化验收的硬条件）。
tm_ort_probe_needed() {
    local bin="${1:-}"
    if readelf -d "$bin" 2>/dev/null | grep -qi 'libonnxruntime'; then
        tm_log_error "probe binary still has DT_NEEDED libonnxruntime"
        return 1
    fi
    return 0
}

tm_ort_write_provenance() {
    local cxx_version glibc_version gcc_version cc_path lib_size cxx_size members
    glibc_version="$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')"
    gcc_version="$(gcc -dumpfullversion -dumpversion 2>/dev/null || gcc --version | head -1)"
    cc_path="$(command -v gcc)"
    lib_size="$(stat -c %s "$OUT_LIB")"
    cxx_size="$(stat -c %s "$OUT_CXX_LIB")"
    members="$(ar t "$OUT_LIB" | wc -l | tr -d ' ')"
    cxx_version="$(strings "$OUT_CXX_LIB" 2>/dev/null | grep -m1 -E '^GCC: \(' || true)"

    {
        echo "onnxruntime_static=1"
        echo "flavor=${FLAVOR}"
        echo "source=${ORT_ORIG_NAME}"
        echo "source_url=${ORT_POOL_URL}/${ORT_ORIG_NAME}"
        echo "source_sha256=${ORT_ORIG_SHA256}"
        echo "source_size=${ORT_ORIG_SIZE}"
        echo "debian_patch=${ORT_DEBIAN_NAME}"
        echo "debian_patch_sha256=${ORT_DEBIAN_SHA256}"
        echo "upstream_version=${ORT_VERSION}"
        echo "build_image=${IMAGE_NAME}"
        echo "build_host_kernel=$(uname -r)"
        echo "toolchain_gcc=${gcc_version} (${cc_path})"
        echo "toolchain_glibc=${glibc_version}"
        echo "toolchain_cxx_static_lib=${cxx_version:-unknown}"
        echo "built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "cmake_version=$(cmake --version | head -1 | awk '{print $3}')"
        echo "archive_size=${lib_size}"
        echo "archive_members=${members}"
        echo "cxx_archive_size=${cxx_size}"
        echo "install_dir=${ORT_STATIC_INSTALL_DIR}"
    } > "$PROVENANCE"
    tm_log_info "provenance: ${PROVENANCE}"
}

tm_ort_container_build() {
    if [ "$FLAVOR" = "centos7" ]; then
        # devtoolset-11：gcc 11 + libstdc++.a + ld 2.36，产物 glibc 上限 = 2.17。
        # shellcheck disable=SC1091
        . /opt/rh/devtoolset-11/enable
    fi
    tm_log_info "flavor=${FLAVOR} cc=$(gcc -dumpfullversion -dumpversion) glibc=$(ldd --version | head -1 | awk '{print $NF}')"

    mkdir -p "$CACHE_ROOT"
    rm -f "$OUT_LIB" "$PROVENANCE"
    mkdir -p "$STAMP_DIR"

    if [ "$SKIP_BUILD" = false ]; then
        if [ "$FORCE" = true ] && [ -d "$BUILD_DIR" ]; then
            tm_log_info "--force: removing previous build dir ${BUILD_DIR}"
            rm -rf "$BUILD_DIR"
        fi
        tm_ort_prepare_dep_sources || return 1
        local -a dep_args=()
        local entry fetch_name
        for entry in "${ORT_DEP_MAP[@]}"; do
            fetch_name="${entry%%:*}"
            dep_args+=("-DFETCHCONTENT_SOURCE_DIR_$(printf '%s' "$fetch_name" | tr '[:lower:]' '[:upper:]')=${BUILD_DIR}/dep-src/${fetch_name}")
        done
        dep_args+=("-DFETCHCONTENT_SOURCE_DIR_EIGEN=${BUILD_DIR}/dep-src/eigen")
        # safeint 走 find_path，直接给缓存变量；其余依赖全部用预取源码。
        dep_args+=("-Dsafeint_SOURCE_DIR=${BUILD_DIR}/dep-src/safeint")
        # 断网兜底：任何没预取的依赖立刻报错，而不是卡在下载上。
        dep_args+=("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
        # ONNX 的 gen_proto.py 是 python3 语法；EL7 的 /usr/bin/python 是 2.7，
        # 必须显式指定解释器，否则 onnx_proto 代码生成会 SyntaxError。
        if command -v python3 >/dev/null 2>&1; then
            dep_args+=("-DPYTHON_EXECUTABLE=$(command -v python3)")
        else
            tm_log_error "python3 is required to generate onnx protobuf sources"
            return 1
        fi
        tm_log_info "cmake configure -> ${BUILD_DIR}"
        # 关键选项（每个都有理由）：
        #   BUILD_SHARED_LIB=OFF     要静态库；ORT 会把各个子模块编译成独立 .a
        #   BUILD_UNIT_TESTS=OFF     不需要 gtest（否则额外拉 googletest）
        #   ENABLE_PYTHON=OFF        只要 C/C++ 库，避免 python 版本/依赖问题
        #   USE_FULL_PROTOBUF=OFF    用 protobuf-lite，体积小、编译快
        #   ENABLE_CPUINFO=ON        CPU 特性探测（默认行为，显式固定）
        #   FETCHCONTENT_SOURCE_DIR_*  依赖全部用宿主预取源码（github 归档不可达）
        #   CMAKE_POSITION_INDEPENDENT_CODE=ON  PIE 主程序链接静态库的前提
        if ! cmake -S "${ORT_SRC}/cmake" -B "$BUILD_DIR" \
                "${dep_args[@]}" \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
                -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
                -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
                -Donnxruntime_BUILD_SHARED_LIB=OFF \
                -Donnxruntime_BUILD_UNIT_TESTS=OFF \
                -Donnxruntime_BUILD_BENCHMARKS=OFF \
                -Donnxruntime_BUILD_JAVA=OFF \
                -Donnxruntime_BUILD_NODEJS=OFF \
                -Donnxruntime_ENABLE_PYTHON=OFF \
                -Donnxruntime_ENABLE_ATEN=OFF \
                -Donnxruntime_ENABLE_TRAINING=OFF \
                -Donnxruntime_ENABLE_TRAINING_OPS=OFF \
                -Donnxruntime_USE_CUDA=OFF \
                -Donnxruntime_USE_DNNL=OFF \
                -Donnxruntime_USE_XNNPACK=OFF \
                -Donnxruntime_USE_TENSORRT=OFF \
                -Donnxruntime_USE_OPENVINO=OFF \
                -Donnxruntime_USE_MIGRAPHX=OFF \
                -Donnxruntime_USE_NNAPI_BUILTIN=OFF \
                -Donnxruntime_USE_RKNPU=OFF \
                -Donnxruntime_USE_ACL=OFF \
                -Donnxruntime_USE_ARMNN=OFF \
                -Donnxruntime_USE_QNN=OFF \
                -Donnxruntime_USE_SNPE=OFF \
                -Donnxruntime_ENABLE_CPUINFO=ON \
                -Donnxruntime_USE_FULL_PROTOBUF=OFF \
                -DFETCHCONTENT_SOURCE_DIR_EIGEN="${EIGEN_SRC}" \
                2>&1 | tee "$CONFIG_LOG"; then
            tm_log_error "cmake configure failed; see ${CONFIG_LOG}"
            return 1
        fi

        tm_log_info "building (this takes tens of minutes; log: ${BUILD_LOG})"
        if ! cmake --build "$BUILD_DIR" --config Release --parallel 4 2>&1 | tee "$BUILD_LOG"; then
            tm_log_error "build failed; see ${BUILD_LOG}"
            return 1
        fi
        # re2 是 FetchContent 的 EXCLUDE_FROM_ALL 依赖：ORT 只在自己的
        # **共享库**链接列表里显式带上它，静态构建的 all 目标不会编它，
        # 但 contrib Tokenizer 的实现文件确实调 re2 符号 —— 合并归档后必须
        # 有 libre2.a，否则最终链接报 undefined reference to
        # re2::RE2::Match。这里显式补编 re2 目标。
        tm_log_info "building excluded-from-all dependency: re2"
        if ! cmake --build "$BUILD_DIR" --target re2 --config Release --parallel 4 2>&1 | tee -a "$BUILD_LOG"; then
            tm_log_error "building re2 failed; see ${BUILD_LOG}"
            return 1
        fi
    else
        tm_log_info "--skip-build: reusing ${BUILD_DIR} (no compile)"
    fi

    # 合并归档 + C++ 运行时归档
    local -a archives=()
    mapfile -t archives < <(tm_ort_collect_archives)
    if [ "${#archives[@]}" -eq 0 ]; then
        tm_log_error "no static archives found under ${BUILD_DIR}"
        return 1
    fi
    tm_log_info "merging ${#archives[@]} static archives into libonnxruntime.a"
    mkdir -p "$CACHE_ROOT"
    local merged_tmp="${OUT_LIB}.part.$$"
    tm_ort_merge_archives "$merged_tmp" "${archives[@]}"
    mv -f "$merged_tmp" "$OUT_LIB"

    {
        echo "# ONNX Runtime 静态库合并清单（顺序即 ar ADDLIB 顺序）"
        echo "# 合并时间: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "# 工具链: gcc $(gcc -dumpfullversion -dumpversion), glibc $(ldd --version | head -1 | awk '{print $NF}'), flavor=${FLAVOR}"
        echo "# 说明: ORT 静态构建会产出多个子目标归档（common/mlas/graph/framework/"
        echo "#       util/providers/optimizer/session/flatbuffers/lora）与第三方归档"
        echo "#       （onnx/protobuf-lite/flatbuffers/date/cpuinfo/pthreadpool/...）；"
        echo "#       这里合并成单个 libonnxruntime.a，打包侧只需一个文件 + 一个链接项。"
        local archive
        for archive in "${archives[@]}"; do
            printf '%s\t%s\t%s\n' \
                "$(stat -c %s "$archive")" \
                "$(ar t "$archive" | wc -l | tr -d ' ')" \
                "${archive#"$BUILD_DIR"/}"
        done
    } > "$DEPS_MANIFEST"
    tm_log_info "deps manifest: ${DEPS_MANIFEST}"

    # C++ 运行时：链接期需要 GCC 11 的 libstdc++.a（见脚本头注释）。
    local cxx_archive
    cxx_archive="$(g++ -print-file-name=libstdc++.a)"
    if [ ! -f "$cxx_archive" ]; then
        tm_log_error "libstdc++.a not found via g++ -print-file-name"
        return 1
    fi
    mkdir -p "$OUT_CXX_DIR"
    cp -f "$cxx_archive" "$OUT_CXX_LIB"
    tm_log_info "c++ runtime archive: ${cxx_archive} -> ${OUT_CXX_LIB}"

    if ! tm_ort_probe_link "$OUT_LIB" "$CACHE_ROOT"; then
        return 1
    fi
    tm_ort_probe_needed "${CACHE_ROOT}/probe_ort" || return 1

    tm_ort_write_provenance

    # 静态库里的未定义符号清单，供 CMake 侧核对系统库依赖（-lm/-ldl/-lpthread...）
    if command -v nm >/dev/null 2>&1; then
        nm -u "$OUT_LIB" 2>/dev/null | awk '{print $NF}' | sed 's/@.*//' | sort -u \
            > "${CACHE_ROOT}/undefined-symbols.txt" || true
    fi

    tm_log_info "static library ready: ${OUT_LIB} ($(stat -c %s "$OUT_LIB") bytes)"
    tm_log_info "$(cat "${CACHE_ROOT}/probe-version.txt" 2>/dev/null || true)"
}

# ── 宿主：编排 ────────────────────────────────────────────────────

tm_ort_run_container() {
    local docker_cmd=(env "DOCKER_CONFIG=${TM_REPO_ROOT}/.cache/docker-buildx" docker)
    local -a run_opts=(
        --rm
        -v "${TM_REPO_ROOT}:/workspace"
        --workdir /workspace
        --user "$(id -u):$(id -g)"
        -e "HOME=/tmp"
        -e "TER_MUSIC_IN_CONTAINER=1"
        -e "TER_MUSIC_ORT_FLAVOR=${FLAVOR}"
        -e "TER_MUSIC_ORT_FORCE=$([ "$FORCE" = true ] && echo 1 || echo 0)"
        -e "TER_MUSIC_ORT_SKIP_BUILD=$([ "$SKIP_BUILD" = true ] && echo 1 || echo 0)"
    )
    local -a cmd=(./scripts/build/build-onnxruntime-static.sh --in-container --flavor "$FLAVOR")
    tm_log_info "running build in ${IMAGE_NAME}"
    "${docker_cmd[@]}" run "${run_opts[@]}" "$IMAGE_NAME" "${cmd[@]}"
}

tm_ort_host_main() {
    if [ "$CLEAN" = true ]; then
        tm_ort_clean
        exit 0
    fi
    if ! command -v docker >/dev/null 2>&1; then
        tm_log_error "docker is required to build the static ONNX Runtime library"
        exit 1
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        tm_log_error "python3 is required (unpack the CMake wheel)"
        exit 1
    fi

    tm_ort_ensure_sources || exit 1
    tm_ort_ensure_eigen || exit 1
    tm_ort_ensure_cmake || exit 1
    tm_ort_ensure_extracted || exit 1
    tm_ort_prefetch_deps || exit 1

    if [ "$FORCE" = false ] && tm_ort_artifacts_ok; then
        tm_log_info "static ONNX Runtime library already built and valid: ${OUT_LIB}"
        tm_log_info "use --force to rebuild"
        exit 0
    fi

    tm_ort_ensure_image || exit 1
    tm_ort_run_container || exit 1

    if ! tm_ort_artifacts_ok; then
        tm_log_error "build finished but artifacts look invalid: ${OUT_LIB}"
        exit 1
    fi
    tm_log_info "done: $(stat -c %s "$OUT_LIB") bytes, sha256 $(sha256sum "$OUT_LIB" | awk '{print $1}')"
}

tm_ort_container_main() {
    # 容器内模式：强制/跳过开关由宿主通过环境变量传入（--force 等参数也接受）。
    [ "${TER_MUSIC_ORT_FORCE:-0}" = "1" ] && FORCE=true
    [ "${TER_MUSIC_ORT_SKIP_BUILD:-0}" = "1" ] && SKIP_BUILD=true
    tm_ort_container_build
}

if [ "$IN_CONTAINER" = true ]; then
    tm_ort_container_main
else
    tm_ort_host_main
fi
