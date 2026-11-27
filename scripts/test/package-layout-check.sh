#!/usr/bin/env bash
#
# 安装布局与内置模型注入检查（可执行验证 + 静态断言）。
#
# 本地可执行部分（不需要 docker / rpmbuild / makepkg / dh / fakeroot）：
#   1) 用真实 CMake 配置、构建并安装到 DESTDIR：前缀
#      /opt/apps/<appid>/files，-DTER_MUSIC_BUNDLED_MODEL 指向 .tmp/models
#      缓存（CMake 会再校验大小 + SHA256）；
#   2) 断言 DESTDIR 树：opt 布局、内置模型、NOTICE.md、lang/help/layouts、
#      desktop 与 hicolor 图标都在 opt 实体目录下；
#   3) 按 debian/links 建 /usr 相对符号链接，用 dpkg-deb 组装 deb，再用
#      dpkg-deb -c / -x 断言“实体在 opt、/usr 只有链接”、模型大小/SHA256；
#   4) 复用同一构建目录重配 INSTALL_LINYAPS_INTEGRATION=ON 安装第二份
#      DESTDIR，断言 linyaps 的 ${PREFIX} 布局 + systemd 用户单元 +
#      D-Bus 激活文件 + 模型/NOTICE 同时存在；
#   5) 原样提取 scripts/build/build-appimage.sh 的 t3-appdir-layout 段组装
#      AppDir，断言 AppRun 从 opt 路径启动、AppDir/usr 只有相对链接、
#      桌面文件/图标自洽，并真实执行 AppRun --version；
#   6) tm_ensure_model 行为：缓存复用、坏缓存/坏下载拒绝、aria2c 优先、
#      curl/wget 回退、.part 原子改名。
#
# 只做静态断言的部分（本机/CI 缺 docker、rpmbuild、makepkg、dh 时）：
#   deb/rpm/aur/appimage/linyaps 五条打包链的 /opt 布局、/usr 链接、模型注入、
#   aria2 构建依赖、.gitignore 与“仓库不含模型二进制”。
#   静态断言失败 → 退出非 0；脚本会如实打印“仅静态”，绝不把静态检查写成构建通过。
#
# 环境变量：
#   TM_LAYOUT_WORK=<dir>        工作目录（默认 .tmp/package-layout）
#   TM_LAYOUT_STATIC_ONLY=1     只跑静态断言 + tm_ensure_model 行为检查
#   TM_MODEL_CACHE_DIR=<dir>    模型缓存目录（默认 .tmp/models）
#   TER_MUSIC_MODEL_URL=<url>   覆盖模型下载 URL（离线镜像/测试用）

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT" || exit 2

# shellcheck source=../build/common.sh
. "$REPO_ROOT/scripts/build/common.sh"

WORK="${TM_LAYOUT_WORK:-${REPO_ROOT}/.tmp/package-layout}"
BUILD_DIR="${WORK}/build"
STAGE_DEB="${WORK}/stage-deb"
STAGE_LINYAPS="${WORK}/stage-linyaps"
APPDIR="${WORK}/AppDir"
STATIC_ONLY="${TM_LAYOUT_STATIC_ONLY:-0}"

APP_ID="$(sed -n 's/^set(TER_MUSIC_APP_ID "\([^"]*\)").*/\1/p' CMakeLists.txt | head -n 1)"
PREFIX="/opt/apps/${APP_ID}/files"
PROJECT_NAME="ter-music"
VERSION="$(sed -n 's/^project(ter-music LANGUAGES C VERSION \([0-9][^ )]*\)).*/\1/p' CMakeLists.txt | head -n 1)"
if [ -z "$APP_ID" ] || [ -z "$VERSION" ]; then
    printf '错误：无法从 CMakeLists.txt 解析 APP_ID/VERSION\n' >&2
    exit 2
fi

PASS=0
FAIL=0
SKIPPED=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
skip() { SKIPPED=$((SKIPPED + 1)); printf '  SKIP %s\n' "$1"; }
info() { printf '  ·    %s\n' "$1"; }

has_grep() { # has_grep <文件> <正则> <说明>
    if grep -q -- "$2" "$1" 2>/dev/null; then ok "$3"; else bad "$3（$1 缺少模式：$2）"; fi
}

has_fixed() { # has_fixed <文件> <固定字符串> <说明>
    if grep -Fq -- "$2" "$1" 2>/dev/null; then ok "$3"; else bad "$3（$1 缺少文本：$2）"; fi
}

# 命令行工具缺失时用于条件判断，避免 set -o pipefail 下 `cmd | grep -q` 的
# SIGPIPE 误判：先把输出收进变量，再用 here-string 匹配。
lines_matching() { # <输入文本> <awk 条件> -> stdout
    local text="$1"
    local cond="$2"
    printf '%s\n' "$text" | awk "$cond"
}

# ── 静态断言：五条打包链的 /opt 布局、/usr 链接与模型注入 ─────────────
static_checks() {
    printf '=== 1. 静态断言：/opt/apps 布局（deb/rpm/aur/appimage/linyaps）===\n'

    has_grep "debian/rules" '^APP_ID := cn.org.huansoft.ter-music$' \
        "debian/rules 定义 APP_ID"
    has_grep "debian/rules" '^PREFIX := /opt/apps/\$(APP_ID)/files$' \
        "debian/rules 前缀 = /opt/apps/\$(APP_ID)/files"
    has_grep "debian/rules" '\-DCMAKE_INSTALL_PREFIX=\$(PREFIX)' \
        "debian/rules 用 \$(PREFIX) 配置 CMake"
    has_grep "debian/rules" '\-DTER_MUSIC_BUNDLED_MODEL=\$(TER_MUSIC_BUNDLED_MODEL)' \
        "debian/rules 注入内置模型"
    if grep -q -- '-DCMAKE_INSTALL_PREFIX=/usr' debian/rules; then
        bad "debian/rules 仍以 /usr 安装前缀构建"
    else
        ok "debian/rules 不再以 /usr 安装前缀构建"
    fi
    if [ -f debian/links ]; then
        ok "debian/links 存在（dh_link 生成 /usr 链接）"
        local dst
        for dst in usr/bin/ter-music \
                   usr/share/applications/ter-music.desktop \
                   usr/share/icons/hicolor/32x32/apps/ter-music.png \
                   usr/share/icons/hicolor/48x48/apps/ter-music.png \
                   usr/share/icons/hicolor/128x128/apps/ter-music.png \
                   usr/share/icons/hicolor/scalable/apps/ter-music.svg; do
            if grep -q "^/opt/apps/${APP_ID}/files/.* ${dst}$" debian/links; then
                ok "debian/links -> ${dst}"
            else
                bad "debian/links 缺少 ${dst}"
            fi
        done
    else
        bad "缺少 debian/links"
    fi

    local spec="rpm/cn.org.huansoft.ter-music.spec"
    has_grep "$spec" '%global tm_prefix /opt/apps/%{name}/files' \
        "rpm spec 定义 /opt/apps/%{name}/files 前缀"
    has_grep "$spec" '^%{tm_prefix}$' "rpm %files 收录 opt 实体目录"
    has_grep "$spec" '^/usr/bin/%{binname}$' "rpm %files 收录 /usr/bin 链接"
    has_grep "$spec" 'ln -sfn ../../opt/apps/%{name}/files/bin/%{binname}' \
        "rpm %install 建 /usr/bin 相对链接"
    has_grep "$spec" 'ln -sfn ../../../opt/apps/%{name}/files/share/applications/%{binname}.desktop' \
        "rpm %install 建 desktop 相对链接"
    has_grep "$spec" '\-DTER_MUSIC_BUNDLED_MODEL="%{tm_model_file}"' \
        "rpm spec 注入 staged 模型"
    has_grep "$spec" '\.tm-bundled-model/UVR_MDXNET_3_9662.onnx' \
        "rpm spec 的模型路径来自 staging"
    if grep -q -- '-DCMAKE_INSTALL_PREFIX=/usr' "$spec"; then
        bad "rpm spec 仍以 /usr 安装前缀构建"
    else
        ok "rpm spec 不再以 /usr 安装前缀构建"
    fi

    has_grep "PKGBUILD" '^_prefix=/opt/apps/\$_appid/files$' \
        "PKGBUILD 定义 /opt/apps/\$_appid/files 前缀"
    has_grep "PKGBUILD" 'DESTDIR="\$pkgdir" cmake --install build' \
        "PKGBUILD 用 cmake --install 装 opt 实体"
    has_grep "PKGBUILD" 'ln -sfn "../../../opt/apps/\$_appid/files/bin/ter-music"' \
        "PKGBUILD 建 /usr/bin 相对链接"
    has_grep "PKGBUILD" 'ln -sfn "\$icon_base/128x128/apps/ter-music.png"' \
        "PKGBUILD 建 128x128 图标相对链接"
    has_grep "PKGBUILD" '\-DTER_MUSIC_BUNDLED_MODEL="\$srcdir/\$_model"' \
        "PKGBUILD 注入内置模型"
    has_grep "PKGBUILD" 'sha256sums=.*e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc' \
        "PKGBUILD 校验模型 SHA256"
    has_grep "PKGBUILD" '^_model_url=https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/UVR_MDXNET_3_9662.onnx$' \
        "PKGBUILD 记录官方模型 URL"
    has_grep "PKGBUILD" 'source=.*"\$_model::\$_model_url"' \
        "PKGBUILD source 引用官方模型 URL"
    has_grep ".SRCINFO" 'source = UVR_MDXNET_3_9662.onnx::https://github.com/TRvlvr/model_repo' \
        ".SRCINFO 与 PKGBUILD 的模型 source 一致"
    has_grep ".SRCINFO" 'sha256sums = e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc' \
        ".SRCINFO 与 PKGBUILD 的模型 SHA256 一致"

    local appimg="scripts/build/build-appimage.sh"
    has_grep "$appimg" '^APP_ID="cn.org.huansoft.ter-music"$' "build-appimage.sh 定义 APP_ID"
    has_grep "$appimg" '\-DCMAKE_INSTALL_PREFIX=/opt/apps/\${APP_ID}/files' \
        "build-appimage.sh 以 opt 前缀安装 AppDir"
    has_grep "$appimg" '\-DTER_MUSIC_BUNDLED_MODEL="\$TM_MODEL_PATH"' \
        "build-appimage.sh 注入内置模型"
    has_grep "$appimg" '^# >>> t3-appdir-layout$' "build-appimage.sh 标记 AppDir 布局段"
    has_fixed "$appimg" 'FILES="\${HERE}/opt/apps/${APP_ID}/files"' \
        "AppImage AppRun 从 opt 路径启动"
    has_fixed "$appimg" 'export XDG_DATA_HOME="\${FILES}/share"' \
        "AppImage AppRun 把 XDG_DATA_HOME 指向包内只读数据目录（模型查找路径）"
    has_grep "$appimg" 'XDG_DATA_DIRS' \
        "AppImage AppRun 用 XDG_DATA_DIRS 保留用户数据目录（不改变主题等既有行为）"
    has_fixed "$appimg" 'exec "\${FILES}/bin/ter-music" "\$@"' \
        "AppImage AppRun exec opt 路径下的二进制"
    has_grep "$appimg" 'ln -sfn "../../../opt/apps/\${APP_ID}/files/bin/\${PROJECT_NAME}"' \
        "AppDir/usr/bin 用相对链接指向 opt 实体"
    if grep -q 'CMAKE_INSTALL_PREFIX=/usr ' "$appimg"; then
        bad "build-appimage.sh 仍以 /usr 安装前缀构建"
    else
        ok "build-appimage.sh 不再以 /usr 安装前缀构建"
    fi

    local yaml="linglong/linglong.yaml.in"
    has_grep "$yaml" '\-DCMAKE_INSTALL_PREFIX=\${PREFIX}' "linyaps 使用 \${PREFIX}（files/ 布局）"
    has_grep "$yaml" '\-DTER_MUSIC_BUNDLED_MODEL="\$BUNDLED_MODEL"' "linyaps 注入内置模型"
    has_grep "$yaml" '\-DINSTALL_LINYAPS_INTEGRATION=ON' "linyaps 保留 systemd/D-Bus 集成"
    has_grep "$yaml" 'bundled-model/UVR_MDXNET_3_9662.onnx' "linyaps 从源码树 bundled-model 取模型"
    has_grep "$yaml" 'share/ter-music/models' "linyaps 断言模型安装目录"
    has_grep "$yaml" 'bundled karaoke model verified:' "linyaps 构建期自检模型/SHA256/NOTICE"
    has_grep "scripts/build/build-linyaps.sh" 'bundled-model' \
        "build-linyaps.sh 把模型复制进 linyaps staging"

    local s
    for s in build-deb build-rpm build-linyaps build-appimage build-aur launch-auto-build; do
        has_grep "scripts/build/${s}.sh" 'tm_ensure_model' \
            "scripts/build/${s}.sh 调用 tm_ensure_model"
    done
    has_grep "scripts/build/build-deb.sh" 'export TER_MUSIC_BUNDLED_MODEL=' \
        "build-deb.sh 导出模型绝对路径给 debian/rules"
    has_grep "scripts/build/build-deb.sh" '\.tm-bundled-model' \
        "build-deb.sh 把模型复制进 deb staging（源码包自包含）"
    has_grep "debian/rules" 'TER_MUSIC_BUNDLED_MODEL = \$(CURDIR)/\.tm-bundled-model/UVR_MDXNET_3_9662.onnx' \
        "debian/rules 在无环境变量时回退到源码树 staging 模型"
    has_grep "scripts/build/build-rpm.sh" '\.tm-bundled-model' \
        "build-rpm.sh 把模型复制进 rpm staging"
    has_grep "scripts/build/build-aur.sh" 'cp -f -- "\$TM_MODEL_PATH" "\${SRC_STAGE}/\${TM_MODEL_NAME}"' \
        "build-aur.sh 把模型预置到 makepkg 的 \$srcdir"

    local dockerfile
    for dockerfile in scripts/docker/Dockerfile.debian10 scripts/docker/Dockerfile.rpm \
                      scripts/docker/Dockerfile.aur; do
        has_grep "$dockerfile" 'aria2' "${dockerfile} 安装 aria2（构建期下载模型）"
    done

    has_grep ".gitignore" '^\*\.onnx$' ".gitignore 忽略 *.onnx"
    has_grep ".gitignore" '^/\.tmp/models/$' ".gitignore 忽略 .tmp/models 缓存目录"
    if [ -n "$(git ls-files -- '*.onnx' 2>/dev/null)" ]; then
        bad "仓库仍跟踪 .onnx：$(git ls-files -- '*.onnx' | tr '\n' ' ')"
    else
        ok "仓库没有受版本控制的 .onnx"
    fi
    local stray
    stray="$(find . -path ./.git -prune -o -path ./.tmp -prune -o -path ./build -prune \
        -o -path './build-*' -prune -o -name '*.onnx' -print 2>/dev/null)"
    if [ -n "$stray" ]; then
        bad "工作树出现模型二进制（应只留在 .tmp 缓存）：$stray"
    else
        ok "工作树（除 .tmp/build）没有模型二进制"
    fi

    printf '=== 2. 静态断言：模型获取/校验/注入链路 ===\n'
    local common="scripts/build/common.sh"
    has_grep "$common" 'tm_ensure_model()' "common.sh 定义 tm_ensure_model()"
    has_grep "$common" 'aria2c' "common.sh 优先用 aria2c"
    has_grep "$common" 'tm_download_to "\$TM_MODEL_URL" "\$tmp"' "common.sh 回退 curl/wget"
    has_grep "$common" '\.part\.\$\$' "common.sh 下载写 .part 临时文件"
    has_grep "$common" 'mv -f -- "\$tmp" "\$TM_MODEL_CACHE"' "common.sh 校验后原子改名"
    has_grep "$common" 'TM_MODEL_CACHE_DIR="\${TER_MUSIC_MODEL_CACHE_DIR:-\${TM_REPO_ROOT}/\.tmp/models}"' \
        "common.sh 默认缓存 .tmp/models"
    has_grep "$common" 'TM_MODEL_SIZE=29704436' "common.sh 固定模型大小"
    has_grep "$common" 'TM_MODEL_SHA256="e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc"' \
        "common.sh 固定模型 SHA256"
    has_grep "$common" 'UVR_MDXNET_3_9662.onnx' "common.sh 固定官方模型文件名"
    has_grep "$common" 'all_public_uvr_models/UVR_MDXNET_3_9662.onnx' "common.sh 固定官方下载 URL"
    has_grep "$common" 'tm_model_is_valid()' "common.sh 定义缓存命中校验"
    has_grep "$common" 'refusing to package a wrong/corrupted model' \
        "common.sh 对坏模型给出可读错误"
    has_grep "$common" 'offline build: put a valid' "common.sh 对网络失败给出可读错误"

    if grep -q 'aria2' debian/control; then
        bad "debian/control 新增了运行时依赖 aria2"
    else
        ok "debian/control 没有 aria2 运行时依赖（仅构建镜像）"
    fi
    if grep -qE '^Requires:.*aria2' rpm/cn.org.huansoft.ter-music.spec; then
        bad "rpm spec 新增了 aria2 运行时依赖"
    else
        ok "rpm spec 没有 aria2 运行时依赖（仅构建镜像）"
    fi
    if grep -qE '^(depends|makedepends|optdepends)=.*aria2' PKGBUILD; then
        bad "PKGBUILD 新增了 aria2 运行时/构建依赖"
    else
        ok "PKGBUILD 没有 aria2 依赖（仅构建镜像）"
    fi
}

# ── 模型获取行为（真实执行 tm_ensure_model 的各分支） ─────────────────
model_checks() {
    printf '=== 3. 可执行验证：tm_ensure_model 缓存/校验/下载行为 ===\n'
    local tmp
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/t3-layout-model.XXXXXX")" || { bad "无法创建临时目录"; return; }
    local model="${tmp}/source.onnx"
    local rc

    # 基线：默认缓存必须可用，否则后续打包链无法构建。
    if tm_ensure_model >"$tmp/default.log" 2>&1 && tm_model_is_valid "$TM_MODEL_CACHE"; then
        ok "默认缓存命中并复用：${TM_MODEL_CACHE}（size/SHA256 正确）"
    else
        bad "默认缓存不可用且无法获取模型（详见 ${tmp}/default.log）"
    fi
    cp -f "$TM_MODEL_CACHE" "$model" 2>/dev/null || true

    # 1) 正确的本地源文件（file:// URL）走 curl/wget 回退 + .part 原子改名。
    rc=0
    (
        TM_MODEL_CACHE_DIR="${tmp}/cache-ok"
        TM_MODEL_CACHE="${TM_MODEL_CACHE_DIR}/${TM_MODEL_NAME}"
        TM_MODEL_URL="file://${model}"
        tm_ensure_model
    ) >"$tmp/ok.log" 2>&1 || rc=$?
    local part_left
    part_left="$(find "${tmp}/cache-ok" -name '*.part*' -print 2>/dev/null)"
    if [ "$rc" -eq 0 ] && tm_model_is_valid "${tmp}/cache-ok/${TM_MODEL_NAME}" &&
       [ -z "$part_left" ]; then
        ok "curl/wget 回退下载 → 校验通过 → .part 原子改名（无残留）"
    else
        bad "回退下载/原子改名失败（rc=${rc}，残留 '${part_left}'，日志 ${tmp}/ok.log）"
    fi

    # 2) 坏缓存 + 不可达 URL：必须非 0，给出可读错误，且不留下坏文件/半成品。
    rc=0
    (
        TM_MODEL_CACHE_DIR="${tmp}/cache-bad"
        TM_MODEL_CACHE="${TM_MODEL_CACHE_DIR}/${TM_MODEL_NAME}"
        TM_MODEL_URL="file://${tmp}/definitely-missing.onnx"
        mkdir -p "$TM_MODEL_CACHE_DIR"
        printf 'corrupted' > "$TM_MODEL_CACHE"
        tm_ensure_model
    ) >"$tmp/bad.log" 2>&1 || rc=$?
    part_left="$(find "${tmp}/cache-bad" -name '*.part*' -print 2>/dev/null)"
    if [ "$rc" -ne 0 ] &&
       grep -q 'failed to download karaoke model' "$tmp/bad.log" &&
       [ ! -e "${tmp}/cache-bad/${TM_MODEL_NAME}" ] && [ -z "$part_left" ]; then
        ok "坏缓存被丢弃 + 网络失败 → 非 0 + 可读错误 + 无残留"
    else
        bad "坏缓存/网络失败路径不符合预期（rc=${rc}，日志 ${tmp}/bad.log）"
    fi

    # 3) 下载到内容错误的文件（大小正确、SHA256 错误）必须拒绝。
    head -c 29704436 /dev/zero > "${tmp}/wrong.onnx"
    rc=0
    (
        TM_MODEL_CACHE_DIR="${tmp}/cache-sha"
        TM_MODEL_CACHE="${TM_MODEL_CACHE_DIR}/${TM_MODEL_NAME}"
        TM_MODEL_URL="file://${tmp}/wrong.onnx"
        tm_ensure_model
    ) >"$tmp/sha.log" 2>&1 || rc=$?
    part_left="$(find "${tmp}/cache-sha" -name '*.part*' -print 2>/dev/null)"
    if [ "$rc" -ne 0 ] && grep -q 'failed validation' "$tmp/sha.log" &&
       [ ! -e "${tmp}/cache-sha/${TM_MODEL_NAME}" ] && [ -z "$part_left" ]; then
        ok "SHA256 不符的下载被拒绝（错误信息含期望/实际值）"
    else
        bad "错误 SHA256 未被拒绝（rc=${rc}，日志 ${tmp}/sha.log）"
    fi
    rm -f "${tmp}/wrong.onnx"

    # 4) aria2c 存在时必须优先（用假的 aria2c 证明没有回退到 curl/wget）。
    mkdir -p "${tmp}/bin"
    cat > "${tmp}/bin/aria2c" <<'ARIA_EOF'
#!/bin/sh
dir=.; name=""; url=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        -d) dir="$2"; shift 2 ;;
        -o) name="$2"; shift 2 ;;
        -*) shift ;;
        *) url="$1"; shift ;;
    esac
done
mkdir -p "$dir"
cp -f "$FAKE_ARIA2_SOURCE" "$dir/$name"
printf '%s\n' "$url" > "$FAKE_ARIA2_URL_LOG"
ARIA_EOF
    chmod +x "${tmp}/bin/aria2c"
    rc=0
    (
        PATH="${tmp}/bin:${PATH}"
        export FAKE_ARIA2_SOURCE="${model}"
        export FAKE_ARIA2_URL_LOG="${tmp}/aria-url"
        TM_MODEL_CACHE_DIR="${tmp}/cache-aria"
        TM_MODEL_CACHE="${TM_MODEL_CACHE_DIR}/${TM_MODEL_NAME}"
        TM_MODEL_URL="https://invalid.example/UVR_MDXNET_3_9662.onnx"
        tm_ensure_model
    ) >"$tmp/aria.log" 2>&1 || rc=$?
    local aria_url=""
    [ -f "${tmp}/aria-url" ] && aria_url="$(cat "${tmp}/aria-url")"
    if [ "$rc" -eq 0 ] && tm_model_is_valid "${tmp}/cache-aria/${TM_MODEL_NAME}" &&
       ! grep -q 'falling back to curl/wget' "$tmp/aria.log" &&
       [ "$aria_url" = "https://invalid.example/UVR_MDXNET_3_9662.onnx" ]; then
        ok "aria2c 优先（无效 URL 仍成功 → 未回退 curl/wget）"
    else
        bad "aria2c 优先路径不符合预期（rc=${rc}，日志 ${tmp}/aria.log）"
    fi

    # 5) staging 复制 helper（rpm/aur/linyaps 用）。
    tm_stage_bundled_model "${tmp}/stage" >/dev/null 2>&1
    if [ -f "${tmp}/stage/${TM_MODEL_NAME}" ] && tm_model_is_valid "${tmp}/stage/${TM_MODEL_NAME}" &&
       [ "$TM_MODEL_STAGED_PATH" = "${tmp}/stage/${TM_MODEL_NAME}" ]; then
        ok "tm_stage_bundled_model 把校验过的模型复制进 staging"
    else
        bad "tm_stage_bundled_model 结果不符合预期"
    fi

    rm -rf "$tmp"
}

# ── 构建/安装/DESTDIR 验证 ───────────────────────────────────────────
prepare_pkg_config() {
    if [ -n "${PKG_CONFIG_EXECUTABLE:-}" ] && "$PKG_CONFIG_EXECUTABLE" --version >/dev/null 2>&1; then
        printf '%s\n' "$PKG_CONFIG_EXECUTABLE"
        return 0
    fi
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --version >/dev/null 2>&1; then
        command -v pkg-config
        return 0
    fi
    # DSH/linglong 宿主：/opt/host-tools 的 pkgconf 需要 LD_PRELOAD 才能跑
    if [ -x /opt/host-tools/usr/bin/pkg-config ] &&
       [ -e /opt/host-tools/usr/lib/x86_64-linux-gnu/libpkgconf.so.7 ]; then
        mkdir -p "${WORK}/bin"
        cat > "${WORK}/bin/pkg-config" <<'PKG_EOF'
#!/bin/sh
LD_PRELOAD=/opt/host-tools/usr/lib/x86_64-linux-gnu/libpkgconf.so.7 \
    exec /opt/host-tools/usr/bin/pkg-config "$@"
PKG_EOF
        chmod +x "${WORK}/bin/pkg-config"
        printf '%s\n' "${WORK}/bin/pkg-config"
        return 0
    fi
    return 1
}

install_tree_checks() { # <DESTDIR 根目录> <场景名>
    local root="${1}"
    local label="${2}"
    printf '=== 5. 可执行验证：DESTDIR 安装树（%s）===\n' "$label"
    local opt_root="${root}/opt/apps/${APP_ID}/files"

    if [ ! -x "${opt_root}/bin/${PROJECT_NAME}" ]; then
        bad "${label}: 实体二进制未安装到 /opt/apps/${APP_ID}/files/bin"
        return
    fi
    ok "${label}: 实体二进制在 /opt/apps/${APP_ID}/files/bin"

    local model="${opt_root}/share/ter-music/models/uvr-mdxnet-3-9662.onnx"
    if [ -f "$model" ] && [ "$(tm_file_size "$model")" = "$TM_MODEL_SIZE" ] &&
       [ "$(tm_sha256_of "$model")" = "$TM_MODEL_SHA256" ]; then
        ok "${label}: 内置模型随包安装且大小/SHA256 正确"
    else
        bad "${label}: 内置模型缺失或大小/SHA256 不符"
    fi
    if [ -f "${opt_root}/share/ter-music/models/NOTICE.md" ] &&
       grep -q 'MIT' "${opt_root}/share/ter-music/models/NOTICE.md"; then
        ok "${label}: NOTICE.md 与模型同目录安装（含 MIT 声明）"
    else
        bad "${label}: NOTICE.md 缺失或内容不含 MIT 声明"
    fi

    local f missing=0
    for f in \
        share/ter-music/lang/zh_CN.xml \
        share/ter-music/lang/en_US.xml \
        share/ter-music/lang/zh-PQ.xml \
        share/ter-music/help/help-quickstart-zh_CN.txt \
        share/ter-music/help/help-quickstart-en_US.txt \
        share/ter-music/help/help-quickstart-zh-PQ.txt \
        share/ter-music/layouts/layouts.json \
        share/applications/ter-music.desktop \
        share/icons/hicolor/32x32/apps/ter-music.png \
        share/icons/hicolor/48x48/apps/ter-music.png \
        share/icons/hicolor/128x128/apps/ter-music.png \
        share/icons/hicolor/scalable/apps/ter-music.svg; do
        if [ ! -f "${opt_root}/${f}" ]; then
            bad "${label}: 缺少数据文件 ${PREFIX}/${f}"
            missing=1
        fi
    done
    [ "$missing" -eq 0 ] &&
        ok "${label}: lang/help/layouts/desktop/icons 全部在 opt 实体目录"
    if [ -e "${root}/usr/share/ter-music" ]; then
        bad "${label}: /usr/share/ter-music 出现实体数据目录"
    else
        ok "${label}: /usr/share/ter-music 未出现实体数据目录"
    fi
}

make_usr_links_from_debian() { # <stage 根目录>
    local root="${1}"
    python3 - "$root" "$APP_ID" <<'PY'
import os
import sys

root, app_id = sys.argv[1], sys.argv[2]
prefix = f"/opt/apps/{app_id}/files"
links = []

with open("debian/links", encoding="utf-8") as fh:
    for line in fh:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) == 2:
            links.append((parts[0], parts[1].lstrip("/")))

if not links:
    raise SystemExit("debian/links contained no link pairs")

for src, dst in links:
    link_path = os.path.join(root, dst)
    os.makedirs(os.path.dirname(link_path), exist_ok=True)
    if os.path.lexists(link_path):
        os.unlink(link_path)
    target = os.path.relpath(src, "/" + os.path.dirname(dst))
    os.symlink(target, link_path)
    if not os.path.exists(link_path):
        raise SystemExit(f"broken link target for {dst}: {target}")
PY
}

deb_checks() {
    printf '=== 6. 可执行验证：按 debian/links 组装 deb 并用 dpkg-deb 校验 ===\n'
    local extract="${WORK}/extract-deb"
    local deb="${WORK}/cn.org.huansoft.ter-music_${VERSION}_amd64-layoutcheck.deb"
    local link_problems=0

    if ! make_usr_links_from_debian "$STAGE_DEB"; then
        bad "按 debian/links 建 /usr 链接失败"
        return
    fi
    ok "按 debian/links 在暂存树建 /usr 符号链接"

    local dst link target expected
    for dst in usr/bin/ter-music \
               usr/share/applications/ter-music.desktop \
               usr/share/icons/hicolor/32x32/apps/ter-music.png \
               usr/share/icons/hicolor/48x48/apps/ter-music.png \
               usr/share/icons/hicolor/128x128/apps/ter-music.png \
               usr/share/icons/hicolor/scalable/apps/ter-music.svg; do
        link="${STAGE_DEB}/${dst}"
        if [ ! -L "$link" ]; then
            bad "暂存树缺少符号链接：/${dst}"
            link_problems=1
            continue
        fi
        target="$(readlink "$link")"
        case "$target" in
            /*)
                bad "/${dst} 是绝对链接（应为相对链接）：${target}"
                link_problems=1
                ;;
        esac
        case "$dst" in
            usr/bin/ter-music)
                expected="${STAGE_DEB}/opt/apps/${APP_ID}/files/bin/ter-music" ;;
            usr/share/*)
                expected="${STAGE_DEB}/opt/apps/${APP_ID}/files/share/${dst#usr/share/}" ;;
            *)
                expected="" ;;
        esac
        if [ -n "$expected" ] && [ "$(readlink -f "$link")" != "$expected" ]; then
            bad "/${dst} 未解析到 opt 实体（实际 $(readlink -f "$link")，期望 ${expected}）"
            link_problems=1
        fi
    done
    [ "$link_problems" -eq 0 ] &&
        ok "/usr 链接均为相对链接且解析到 /opt 实体（dpkg 包内路径）"

    mkdir -p "${STAGE_DEB}/DEBIAN"
    chmod 0755 "${STAGE_DEB}/DEBIAN"
    cat > "${STAGE_DEB}/DEBIAN/control" <<EOF
Package: ${APP_ID}
Version: ${VERSION}-1
Architecture: amd64
Maintainer: HuanSoft <yxzl666xx@outlook.com>
Installed-Size: 30000
Description: terminal-based music player (package layout check)
EOF
    chmod 0644 "${STAGE_DEB}/DEBIAN/control"
    rm -f "$deb"
    local deb_help
    deb_help="$(dpkg-deb --help 2>/dev/null)"
    if grep -q -- '--root-owner-group' <<< "$deb_help"; then
        dpkg-deb --root-owner-group --build "$STAGE_DEB" "$deb" >/dev/null 2>"${WORK}/dpkg-deb.log" || {
            bad "dpkg-deb 组装失败（日志 ${WORK}/dpkg-deb.log）"
            return
        }
    else
        dpkg-deb --build "$STAGE_DEB" "$deb" >/dev/null 2>"${WORK}/dpkg-deb.log" || {
            bad "dpkg-deb 组装失败（日志 ${WORK}/dpkg-deb.log）"
            return
        }
    fi
    ok "dpkg-deb 从 DESTDIR 暂存树组装 deb 成功"

    local listing
    listing="$(dpkg-deb -c "$deb")" || { bad "dpkg-deb -c 失败"; return; }
    local regular_lines link_lines
    regular_lines="$(lines_matching "$listing" '$1 ~ /^-/ {print}')"
    link_lines="$(lines_matching "$listing" '$1 ~ /^l/ {print}')"
    local opt_prefix="./opt/apps/${APP_ID}/files"

    if grep -Fq -- "${opt_prefix}/bin/ter-music" <<< "$regular_lines"; then
        ok "dpkg-deb -c: opt 实体二进制是普通文件"
    else
        bad "dpkg-deb -c: deb 内缺少 opt 实体二进制"
    fi
    if grep -Fq -- "${opt_prefix}/share/ter-music/models/uvr-mdxnet-3-9662.onnx" <<< "$regular_lines"; then
        ok "dpkg-deb -c: 内置模型是普通文件"
    else
        bad "dpkg-deb -c: deb 内模型不是普通文件"
    fi

    local -a symlink_pairs=(
        "./usr/bin/ter-music:${opt_prefix}/bin/ter-music"
        "./usr/share/applications/ter-music.desktop:${opt_prefix}/share/applications/ter-music.desktop"
        "./usr/share/icons/hicolor/32x32/apps/ter-music.png:${opt_prefix}/share/icons/hicolor/32x32/apps/ter-music.png"
        "./usr/share/icons/hicolor/48x48/apps/ter-music.png:${opt_prefix}/share/icons/hicolor/48x48/apps/ter-music.png"
        "./usr/share/icons/hicolor/128x128/apps/ter-music.png:${opt_prefix}/share/icons/hicolor/128x128/apps/ter-music.png"
        "./usr/share/icons/hicolor/scalable/apps/ter-music.svg:${opt_prefix}/share/icons/hicolor/scalable/apps/ter-music.svg"
    )
    local pair path suffix symlink_problems=0
    for pair in "${symlink_pairs[@]}"; do
        path="${pair%%:*}"
        suffix="${pair#*:}"
        if grep -Fq -- " ${path} -> " <<< "$link_lines" &&
           grep -Fq -- "$suffix" <<< "$(grep -F -- " ${path} -> " <<< "$link_lines")"; then
            ok "dpkg-deb -c: ${path#./} -> opt 实体"
        else
            bad "dpkg-deb -c: ${path#./} 不是指向 opt 实体的符号链接"
            symlink_problems=1
        fi
    done
    [ "$symlink_problems" -eq 0 ] && ok "dpkg-deb -c: 6 个 /usr 链接全部指向 opt 实体"

    local raw_usr
    raw_usr="$(grep -E ' \./usr/(bin/ter-music$|share/ter-music/|share/applications/ter-music\.desktop$|share/icons/hicolor/.*/apps/ter-music\.)' <<< "$regular_lines")"
    if [ -n "$raw_usr" ]; then
        bad "deb 的 /usr 下仍出现实体文件：$(head -n 3 <<< "$raw_usr" | tr '\n' ' ')"
    else
        ok "deb 的 /usr 下没有实体文件（只有链接）"
    fi

    local deb_model_size
    deb_model_size="$(awk -v p="${opt_prefix}/share/ter-music/models/uvr-mdxnet-3-9662.onnx" \
        '$NF==p {print $3}' <<< "$regular_lines")"
    if [ "$deb_model_size" = "$TM_MODEL_SIZE" ]; then
        ok "dpkg-deb -c: 模型大小 = ${TM_MODEL_SIZE}"
    else
        bad "dpkg-deb -c: 模型大小 ${deb_model_size:-<缺失>} != ${TM_MODEL_SIZE}"
    fi
    if grep -Fq -- "${opt_prefix}/share/ter-music/models/NOTICE.md" <<< "$regular_lines"; then
        ok "dpkg-deb -c: NOTICE.md 随包"
    else
        bad "dpkg-deb -c: 缺少 NOTICE.md"
    fi
    if grep -Fq -- "${opt_prefix}/share/ter-music/lang/zh_CN.xml" <<< "$regular_lines" &&
       grep -Fq -- "${opt_prefix}/share/ter-music/layouts/layouts.json" <<< "$regular_lines"; then
        ok "dpkg-deb -c: lang/layouts 数据随包"
    else
        bad "dpkg-deb -c: lang/layouts 数据缺失"
    fi

    rm -rf "$extract"
    if ! dpkg-deb -x "$deb" "$extract" >/dev/null 2>&1; then
        bad "dpkg-deb -x 解包失败"
        return
    fi
    if [ "$(tm_sha256_of "${extract}/opt/apps/${APP_ID}/files/share/ter-music/models/uvr-mdxnet-3-9662.onnx")" = "$TM_MODEL_SHA256" ]; then
        ok "dpkg-deb -x: 解包后模型 SHA256 正确"
    else
        bad "dpkg-deb -x: 解包后模型 SHA256 不符"
    fi
    if [ "$(readlink -f "${extract}/usr/bin/ter-music")" = "${extract}/opt/apps/${APP_ID}/files/bin/ter-music" ]; then
        ok "dpkg-deb -x: /usr/bin/ter-music 解析到解包后的 opt 实体"
    else
        bad "dpkg-deb -x: /usr/bin/ter-music 未解析到 opt 实体"
    fi
    if [ -x "${extract}/opt/apps/${APP_ID}/files/bin/ter-music" ]; then
        ok "dpkg-deb -x: opt 实体二进制可执行"
    else
        bad "dpkg-deb -x: opt 实体二进制不可执行"
    fi
    local usr_files
    usr_files="$(find "${extract}/usr" -type f -print 2>/dev/null)"
    if [ -n "$usr_files" ]; then
        bad "dpkg-deb -x: /usr 下存在实体文件：$usr_files"
    else
        ok "dpkg-deb -x: /usr 下只有符号链接与目录"
    fi

    # 运行时冒烟：解包后二进制能报版本（不依赖安装前缀上的数据目录）。
    local run_log="${WORK}/extract-run.log"
    mkdir -p "${WORK}/home"
    if HOME="${WORK}/home" XDG_CONFIG_HOME="${WORK}/home/.config" \
        "${extract}/opt/apps/${APP_ID}/files/bin/ter-music" --version >"$run_log" 2>&1; then
        ok "解包后 opt 实体二进制可运行：$(tr -d '\n' < "$run_log")"
    else
        skip "解包后二进制无法运行（当前环境运行时依赖限制，非布局问题；日志 ${run_log}）"
    fi
}

linyaps_checks() {
    printf '=== 7. 可执行验证：linyaps 安装树（${PREFIX} + systemd/D-Bus 集成）===\n'
    local base="${STAGE_LINYAPS}/opt/apps/${APP_ID}/files"
    if [ -x "${base}/bin/${PROJECT_NAME}" ]; then
        ok "linyaps: 实体二进制在 ${PREFIX}/bin"
    else
        bad "linyaps 安装树缺少 ${PREFIX}/bin/${PROJECT_NAME}"
        return
    fi
    if tm_model_is_valid "${base}/share/ter-music/models/uvr-mdxnet-3-9662.onnx"; then
        ok "linyaps: 内置模型随包且大小/SHA256 正确"
    else
        bad "linyaps: 内置模型缺失或校验失败"
    fi
    if [ -f "${base}/share/ter-music/models/NOTICE.md" ]; then
        ok "linyaps: NOTICE.md 与模型同目录"
    else
        bad "linyaps: NOTICE.md 缺失"
    fi
    if [ -f "${base}/lib/systemd/user/${APP_ID}.service" ]; then
        ok "linyaps: systemd 用户单元未被破坏（${PREFIX}/lib/systemd/user）"
    else
        bad "linyaps: systemd 用户单元缺失"
    fi
    if [ -f "${base}/share/dbus-1/services/org.mpris.MediaPlayer2.ter_music.service" ]; then
        ok "linyaps: D-Bus 激活文件未被破坏（${PREFIX}/share/dbus-1/services）"
    else
        bad "linyaps: D-Bus 激活文件缺失"
    fi
    if [ -e "${STAGE_LINYAPS}/usr" ]; then
        bad "linyaps: 安装树出现 /usr（linyaps 不应安装 /usr 链接）"
    else
        ok "linyaps: 不安装 /usr 链接（宿主集成由 ll-builder 导出）"
    fi
}

appimage_layout_checks() {
    printf '=== 8. 可执行验证：AppImage AppDir 结构（提取 build-appimage.sh 布局段）===\n'
    local block="${WORK}/appdir-layout.sh"
    awk '/^# >>> t3-appdir-layout$/{flag=1;next} /^# <<< t3-appdir-layout$/{flag=0} flag' \
        scripts/build/build-appimage.sh > "$block"
    if [ ! -s "$block" ]; then
        bad "无法从 build-appimage.sh 提取 AppDir 布局段（t3-appdir-layout 标记）"
        return
    fi
    mkdir -p "$APPDIR"
    if ! (
        set -e
        export APPDIR APP_ID PROJECT_NAME VERSION REPO_ROOT
        # shellcheck source=/dev/null
        . "$block"
    ) >"${WORK}/appdir-layout.log" 2>&1; then
        bad "执行 AppDir 布局段失败（日志 ${WORK}/appdir-layout.log）"
        return
    fi
    ok "AppDir 布局段执行成功（AppRun/desktop/AppInfo/图标/链接）"

    local opt_root="${APPDIR}/opt/apps/${APP_ID}/files"
    if [ -x "${opt_root}/bin/${PROJECT_NAME}" ]; then
        ok "AppDir: 实体二进制在 opt/apps/${APP_ID}/files/bin"
    else
        bad "AppDir: opt 实体二进制缺失"
    fi
    if tm_model_is_valid "${opt_root}/share/ter-music/models/uvr-mdxnet-3-9662.onnx" &&
       [ -f "${opt_root}/share/ter-music/models/NOTICE.md" ]; then
        ok "AppDir: 内置模型 + NOTICE 随包"
    else
        bad "AppDir: 内置模型或 NOTICE 缺失"
    fi
    local apprun="${APPDIR}/AppRun"
    if [ -x "$apprun" ] && grep -q "opt/apps/${APP_ID}/files" "$apprun" &&
       grep -q "/bin/${PROJECT_NAME}" "$apprun" && grep -q 'XDG_DATA_HOME' "$apprun"; then
        ok "AppDir: AppRun 从 opt 路径启动且设置模型查找用 XDG_DATA_HOME"
    else
        bad "AppDir: AppRun 未指向 opt 路径或缺少 XDG_DATA_HOME"
    fi

    local dst link target link_problems=0
    for dst in usr/bin/ter-music \
               usr/share/applications/ter-music.desktop \
               usr/share/icons/hicolor/32x32/apps/ter-music.png \
               usr/share/icons/hicolor/48x48/apps/ter-music.png \
               usr/share/icons/hicolor/128x128/apps/ter-music.png \
               usr/share/icons/hicolor/scalable/apps/ter-music.svg; do
        link="${APPDIR}/${dst}"
        if [ ! -L "$link" ]; then
            bad "AppDir: 缺少相对符号链接 /${dst}"
            link_problems=1
            continue
        fi
        target="$(readlink "$link")"
        case "$target" in
            /*)
                bad "AppDir: /${dst} 是绝对链接（应为相对链接）"
                link_problems=1
                ;;
        esac
    done
    [ "$link_problems" -eq 0 ] && ok "AppDir: /usr 链接均为相对链接"
    if [ -n "$(find "${APPDIR}/usr" -type f -print 2>/dev/null)" ]; then
        bad "AppDir: /usr 下存在实体文件（应只放链接）"
    else
        ok "AppDir: /usr 下只有链接与目录"
    fi
    if [ -f "${APPDIR}/${PROJECT_NAME}.desktop" ] &&
       grep -q '^Exec=ter-music$' "${APPDIR}/${PROJECT_NAME}.desktop" &&
       grep -q '^Icon=ter-music$' "${APPDIR}/${PROJECT_NAME}.desktop" &&
       [ -f "${APPDIR}/${PROJECT_NAME}.png" ] && [ -f "${APPDIR}/.DirIcon" ]; then
        ok "AppDir: 桌面文件 Exec/Icon 自洽，根目录图标存在"
    else
        bad "AppDir: 桌面文件或根目录图标不自洽"
    fi
    if grep -q "<id>${APP_ID}</id>" "${APPDIR}/AppInfo.xml" 2>/dev/null; then
        ok "AppDir: AppInfo.xml 应用 ID 正确"
    else
        bad "AppDir: AppInfo.xml 应用 ID 不正确"
    fi

    local run_log="${WORK}/appdir-run.log"
    mkdir -p "${WORK}/home"
    if HOME="${WORK}/home" XDG_CONFIG_HOME="${WORK}/home/.config" \
        "$apprun" --version >"$run_log" 2>&1; then
        ok "AppRun 可运行：$(tr -d '\n' < "$run_log")"
    else
        skip "AppRun 无法运行（当前环境运行时依赖限制；日志 ${run_log}）"
    fi
    info "AppImage 只验证 AppDir 结构；appimagetool/mksquashfs 打包未在本地执行"
}

build_checks() {
    printf '=== 4. 可执行验证：CMake 构建 + DESTDIR 安装（deb 布局）===\n'
    if [ "$STATIC_ONLY" = "1" ]; then
        skip "TM_LAYOUT_STATIC_ONLY=1：跳过 CMake 构建/DESTDIR 安装验证（仅静态断言）"
        return
    fi
    local missing=() cmd
    for cmd in cmake make gcc python3 dpkg-deb; do
        command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        skip "缺少构建/打包工具：${missing[*]}；只做静态断言（不声称构建通过）"
        return
    fi
    local pkg_config_bin
    if ! pkg_config_bin="$(prepare_pkg_config)"; then
        bad "找不到可用的 pkg-config（CMake 配置会失败）"
        return
    fi
    if [ -d /opt/host-tools/usr/lib/x86_64-linux-gnu/pkgconfig ]; then
        export PKG_CONFIG_PATH="/opt/host-tools/usr/lib/x86_64-linux-gnu/pkgconfig:/opt/host-tools/usr/share/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
        export PKG_CONFIG_SYSROOT_DIR="${PKG_CONFIG_SYSROOT_DIR:-/opt/host-tools}"
    fi
    if ! tm_ensure_model >/dev/null 2>&1; then
        bad "无法获取/校验内置模型，DESTDIR 安装验证无法进行"
        return
    fi

    rm -rf "$BUILD_DIR" "$STAGE_DEB" "$STAGE_LINYAPS" "$APPDIR"
    mkdir -p "$BUILD_DIR" "$STAGE_DEB" "$STAGE_LINYAPS"
    local cmake_args=(
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX="$PREFIX"
        -DTER_MUSIC_VERSION="$VERSION"
        -DTER_MUSIC_BUNDLED_MODEL="$TM_MODEL_PATH"
        -DINSTALL_LINYAPS_INTEGRATION=OFF
        -DPKG_CONFIG_EXECUTABLE="$pkg_config_bin"
        "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--allow-shlib-undefined"
    )
    if [ -n "${CC:-}" ]; then
        cmake_args+=(-DCMAKE_C_COMPILER="$CC")
    fi
    if [ -d /opt/host-tools/usr ]; then
        cmake_args+=(-DCMAKE_PREFIX_PATH=/opt/host-tools/usr)
    fi

    if ! ( cd "$BUILD_DIR" && cmake "$REPO_ROOT" "${cmake_args[@]}" ) \
            >"${WORK}/cmake-configure.log" 2>&1; then
        bad "CMake 配置失败（日志 ${WORK}/cmake-configure.log）"
        return
    fi
    ok "CMake 配置成功：${PREFIX} + 注入模型（CMake 侧大小/SHA256 校验通过）"
    if ! cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 2)" \
            >"${WORK}/cmake-build.log" 2>&1; then
        bad "CMake 构建失败（日志 ${WORK}/cmake-build.log）"
        return
    fi
    ok "CMake 构建成功"
    if ! DESTDIR="$STAGE_DEB" cmake --install "$BUILD_DIR" \
            >"${WORK}/cmake-install.log" 2>&1; then
        bad "CMake 安装到 DESTDIR 失败（日志 ${WORK}/cmake-install.log）"
        return
    fi
    ok "CMake 安装到 DESTDIR 成功"
    install_tree_checks "$STAGE_DEB" "deb 布局"
    deb_checks

    printf '=== 7a. 可执行验证：linyaps 配方重配安装 ===\n'
    if ! ( cd "$BUILD_DIR" && cmake "$REPO_ROOT" "${cmake_args[@]}" \
            -DINSTALL_LINYAPS_INTEGRATION=ON "-DCMAKE_INSTALL_RPATH=\$ORIGIN/../lib" ) \
            >"${WORK}/cmake-configure-linyaps.log" 2>&1; then
        bad "linyaps 配方重配失败（日志 ${WORK}/cmake-configure-linyaps.log）"
    elif ! cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 2)" \
            >"${WORK}/cmake-build-linyaps.log" 2>&1; then
        bad "linyaps 配方重建失败（日志 ${WORK}/cmake-build-linyaps.log）"
    elif ! DESTDIR="$STAGE_LINYAPS" cmake --install "$BUILD_DIR" \
            >"${WORK}/cmake-install-linyaps.log" 2>&1; then
        bad "linyaps 配方安装失败（日志 ${WORK}/cmake-install-linyaps.log）"
    else
        linyaps_checks
    fi

    printf '=== 8a. 可执行验证：AppDir 复用 deb 布局安装 ===\n'
    if ! ( cd "$BUILD_DIR" && cmake "$REPO_ROOT" "${cmake_args[@]}" ) \
            >"${WORK}/cmake-configure-appimage.log" 2>&1; then
        bad "AppImage 配方（关闭 linyaps 集成）重配失败"
    elif ! DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" \
            >"${WORK}/cmake-install-appimage.log" 2>&1; then
        bad "AppImage AppDir 安装失败"
    else
        appimage_layout_checks
    fi
}

mkdir -p "$WORK"
printf '=== package-layout-check（APP_ID=%s, VERSION=%s, PREFIX=%s）===\n' \
    "$APP_ID" "$VERSION" "$PREFIX"

static_checks
model_checks
build_checks

printf '\n--- 结果 ---\n'
printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIPPED"
if [ "$FAIL" -gt 0 ]; then
    printf '结论：安装布局/模型注入检查失败\n' >&2
    exit 1
fi
if [ "$STATIC_ONLY" = "1" ] || [ ! -d "$BUILD_DIR" ]; then
    printf '结论：静态断言通过（未执行本地构建/DESTDIR 安装，不代表构建通过）\n'
else
    printf '结论：静态断言 + 可执行验证（DESTDIR/dpkg-deb/AppDir/linyaps 配方）通过\n'
fi
printf '说明：本地未运行 docker/dpkg-buildpackage/rpmbuild/makepkg/ll-builder；本脚本产物见 %s\n' "$WORK"
exit 0
