#!/usr/bin/env bash
#
# 包名 / 版本 / 命名空间 / 打包入口一致性检查（身份矩阵）。
#
# 纯文本解析，不依赖 dbus / dpkg-dev / makepkg，CI 编译机可跑。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT" || exit 2

REPORT_ONLY=0
[ "${1:-}" = "--report" ] && REPORT_ONLY=1

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
info(){ printf '  ·    %s\n' "$1"; }

grep1() { # grep1 <文件> <正则（含且仅含一个捕获组）>
    [ -f "$1" ] || return 0
    python3 -c '
import re, sys
try:
    pat = re.compile(sys.argv[2])
    with open(sys.argv[1], encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = pat.search(line.rstrip("\n"))
            if m:
                print(m.group(1)); break
except OSError:
    pass
' "$1" "$2" 2>/dev/null
}

check_eq() { # check_eq <期望> <标签> <实际>
    if [ -n "${3:-}" ] && [ "$1" = "$3" ]; then ok "$2 = $3"; else bad "$2 期望 '$1'，实际 '${3:-<空>}'"; fi
}

# ── 权威值 ──────────────────────────────────────────────────────────
APP_ID="$(grep1 CMakeLists.txt 'set\(TER_MUSIC_APP_ID "([^"]+)"\)')"
if [ -z "$APP_ID" ]; then
    printf '错误：无法从 CMakeLists.txt 取到 TER_MUSIC_APP_ID\n' >&2
    exit 2
fi
BIN_NAME="$(grep1 CMakeLists.txt 'project\(([A-Za-z0-9_.-]+) LANGUAGES C VERSION')"
INC_ROOT="include/$APP_ID"

printf '=== 检查 1：包名 / 应用 ID 全链一致（权威值 %s）===\n' "$APP_ID"
check_eq "$APP_ID" "debian/control Source"  "$(grep1 debian/control '^Source:\s*(.+)$')"
check_eq "$APP_ID" "debian/control Package" "$(grep1 debian/control '^Package:\s*(.+)$')"
check_eq "$APP_ID" "PKGBUILD pkgname"       "$(grep1 PKGBUILD '^pkgname=(.+)$')"
check_eq "$APP_ID" ".SRCINFO pkgbase"       "$(grep1 .SRCINFO '^pkgbase\s*=\s*(.+)$')"
SRCINFO_PKGNAME="$(python3 -c '
import re
try:
    s = open(".SRCINFO", encoding="utf-8", errors="replace").read()
    m = re.findall(r"^pkgname\s*=\s*(.+)$", s, re.M)
    print(m[-1].strip() if m else "")
except OSError:
    pass' 2>/dev/null)"
check_eq "$APP_ID" ".SRCINFO pkgname" "$SRCINFO_PKGNAME"
check_eq "$APP_ID" "rpm spec Name" "$(grep1 rpm/cn.org.huansoft.ter-music.spec '^Name:\s*(.+)$')"
check_eq "$APP_ID" "linglong.yaml.in package.id" "$(grep1 linglong/linglong.yaml.in '^\s*id:\s*(.+)$')"
for f in build-deb build-rpm build-aur; do
    check_eq "$APP_ID" "scripts/build/$f.sh PACKAGE_NAME" \
        "$(grep1 "scripts/build/$f.sh" '^PACKAGE_NAME="([^"]+)"')"
done
check_eq "$APP_ID" "scripts/build/build-linyaps.sh APP_ID" \
    "$(grep1 scripts/build/build-linyaps.sh '^APP_ID="([^"]+)"')"
check_eq "$APP_ID" "scripts/build/build-appimage.sh APP_ID" \
    "$(grep1 scripts/build/build-appimage.sh '^APP_ID="([^"]+)"')"
check_eq "ter-music" "可执行文件名（project 第一参数）" "$BIN_NAME"
check_eq "ter-music" "build-appimage.sh PROJECT_NAME" \
    "$(grep1 scripts/build/build-appimage.sh '^PROJECT_NAME="([^"]+)"')"
check_eq "ter-music" "build-linyaps.sh PROJECT_NAME" \
    "$(grep1 scripts/build/build-linyaps.sh '^PROJECT_NAME="([^"]+)"')"
[ -f "data/linyaps/$APP_ID.service" ] \
    && ok "systemd 用户单元 = data/linyaps/$APP_ID.service" \
    || bad "缺少 data/linyaps/$APP_ID.service"

printf '=== 检查 2：开发默认版本同值 ===\n'
VER_RAW="$(grep1 "$INC_ROOT/types.h" '#define APP_VERSION\s+"v?([^"]+)"')"
if [ -z "$VER_RAW" ]; then
    bad "取不到 APP_VERSION（$INC_ROOT/types.h）"
else
    ok "APP_VERSION = $VER_RAW（开发默认）"
    check_eq "$VER_RAW" "CMakeLists project VERSION" "$(grep1 CMakeLists.txt 'LANGUAGES C VERSION ([0-9][^ )]*)')"
    check_eq "$VER_RAW" "PKGBUILD pkgver" "$(grep1 PKGBUILD '^pkgver=(.+)$')"
    check_eq "$VER_RAW" "debian/changelog 顶部条目" \
        "$(grep1 debian/changelog '^[a-z0-9+.-]+\s*\(([0-9][^(]*)-[0-9a-zA-Z.]+\)\s')"
    check_eq "$VER_RAW" "rpm spec 默认 _version" \
        "$(grep1 rpm/cn.org.huansoft.ter-music.spec '%\{!\?_version:%global _version ([0-9.]+)\}')"
    if printf '%s' "$VER_RAW" | grep -qE '^[0-9]{2}\.[1-4]\.[0-9]+\.[0-9]+$'; then
        ok "版本格式符合滚动方案 <yy>.<q>.x.y（$VER_RAW）"
    else
        bad "版本 '$VER_RAW' 不符合 <yy>.<q>.x.y"
    fi
fi

printf '=== 检查 3：版本只由参数注入 ===\n'
grep -q 'set(TER_MUSIC_VERSION' CMakeLists.txt \
    && ok "CMakeLists 定义 TER_MUSIC_VERSION" \
    || bad "CMakeLists 缺少 TER_MUSIC_VERSION"
grep -q '#ifndef APP_VERSION' "$INC_ROOT/types.h" \
    && ok "types.h 允许 APP_VERSION 被编译定义覆盖" \
    || bad "types.h 没有 #ifndef APP_VERSION"
for f in build-deb build-rpm build-appimage build-linyaps build-aur; do
    if grep -q 'tm_normalize_version_or_die' "scripts/build/$f.sh"; then
        ok "scripts/build/$f.sh 强制 -v"
    else
        bad "scripts/build/$f.sh 没有调用 tm_normalize_version_or_die"
    fi
done
if grep -RnE 'git describe|git tag|DEFAULT_VERSION' scripts/build >/dev/null 2>&1; then
    bad "scripts/build 仍依赖 git tag/DEFAULT_VERSION"
else
    ok "scripts/build 不依赖 git tag/DEFAULT_VERSION"
fi
for f in debian/rules rpm/cn.org.huansoft.ter-music.spec linglong/linglong.yaml.in \
         scripts/build/build-appimage.sh PKGBUILD; do
    if grep -q -- '-DTER_MUSIC_VERSION' "$f"; then
        ok "$f 注入 -DTER_MUSIC_VERSION"
    else
        bad "$f 没有注入 -DTER_MUSIC_VERSION"
    fi
done
grep -q '@VERSION@' linglong/linglong.yaml.in && grep -q '@ARCH@' linglong/linglong.yaml.in \
    && ok "linglong.yaml.in 保留版本/架构占位符" \
    || bad "linglong.yaml.in 缺少 @VERSION@/@ARCH@ 占位符"

printf '=== 检查 4：libxml2 运行期兼容层 ===\n'
if grep -q '^[[:space:]]*\${LIBXML2_LIBRARIES}' CMakeLists.txt; then
    bad "CMakeLists 仍链接 \${LIBXML2_LIBRARIES}"
else
    ok "CMakeLists 不链接 libxml2（运行期 dlopen）"
fi
grep -q 'util/xml_compat.c' CMakeLists.txt \
    && ok "CMakeLists 编译 xml_compat.c" \
    || bad "CMakeLists 缺少 xml_compat.c"
for soname in 'libxml2.so.2' 'libxml2.so.16'; do
    grep -q "$soname" src/cn.org.huansoft.ter-music/util/xml_compat.c \
        && ok "xml_compat.c 候选 $soname" \
        || bad "xml_compat.c 缺少 $soname"
done
grep -q 'libxml2 | libxml2-16' debian/control \
    && ok "debian/control 使用 libxml2 | libxml2-16" \
    || bad "debian/control 缺少 libxml2 备选依赖"
grep -qE '^Requires:\s*libxml2' rpm/cn.org.huansoft.ter-music.spec \
    && ok "rpm spec 显式 Requires: libxml2" \
    || bad "rpm spec 缺少显式 libxml2 依赖"

printf '=== 检查 5：源码/调试包默认关闭、选项存在 ===\n'
for f in build-deb build-rpm; do
    if grep -q -- '--with-source' "scripts/build/$f.sh" && \
       grep -q -- '--with-debuginfo' "scripts/build/$f.sh"; then
        ok "scripts/build/$f.sh 提供 --with-source / --with-debuginfo"
    else
        bad "scripts/build/$f.sh 缺少可选源码/调试包选项"
    fi
done
grep -q 'noautodbgsym' scripts/build/build-deb.sh \
    && ok "DEB 默认抑制自动 dbgsym" \
    || bad "DEB 默认没有抑制自动 dbgsym"
grep -q 'debug_package %{nil}' scripts/build/build-rpm.sh \
    && ok "RPM 默认抑制 debuginfo" \
    || bad "RPM 默认没有抑制 debuginfo"

printf '=== 检查 6：旧命名空间在打包链已闭合 ===\n'
if [ -d "src/org.yxzl.ter-music" ] || [ -d "include/org.yxzl.ter-music" ]; then
    bad "旧目录树 src|include/org.yxzl.ter-music 仍存在"
else
    ok "旧目录树已不存在"
fi
STALE_PKG="$(git ls-files -z -- data CMakeLists.txt PKGBUILD debian .SRCINFO scripts/build rpm linglong | xargs -0 grep -lI 'org\.yxzl\.ter-music' 2>/dev/null)"
[ -z "$STALE_PKG" ] && ok "打包/安装链无 org.yxzl.ter-music 残留" || bad "打包链残留旧应用 ID：$STALE_PKG"
STALE_INC="$(git ls-files -z -- src include | xargs -0 grep -lI '#include "org\.yxzl' 2>/dev/null)"
[ -z "$STALE_INC" ] && ok "无 #include \"org.yxzl…\" 残留" || bad "残留旧 include：$STALE_INC"
STALE_LIT="$(git ls-files -z -- src include data | xargs -0 grep -lI '"org\.yxzl\.ter_music' 2>/dev/null)"
[ -z "$STALE_LIT" ] && ok "无 C 字符串字面量 \"org.yxzl.ter_music…\" 残留" || bad "残留旧接口字符串：$STALE_LIT"
STALE_RE="$(git ls-files -z -- scripts | xargs -0 grep -lIE 'org\\\.yxzl|"org\.yxzl\.ter_music' 2>/dev/null | grep -v '^scripts/test/check-package-identity\.sh$' || true)"
[ -z "$STALE_RE" ] && ok "scripts/ 内无旧前缀正则/匹配串" || bad "脚本仍按旧前缀匹配：$STALE_RE"

printf '=== 检查 7：AUR 不依赖 tag ===\n'
grep -q '#commit=\$_commit' PKGBUILD \
    && ok "PKGBUILD 使用 #commit=\$_commit" \
    || bad "PKGBUILD 没有使用固定 commit"
COMMIT="$(grep1 PKGBUILD '^_commit=([0-9a-f]{40})$')"
if [ -n "$COMMIT" ]; then
    ok "_commit = ${COMMIT:0:12}…"
else
    bad "PKGBUILD 的 _commit 不是 40 位十六进制"
fi
grep -q '#commit=' .SRCINFO \
    && ok ".SRCINFO 与固定 commit 来源一致" \
    || bad ".SRCINFO 仍使用 tag 来源"

printf '=== 检查 8：统一 /opt/apps 安装布局（实体在 opt，/usr 只放链接）===\n'
PREFIX="/opt/apps/$APP_ID/files"
for f in debian/rules rpm/cn.org.huansoft.ter-music.spec PKGBUILD \
         scripts/build/build-appimage.sh linglong/linglong.yaml.in; do
    if grep -q -- '-DCMAKE_INSTALL_PREFIX=/usr' "$f"; then
        bad "$f 仍把 /usr 当作 CMAKE_INSTALL_PREFIX"
    else
        ok "$f 不再使用 /usr 安装前缀"
    fi
done
grep -q '^APP_ID := cn.org.huansoft.ter-music$' debian/rules \
    && ok "debian/rules 定义 APP_ID" || bad "debian/rules 缺少 APP_ID"
grep -q '^PREFIX := /opt/apps/$(APP_ID)/files$' debian/rules \
    && ok "debian/rules 前缀 = $PREFIX" || bad "debian/rules 前缀不是 $PREFIX"
grep -q -- '-DCMAKE_INSTALL_PREFIX=\$(PREFIX)' debian/rules \
    && ok "debian/rules 用 \$(PREFIX) 配置 CMake" || bad "debian/rules 未注入 \$(PREFIX)"
if grep -q '%global tm_prefix /opt/apps/%{name}/files' rpm/cn.org.huansoft.ter-music.spec; then
    ok "rpm spec 前缀 = /opt/apps/%{name}/files"
else
    bad "rpm spec 前缀不是 /opt 布局"
fi
grep -q '^_prefix=/opt/apps/\$_appid/files$' PKGBUILD \
    && ok "PKGBUILD 前缀 = /opt/apps/\$_appid/files" || bad "PKGBUILD 前缀不是 /opt 布局"
grep -q 'CMAKE_INSTALL_PREFIX=/opt/apps/\${APP_ID}/files' scripts/build/build-appimage.sh \
    && ok "build-appimage.sh 前缀 = /opt/apps/\${APP_ID}/files" || bad "build-appimage.sh 前缀不是 /opt 布局"
grep -q -- '-DCMAKE_INSTALL_PREFIX=\${PREFIX}' linglong/linglong.yaml.in \
    && ok "linglong.yaml.in 前缀 = \${PREFIX}（files/ 布局）" || bad "linglong.yaml.in 前缀不是 files/ 布局"
grep -q "/opt/apps/$APP_ID/files/bin/ter-music" linglong/linglong.yaml.in \
    && ok "linglong command 指向 /opt/apps/<appid>/files/bin" || bad "linglong command 未指向 opt 实体"

# /usr 只放指向 opt 实体的链接（deb/rpm/aur/appimage 四条链路）
for dst in usr/bin/ter-music usr/share/applications/ter-music.desktop \
           usr/share/icons/hicolor/32x32/apps/ter-music.png \
           usr/share/icons/hicolor/48x48/apps/ter-music.png \
           usr/share/icons/hicolor/128x128/apps/ter-music.png \
           usr/share/icons/hicolor/scalable/apps/ter-music.svg; do
    if grep -q "/opt/apps/$APP_ID/files/[^ ]* $dst\$" debian/links 2>/dev/null; then
        ok "debian/links -> $dst"
    else
        bad "debian/links 缺少 $dst"
    fi
done
if grep -q '^ln -sfn ../../opt/apps/%{name}/files/bin/%{binname}' \
        rpm/cn.org.huansoft.ter-music.spec && \
   grep -q '^%{tm_prefix}$' rpm/cn.org.huansoft.ter-music.spec && \
   grep -q '^/usr/bin/%{binname}$' rpm/cn.org.huansoft.ter-music.spec; then
    ok "rpm spec %install 建链接、%files 同时收录 opt 实体与 /usr 链接"
else
    bad "rpm spec 缺少 opt 实体 + /usr 链接布局"
fi
if grep -q 'ln -sfn "../../../opt/apps/\$_appid/files/bin/ter-music"' PKGBUILD && \
   grep -q 'DESTDIR="$pkgdir" cmake --install build' PKGBUILD; then
    ok "PKGBUILD 安装 opt 实体并在 /usr 建相对链接"
else
    bad "PKGBUILD 缺少 opt 实体 + /usr 链接布局"
fi
if grep -q 'ln -sfn "../../../opt/apps/\${APP_ID}/files/bin/\${PROJECT_NAME}"' \
        scripts/build/build-appimage.sh && \
   grep -q 'AppDir/usr 只放指向 opt 实体的相对符号链接' scripts/build/build-appimage.sh; then
    ok "build-appimage.sh 的 AppDir/usr 只放相对链接"
else
    bad "build-appimage.sh 的 AppDir/usr 布局不符合要求"
fi

printf '=== 检查 9：9662 模型构建期获取/校验/注入 ===\n'
MODEL_SIZE_COMMON="$(grep1 scripts/build/common.sh '^TM_MODEL_SIZE=([0-9]+)$')"
MODEL_SIZE_CMAKE="$(grep1 CMakeLists.txt 'set\(TER_MUSIC_BUNDLED_MODEL_SIZE ([0-9]+)\)')"
MODEL_SHA_COMMON="$(grep1 scripts/build/common.sh '^TM_MODEL_SHA256="([0-9a-f]{64})"$')"
MODEL_SHA_CMAKE="$(grep1 CMakeLists.txt '^\s*"([0-9a-f]{64})"\)')"
check_eq "29704436" "common.sh TM_MODEL_SIZE" "$MODEL_SIZE_COMMON"
check_eq "$MODEL_SIZE_COMMON" "CMakeLists 模型大小与 common.sh 一致" "$MODEL_SIZE_CMAKE"
check_eq "e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc" \
    "common.sh TM_MODEL_SHA256" "$MODEL_SHA_COMMON"
check_eq "$MODEL_SHA_COMMON" "CMakeLists 模型 SHA256 与 common.sh 一致" "$MODEL_SHA_CMAKE"
grep -q "$MODEL_SHA_COMMON" resources/models/NOTICE.md \
    && ok "NOTICE.md 记录模型 SHA256" || bad "NOTICE.md 缺少模型 SHA256"
grep -q '29,704,436' resources/models/NOTICE.md \
    && ok "NOTICE.md 记录模型字节数" || bad "NOTICE.md 缺少模型字节数"
grep -q 'UVR_MDXNET_3_9662.onnx' resources/models/NOTICE.md \
    && ok "NOTICE.md 记录上游模型文件名" || bad "NOTICE.md 缺少上游文件名"
grep -q 'TRvlvr/model_repo' scripts/build/common.sh \
    && ok "common.sh 记录模型官方 URL" || bad "common.sh 缺少模型官方 URL"
for f in build-deb build-rpm build-linyaps build-appimage build-aur; do
    if grep -q 'tm_ensure_model' "scripts/build/$f.sh"; then
        ok "scripts/build/$f.sh 调用 tm_ensure_model"
    else
        bad "scripts/build/$f.sh 没有调用 tm_ensure_model"
    fi
done
for f in debian/rules rpm/cn.org.huansoft.ter-music.spec PKGBUILD \
         linglong/linglong.yaml.in scripts/build/build-appimage.sh; do
    if grep -q -- '-DTER_MUSIC_BUNDLED_MODEL' "$f"; then
        ok "$f 注入 -DTER_MUSIC_BUNDLED_MODEL"
    else
        bad "$f 没有注入 -DTER_MUSIC_BUNDLED_MODEL"
    fi
done
grep -q 'export TER_MUSIC_BUNDLED_MODEL=' scripts/build/build-deb.sh \
    && ok "build-deb.sh 通过环境变量把模型绝对路径传给 debian/rules" \
    || bad "build-deb.sh 没有导出 TER_MUSIC_BUNDLED_MODEL"
grep -q '\.tm-bundled-model' rpm/cn.org.huansoft.ter-music.spec \
    && ok "rpm spec 从 staging 的 .tm-bundled-model 注入模型" \
    || bad "rpm spec 缺少 staged 模型路径"
grep -q 'bundled-model' linglong/linglong.yaml.in \
    && ok "linglong.yaml.in 从源码树 bundled-model 注入模型" \
    || bad "linglong.yaml.in 缺少 bundled-model 注入"
for f in scripts/docker/Dockerfile.debian10 scripts/docker/Dockerfile.rpm scripts/docker/Dockerfile.aur; do
    if grep -q 'aria2' "$f"; then
        ok "$f 安装 aria2（构建期下载模型）"
    else
        bad "$f 没有安装 aria2"
    fi
done
grep -qx '\*\.onnx' .gitignore \
    && ok ".gitignore 忽略 *.onnx" || bad ".gitignore 没有忽略 *.onnx"
TRACKED_ONNX="$(git ls-files -- '*.onnx' 2>/dev/null)"
[ -z "$TRACKED_ONNX" ] && ok "仓库没有受版本控制的 .onnx" \
    || bad "仓库仍跟踪模型二进制：$TRACKED_ONNX"

printf '\n--- 结果 ---\n'
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
if [ "$FAIL" -gt 0 ] && [ "$REPORT_ONLY" -eq 0 ]; then
    exit 1
fi
exit 0
