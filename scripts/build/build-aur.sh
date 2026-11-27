#!/usr/bin/env bash
#
# AUR 构建 + 安装验收（Arch 容器）。不投递、不打 tag。
#
# 验收对象是真实 PKGBUILD 源：makepkg 从 source=git+https://…#commit=<_commit>
# 全新克隆固定提交（不复用本地工作树），模型按 sha256sums 校验；随后把包
# 安装进容器做布局/冒烟测试，并安装 Arch 官方 onnxruntime-cpu 验证运行期
# dlopen 名（libonnxruntime.so.1）与最小模型加载。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=common.sh
. "${SCRIPT_DIR}/scripts/build/common.sh"

PACKAGE_NAME="cn.org.huansoft.ter-music"
REPO_ROOT="$SCRIPT_DIR"
NET_HINT_DIR="${REPO_ROOT}/.tmp/aur-net"
NET_HINT_FILE="${NET_HINT_DIR}/git-curlopt-resolve"

show_help() {
    cat <<EOF
用法: $0 -v VERSION [选项]

在 Arch 容器里用真实 PKGBUILD 源（git+https 固定 commit）makepkg 并安装验收
AUR 包（不投递、不打 tag）。

选项:
  -v, --version VERSION   必填；三段自动补 .0 为四段（--write-srcinfo 时可省略）
  -a, --arch ARCH         仅支持 x86_64（默认 x86_64）
  -k, --keep-temp         保留 .tmp/aur
      --no-install-test   只构建，不做 pacman -U 安装验收
      --write-srcinfo     用 makepkg --printsrcinfo 重新生成仓库 .SRCINFO 后退出
      --rebuild-image     强制重建 Arch 镜像
      --no-cache          docker build 不使用缓存
  -h, --help              显示帮助

网络: GitHub HTTPS 在部分网络会被 DNS 解析到不可达地址。容器内实测不可达时
      会探测一组已知 GitHub IP 并临时写 /etc/hosts（仅本次构建容器）；也可用
      TM_AUR_GITHUB_IP=<ip> 指定候选。PKGBUILD 的 source 保持不变。

输出: build/aur/x86_64/cn.org.huansoft.ter-music-<V>-1-x86_64.pkg.tar.zst
EOF
}

VERSION=""
ARCH="x86_64"
KEEP_TEMP=false
INSTALL_TEST=true
REBUILD_IMAGE=false
NO_CACHE=false
WRITE_SRCINFO=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        -v|--version) VERSION="${2:-}"; shift 2 ;;
        -a|--arch) ARCH="${2:-}"; shift 2 ;;
        -k|--keep-temp) KEEP_TEMP=true; shift ;;
        --no-install-test) INSTALL_TEST=false; shift ;;
        --write-srcinfo) WRITE_SRCINFO=true; shift ;;
        --rebuild-image) REBUILD_IMAGE=true; shift ;;
        --no-cache) NO_CACHE=true; shift ;;
        *)
            tm_log_error "unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

if [ "$WRITE_SRCINFO" = true ] && [ -z "$VERSION" ]; then
    : # 只重生成 .SRCINFO 时不需要版本号
else
    VERSION="$(tm_normalize_version_or_die "$VERSION")" || exit 1
fi
if [ "$ARCH" != "x86_64" ]; then
    tm_log_error "unsupported architecture: $ARCH (only x86_64)"
    exit 1
fi

# 受限网络可显式给一个候选 IP（TM_AUR_GITHUB_IP）。实际探测在容器内做：
# 容器与宿主的 DNS 解析结果可能不同（宿主可达不代表容器可达）。
tm_aur_export_network_hint() {
    if [ -n "${TM_AUR_GITHUB_IP:-}" ]; then
        mkdir -p "$NET_HINT_DIR"
        printf '%s\n' "$TM_AUR_GITHUB_IP" > "$NET_HINT_FILE"
        tm_log_info "TM_AUR_GITHUB_IP=${TM_AUR_GITHUB_IP} passed to container as GitHub IP candidate"
    fi
}

# 宿主机入口：委托 Docker；容器内继续执行实际构建。
if ! tm_is_container; then
    rm -f "$NET_HINT_FILE"
    tm_aur_export_network_hint
    xb_args=(
        -f scripts/docker/Dockerfile.aur
        -n ter-music-aur
        -s build-aur.sh
        --root
    )
    [ "$REBUILD_IMAGE" = true ] && xb_args+=(--rebuild)
    [ "$NO_CACHE" = true ] && xb_args+=(--no-cache)
    inner=(-a "$ARCH")
    [ -n "$VERSION" ] && inner+=(-v "$VERSION")
    [ "$KEEP_TEMP" = true ] && inner+=(-k)
    [ "$INSTALL_TEST" = false ] && inner+=(--no-install-test)
    [ "$WRITE_SRCINFO" = true ] && inner+=(--write-srcinfo)
    exec "${REPO_ROOT}/scripts/docker/docker-build.sh" "${xb_args[@]}" -- "${inner[@]}"
fi

read_network_hint_ip() {
    if [ -s "$NET_HINT_FILE" ]; then
        head -n1 "$NET_HINT_FILE" | tr -d '[:space:]'
    fi
}

# 容器内网络修正：DNS 可能把 github.com 解析到不可达 IP，makepkg 的 git+https
# 源会卡 60s+ 后失败。先实测；不可达时逐个探测候选 IP（git 协议实测），
# 命中的写进 /etc/hosts —— 只影响本次构建容器，PKGBUILD 的 source 不变。
tm_aur_fix_github_dns() {
    local repo_url hint_ip candidate
    repo_url="$(sed -n 's/^source=("ter-music::git+\(https[^"#]*\).*/\1/p' \
        "${REPO_ROOT}/PKGBUILD" | head -n1)"
    if [ -z "$repo_url" ]; then
        tm_log_warn "cannot parse git source URL from PKGBUILD; skip network probe"
        return 0
    fi
    if timeout 20 git ls-remote "$repo_url" >/dev/null 2>&1; then
        tm_log_info "github HTTPS reachable; makepkg clones ${repo_url} directly"
        return 0
    fi
    hint_ip="$(read_network_hint_ip)"
    if [ ! -w /etc/hosts ]; then
        tm_log_warn "/etc/hosts is not writable; cannot override unreachable github.com DNS"
        return 0
    fi
    tm_log_warn "github HTTPS unreachable via system DNS; probing known GitHub IPs"
    for candidate in "$hint_ip" 20.205.243.166 140.82.114.3 \
              140.82.113.3 140.82.112.3 140.82.121.3; do
        [ -n "$candidate" ] || continue
        if ! timeout 40 env GIT_CONFIG_COUNT=1 \
                GIT_CONFIG_KEY_0=http.curloptResolve \
                GIT_CONFIG_VALUE_0="github.com:443:${candidate}" \
                git ls-remote "$repo_url" >/dev/null 2>&1; then
            continue
        fi
        printf '%s github.com\n' "$candidate" >> /etc/hosts
        tm_log_warn "applied /etc/hosts override: github.com -> ${candidate} (this build container only)"
        return 0
    done
    tm_log_warn "no reachable GitHub IP found; makepkg source download may fail"
    return 0
}

# 重新生成 .SRCINFO：只从 PKGBUILD 推导，绝不手改。
if [ "$WRITE_SRCINFO" = true ]; then
    tm_log_info "regenerating .SRCINFO from PKGBUILD (makepkg --printsrcinfo)"
    srcinfo_tmp="$(mktemp -d)"
    cp -f "${REPO_ROOT}/PKGBUILD" "${srcinfo_tmp}/PKGBUILD"
    chown -R builder:builder "$srcinfo_tmp"
    su -s /bin/bash builder -c \
        "cd '${srcinfo_tmp}' && HOME=/home/builder makepkg --printsrcinfo > '${srcinfo_tmp}/SRCINFO'"
    install -m 0644 "${srcinfo_tmp}/SRCINFO" "${REPO_ROOT}/.SRCINFO"
    chown "${HOST_UID:-0}:${HOST_GID:-0}" "${REPO_ROOT}/.SRCINFO" 2>/dev/null || true
    rm -rf "$srcinfo_tmp"
    tm_fix_ownership
    tm_log_info ".SRCINFO regenerated: ${REPO_ROOT}/.SRCINFO"
    exit 0
fi

WORK="${REPO_ROOT}/.tmp/aur"
SRCDEST="${WORK}/srcdest"
OUT_DIR="${REPO_ROOT}/build/aur/x86_64"
PKG_DIR="${WORK}/pkg"
BUILDDIR="${WORK}/build"

rm -rf "$WORK"
mkdir -p "$SRCDEST" "$OUT_DIR" "$PKG_DIR" "$BUILDDIR"

# 容器以 root 运行；即使 makepkg/pacman 失败，也要把产物目录交还宿主用户。
restore_owner() {
    if [ -n "${HOST_UID:-}" ] && [ -n "${HOST_GID:-}" ]; then
        chown -R "${HOST_UID}:${HOST_GID}" \
            "${REPO_ROOT}/build" "$WORK" 2>/dev/null || true
    fi
}
trap restore_owner EXIT

tm_log_info "AUR build: version=${VERSION} arch=${ARCH} install_test=${INSTALL_TEST} (real git source)"

# 使用仓库里的 PKGBUILD 原文：source=git+https://…#commit=<_commit>。
# 不把本地工作树塞进 $srcdir —— 验收的就是 AUR 真正会拉到的代码。
cp -f "${REPO_ROOT}/PKGBUILD" "${WORK}/PKGBUILD"
pkgbuild_pkgver="$(sed -n 's/^pkgver=//p' "${WORK}/PKGBUILD" | head -n1)"
if [ "$pkgbuild_pkgver" != "$VERSION" ]; then
    tm_log_warn "PKGBUILD pkgver=${pkgbuild_pkgver} != requested ${VERSION}; overriding pkgver for this build (source commit unchanged)"
    sed -i "s/^pkgver=.*/pkgver=${VERSION}/" "${WORK}/PKGBUILD"
fi
commit="$(sed -n 's/^_commit=\([0-9a-f]\{40\}\)$/\1/p' "${WORK}/PKGBUILD" | head -n1)"
if [ -z "$commit" ]; then
    tm_log_error "PKGBUILD has no 40-hex _commit"
    exit 1
fi
tm_log_info "pinned commit: ${commit}"

# 先确认容器到 GitHub 的 HTTPS 可用（必要时临时修正 DNS），再交给 makepkg。
tm_aur_fix_github_dns


# 内置消音模型：tm_ensure_model 校验 .tmp/models 缓存（缺失才下载），再把
# 校验过的文件预置进 SRCDEST。makepkg 仍会按 sha256sums 再校验一次。
tm_ensure_model || exit 1
cp -f -- "$TM_MODEL_PATH" "${SRCDEST}/${TM_MODEL_NAME}"
tm_log_info "model staged into SRCDEST for makepkg checksum verification: ${SRCDEST}/${TM_MODEL_NAME}"

chown -R builder:builder "$WORK"

tm_log_info "makepkg (as builder, real source; -C clean build)"
set +e
su -s /bin/bash builder -c \
    "cd '${WORK}' && HOME=/home/builder PKGDEST='${PKG_DIR}' SRCDEST='${SRCDEST}' BUILDDIR='${BUILDDIR}' makepkg -C -f --nodeps --noconfirm"
makepkg_rc=$?
set -e
if [ "$makepkg_rc" -ne 0 ]; then
    tm_log_error "makepkg failed (rc=${makepkg_rc})"
    exit 1
fi

# 源码树取证：$srcdir/ter-music 必须是 makepkg 从远端克隆的 git 工作树，
# HEAD 必须等于 PKGBUILD 的 _commit，且包含 config v12 / ONNX 代码。
src_tree="${BUILDDIR}/${PACKAGE_NAME}/src/ter-music"
if [ ! -d "$src_tree" ]; then
    tm_log_error "source tree missing after makepkg: ${src_tree}"
    exit 1
fi
if [ ! -d "${src_tree}/.git" ] && [ ! -f "${src_tree}/.git" ]; then
    tm_log_error "source tree is not a git checkout: ${src_tree}"
    exit 1
fi
head_commit="$(su -s /bin/bash builder -c \
    "cd '${src_tree}' && HOME=/home/builder git rev-parse HEAD")"
if [ "$head_commit" != "$commit" ]; then
    tm_log_error "source HEAD ${head_commit} != PKGBUILD _commit ${commit}"
    exit 1
fi
tm_log_info "source tree HEAD=${head_commit} (matches PKGBUILD _commit)"

# SRCDEST 里的镜像仓库 origin 必须是 GitHub URL：证明代码来自网络克隆，
# 而不是本地工作树拷贝。
mirror_origin="$(su -s /bin/bash builder -c \
    "HOME=/home/builder git --git-dir='${SRCDEST}/ter-music' config --get remote.origin.url" 2>/dev/null || true)"
case "$mirror_origin" in
    *github.com*ter-music.git*) ;;
    *)
        tm_log_error "source mirror origin is not the GitHub repo: ${mirror_origin:-<empty>}"
        exit 1
        ;;
esac
tm_log_info "source mirror origin: ${mirror_origin}"
if ! su -s /bin/bash builder -c \
        "HOME=/home/builder git --git-dir='${SRCDEST}/ter-music' cat-file -e '${commit}^{commit}'"; then
    tm_log_error "commit ${commit} missing from cloned mirror"
    exit 1
fi

# PKGBUILD 的 prepare() 会把 ncursesw/ 前缀改掉；除这些 .c/.h 外，
# 工作树必须与固定提交一致（不允许带入本地改动）。
dirty_paths="$(su -s /bin/bash builder -c \
    "cd '${src_tree}' && HOME=/home/builder git diff --name-only")"
unexpected_dirty="$(printf '%s\n' "$dirty_paths" | grep -vE '\.(c|h)$' | grep -v '^$' || true)"
if [ -n "$unexpected_dirty" ]; then
    tm_log_error "source tree has unexpected local modifications:"
    printf '%s\n' "$unexpected_dirty" >&2
    exit 1
fi

check_v12_tree() {
    local tree="$1"
    local schema_h="${tree}/include/cn.org.huansoft.ter-music/config/schema.h"
    local types_h="${tree}/include/cn.org.huansoft.ter-music/types.h"
    local model_c="${tree}/src/cn.org.huansoft.ter-music/audio/karaoke_model.c"
    local required
    for required in \
        "$schema_h" \
        "$types_h" \
        "$model_c" \
        "${tree}/src/cn.org.huansoft.ter-music/audio/karaoke_cache.c" \
        "${tree}/include/third_party/onnxruntime/onnxruntime_c_api.h" \
        "${tree}/resources/models/NOTICE.md"; do
        if [ ! -f "$required" ]; then
            tm_log_error "v12 source tree is missing: ${required#"${tree}"/}"
            exit 1
        fi
    done
    if ! grep -qE '^#define CONFIG_CURRENT_VERSION[[:space:]]+12([[:space:]]|$)' "$schema_h"; then
        tm_log_error "config v12 marker missing in ${schema_h#"${tree}"/}"
        exit 1
    fi
    if ! grep -qE '#define CONFIG_CURRENT_VERSION[[:space:]]+12([[:space:]]|$)' "$types_h"; then
        tm_log_error "config v12 marker missing in ${types_h#"${tree}"/}"
        exit 1
    fi
    if ! grep -q 'libonnxruntime.so.1.21' "$model_c"; then
        tm_log_error "ONNX runtime dlopen list missing in ${model_c#"${tree}"/}"
        exit 1
    fi
    tm_log_info "v12/ONNX source markers present (CONFIG_CURRENT_VERSION=12, onnxruntime_c_api.h, karaoke_model.c dlopen)"
}
check_v12_tree "$src_tree"

pkg_file="$(find "$PKG_DIR" -maxdepth 1 -name '*.pkg.tar.zst' -type f -print -quit)"
if [ -z "$pkg_file" ]; then
    tm_log_error "makepkg produced no .pkg.tar.zst"
    exit 1
fi
tm_log_info "package: $(basename "$pkg_file") ($(wc -c < "$pkg_file" | tr -d ' \n') bytes)"

# 包内布局（Linyaps 目录规范）：实体在 /opt/apps/<appid>/files，
# /usr 下只有符号链接；模型与 NOTICE 必须随包分发。
pkg_listing="$(pacman -Qlp "$pkg_file" | awk '{print $2}' | sed 's|^/||')"
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
        tm_log_error "AUR package path is not a symlink: ${link#"${pkg_extract}"}"
        exit 1
    fi
done
if [ -e "${pkg_extract}/usr/bin/ter-music" ] && [ ! -L "${pkg_extract}/usr/bin/ter-music" ]; then
    tm_log_error "AUR package still ships a real /usr/bin/ter-music file"
    exit 1
fi

# 运行期 ONNX Runtime 验收：官方 onnxruntime-cpu 提供 libonnxruntime.so.1，
# 与 karaoke_model.c 的 dlopen 列表（.so.1.21 → .so.1 → .so）匹配。
# 这是可选增强（optdepends）：先证明未装运行库时包能正常安装运行，
# 再安装运行库，用包内置模型建一个真实 InferenceSession。
verify_onnxruntime_dlopen() {
    local tree="$1"
    local model="$2"
    local header="${tree}/include/third_party/onnxruntime/onnxruntime_c_api.h"

    tm_log_info "onnxruntime optdepends check: install Arch onnxruntime-cpu"
    pacman -Sy --noconfirm >/dev/null
    pacman -S --noconfirm --needed onnxruntime-cpu
    local ort_files
    ort_files="$(pacman -Ql onnxruntime-cpu | awk '{print $2}' | grep 'libonnxruntime\.so\.1$' || true)"
    if [ -z "$ort_files" ]; then
        tm_log_error "onnxruntime-cpu does not ship libonnxruntime.so.1"
        exit 1
    fi
    tm_log_info "onnxruntime-cpu ships: ${ort_files}"
    ldconfig -p | grep -m3 'libonnxruntime' || true

    cat > /tmp/tm-aur-ort-check.c <<'CEOF'
/* ter-music AUR 验收：最小 ONNX Runtime 加载测试。
 * 不链接运行库，按 karaoke_model.c 的顺序 dlopen，再用打包的模型
 * 建一个真实 InferenceSession（只加载，不推理）。 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "onnxruntime_c_api.h"

typedef const OrtApiBase *(ORT_API_CALL *tm_ort_get_api_base_fn)(void);

static void tm_print_status(const OrtApi *api, const char *what,
                            OrtStatus *status) {
    if (!status) {
        return;
    }
    const char *message = api ? api->GetErrorMessage(status) : NULL;
    fprintf(stderr, "%s failed: %s\n", what, message ? message : "(null)");
}

int main(int argc, char **argv) {
    static const char *const names[] = {
        "libonnxruntime.so.1.21",
        "libonnxruntime.so.1",
        "libonnxruntime.so",
    };
    void *handle = NULL;
    const char *dlopen_name = NULL;
    char last_error[512] = "";
    const OrtApiBase *api_base = NULL;
    const OrtApi *api = NULL;
    OrtStatus *status = NULL;
    OrtEnv *env = NULL;
    OrtSessionOptions *options = NULL;
    OrtSession *session = NULL;
    size_t input_count = 0;
    size_t output_count = 0;
    tm_ort_get_api_base_fn get_api_base = NULL;
    int api_version = 0;
    int version;
    int rc = 0;
    size_t i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <model.onnx>\n", argv[0]);
        return 64;
    }

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        dlerror();
        handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            dlopen_name = names[i];
            break;
        }
        {
            const char *error = dlerror();
            if (error) {
                snprintf(last_error, sizeof(last_error), "%s", error);
            }
        }
    }
    if (!handle) {
        fprintf(stderr,
                "dlopen failed for libonnxruntime.so.1.21/.so.1/.so: %s\n",
                last_error);
        return 2;
    }
    printf("dlopen_name=%s\n", dlopen_name);

    get_api_base = (tm_ort_get_api_base_fn)dlsym(handle, "OrtGetApiBase");
    if (!get_api_base) {
        fprintf(stderr, "dlsym OrtGetApiBase failed: %s\n", dlerror());
        rc = 3;
        goto done;
    }
    api_base = get_api_base();
    for (version = ORT_API_VERSION; version >= 16; version--) {
        api = api_base->GetApi(version);
        if (api) {
            api_version = version;
            break;
        }
    }
    if (!api) {
        fprintf(stderr, "no supported OrtApi between 16 and %d\n",
                ORT_API_VERSION);
        rc = 4;
        goto done;
    }
    printf("ort_api_version=%d (header ORT_API_VERSION=%d)\n", api_version,
           ORT_API_VERSION);

    status = api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "tm-aur-ort-check", &env);
    if (status) {
        tm_print_status(api, "CreateEnv", status);
        api->ReleaseStatus(status);
        rc = 5;
        goto done;
    }
    status = api->CreateSessionOptions(&options);
    if (status) {
        tm_print_status(api, "CreateSessionOptions", status);
        api->ReleaseStatus(status);
        rc = 6;
        goto done;
    }
    status = api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
    if (status) {
        tm_print_status(api, "SetSessionGraphOptimizationLevel", status);
        api->ReleaseStatus(status);
        rc = 7;
        goto done;
    }
    status = api->CreateSession(env, argv[1], options, &session);
    if (status) {
        tm_print_status(api, "CreateSession", status);
        api->ReleaseStatus(status);
        rc = 8;
        goto done;
    }
    status = api->SessionGetInputCount(session, &input_count);
    if (status) {
        tm_print_status(api, "SessionGetInputCount", status);
        api->ReleaseStatus(status);
        rc = 9;
        goto done;
    }
    status = api->SessionGetOutputCount(session, &output_count);
    if (status) {
        tm_print_status(api, "SessionGetOutputCount", status);
        api->ReleaseStatus(status);
        rc = 10;
        goto done;
    }
    printf("session_ok model=%s inputs=%zu outputs=%zu\n", argv[1], input_count,
           output_count);

done:
    if (session) {
        api->ReleaseSession(session);
    }
    if (options) {
        api->ReleaseSessionOptions(options);
    }
    if (env) {
        api->ReleaseEnv(env);
    }
    dlclose(handle);
    return rc;
}
CEOF

    if ! gcc -O1 -Wall -Wextra -I"$(dirname "$header")" \
            -o /tmp/tm-aur-ort-check /tmp/tm-aur-ort-check.c -ldl; then
        tm_log_error "failed to build minimal ONNX runtime load test"
        exit 1
    fi
    tm_log_info "minimal ONNX load test (dlopen + real InferenceSession)"
    /tmp/tm-aur-ort-check "$model"
}

if [ "$INSTALL_TEST" = true ]; then
    tm_log_info "install and smoke test (as root)"
    pacman -U --noconfirm "$pkg_file"
    installed_root="/opt/apps/${PACKAGE_NAME}/files"
    installed_model="${installed_root}/share/ter-music/models/uvr-mdxnet-3-9662.onnx"
    if [ ! -x "${installed_root}/bin/ter-music" ]; then
        tm_log_error "installed layout: ${installed_root}/bin/ter-music missing"
        exit 1
    fi
    if [ ! -f "$installed_model" ] || [ "$(wc -c < "$installed_model" | tr -d ' \n')" != "$TM_MODEL_SIZE" ]; then
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
    # optdepends 不是硬依赖：安装时不要求 onnxruntime 已存在。
    pkg_depends="$(pacman -Qi "$PACKAGE_NAME" | sed -n 's/^Depends On[[:space:]]*: //p')"
    if printf '%s\n' "$pkg_depends" | grep -qi 'onnxruntime'; then
        tm_log_error "onnxruntime must stay in optdepends, not depends: ${pkg_depends}"
        exit 1
    fi
    tm_log_info "hard dependencies do not include onnxruntime: ${pkg_depends}"
    if ! ter-music --help >/dev/null; then
        tm_log_error "installed package: ter-music --help failed"
        exit 1
    fi
    installed_version="$(ter-music version | awk '{print $2}')"
    if [ "$installed_version" != "v${VERSION}" ]; then
        tm_log_error "installed version '${installed_version}' != v${VERSION}"
        exit 1
    fi
    tm_log_info "installed version: ${installed_version}"

    verify_onnxruntime_dlopen "$src_tree" "$installed_model"

    # 卸载安全性：链接与 /opt 实体都由本包拥有，卸载后应一并消失。
    pacman -R --noconfirm "$PACKAGE_NAME"
    if [ -e /usr/bin/ter-music ] || [ -e "${installed_root}/bin/ter-music" ]; then
        tm_log_error "uninstall left files behind: /usr/bin/ter-music or ${installed_root}"
        exit 1
    fi
    tm_log_info "install/uninstall smoke test passed (symlinks + /opt entity + ONNX runtime)"
fi

# .SRCINFO 漂移检查：用原始 PKGBUILD 生成，和仓库里的文件比较。
mkdir -p "${WORK}/srcinfo"
cp -f "${REPO_ROOT}/PKGBUILD" "${WORK}/srcinfo/PKGBUILD"
chown -R builder:builder "${WORK}/srcinfo"
su -s /bin/bash builder -c \
    "cd '${WORK}/srcinfo' && HOME=/home/builder makepkg --printsrcinfo > '${WORK}/SRCINFO.generated'"
if ! diff -u "${REPO_ROOT}/.SRCINFO" "${WORK}/SRCINFO.generated"; then
    tm_log_error ".SRCINFO is out of date; regenerate it with: $0 -v ${VERSION} --write-srcinfo"
    exit 1
fi
tm_log_info ".SRCINFO matches PKGBUILD"

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
echo "源:  git+https://github.com/HuanSoft-Open-Source-Community/ter-music.git#commit=${commit}"
echo "产物: ${OUT_DIR}/$(basename "$pkg_file")"
ls -lh "${OUT_DIR}/"*.pkg.tar.zst 2>/dev/null || true
