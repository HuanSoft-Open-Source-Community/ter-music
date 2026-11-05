#!/usr/bin/env bash
#
# 包名 / 版本 / 命名空间 一致性检查（身份矩阵）
#
# 为什么需要它：本项目的"身份"散落在十几个互不相干的声明式文件里（CMake、
# debian/*、PKGBUILD、.SRCINFO、scripts/build/*、types.h、data/lang/*），没有任何
# 一处是编译器会替我们校验的。26.4.1.0 的组织迁移（org.yxzl.* → cn.org.huansoft.*）
# 与滚动版本号（<yy>.<q>.x.y）落地时真实踩到过两类坑：
#   · scripts/test/doc_api_check.py 的小节正则仍是 org\.yxzl\.，文档明明齐全却被
#     判成"缺全部方法"（字面替换抓不到正则里的转义写法）；
#   · RPM 的 %{name} 同时被当成可执行文件名用，差点把二进制装成
#     /usr/bin/cn.org.huansoft.ter-music。
# 这两类都不影响编译，只在打包机或用户机器上炸，所以固化成回归门。
#
# 用法：
#   scripts/test/check-package-identity.sh            # 严格：任一项不符即退出 1
#   scripts/test/check-package-identity.sh --report   # 只报告，恒退出 0
#
# 设计约束：纯文本解析，不依赖 dbus / python-gi / dpkg-dev / makepkg，任何 Linux
# 机器（含 CI 编译机）都能跑。只扫 git 跟踪的文件——本地未跟踪的规划草稿
# （.trae/ 等）不是交付物，不参与判定。

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

# 取某文件第一个匹配行的捕获组 1。
# 刻意不用 "sed -E s/.*<含锚点正则>.*/\1/"：调用方写的 ^ $ 会被前面补的 .* 吃掉，
# 永远匹配不上（本脚本第一版就栽在这儿）。
# 正则按 Python 语法：POSIX 字符类 [[:space:]] 在 Python 里不成立，一律用 \s。
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
# 安装后的目录布局由 CMake 的 TER_MUSIC_APP_ID 决定，拿它当包名/应用 ID 的权威源；
# 可执行文件名由 project() 的第一个参数决定，必须与包名相互独立。
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
# .SRCINFO 由 makepkg --printsrcinfo 生成：pkgbase 与包名都要等于权威值
check_eq "$APP_ID" ".SRCINFO pkgbase" "$(grep1 .SRCINFO '^pkgbase\s*=\s*(.+)$')"
SRCINFO_PKGNAME="$(python3 -c '
import re
try:
    s = open(".SRCINFO", encoding="utf-8", errors="replace").read()
    m = re.findall(r"^pkgname\s*=\s*(.+)$", s, re.M)
    print(m[-1].strip() if m else "")
except OSError:
    pass' 2>/dev/null)"
check_eq "$APP_ID" ".SRCINFO pkgname" "$SRCINFO_PKGNAME"
for f in build-deb build-rpm; do
    check_eq "$APP_ID" "scripts/build/$f.sh PACKAGE_NAME" \
        "$(grep1 "scripts/build/$f.sh" '^PACKAGE_NAME="([^"]+)"')"
done
check_eq "$APP_ID" "scripts/build/build-linyaps.sh APP_ID" \
    "$(grep1 scripts/build/build-linyaps.sh '^APP_ID="([^"]+)"')"
# AppImage / Portable 靠 glob 找 RPM 产物，名字写错就找不到包
for f in build-appimage build-portable; do
    check_eq "$APP_ID" "scripts/build/$f.sh RPM_PACKAGE_NAME" \
        "$(grep1 "scripts/build/$f.sh" '^RPM_PACKAGE_NAME="([^"]+)"')"
done
# systemd 用户单元随包名改名（单元名即文件名，改了就要用户重新 enable）
[ -f "data/linyaps/$APP_ID.service" ] \
    && ok "systemd 用户单元 = data/linyaps/$APP_ID.service" \
    || bad "缺少 data/linyaps/$APP_ID.service"

printf '=== 检查 2：版本号四处同值 ===\n'
# 事实源是 types.h 的 APP_VERSION（v 前缀只存在于这里），其余三处是镜像。
VER_RAW="$(grep1 "$INC_ROOT/types.h" '#define APP_VERSION\s+"v?([^"]+)"')"
if [ -z "$VER_RAW" ]; then
    bad "取不到 APP_VERSION（$INC_ROOT/types.h）"
else
    ok "APP_VERSION = $VER_RAW（唯一事实源）"
    check_eq "$VER_RAW" "CMakeLists project VERSION" "$(grep1 CMakeLists.txt 'LANGUAGES C VERSION ([0-9][^ )]*)')"
    check_eq "$VER_RAW" "PKGBUILD pkgver" "$(grep1 PKGBUILD '^pkgver=(.+)$')"
    # changelog 顶部条目形如 name (X.Y.Z-N) suite; urgency=…，去掉 Debian 修订号再比
    check_eq "$VER_RAW" "debian/changelog 顶部条目" \
        "$(grep1 debian/changelog '^[a-z0-9+.-]+\s*\(([0-9][^(]*)-[0-9a-zA-Z.]+\)\s')"
    if printf '%s' "$VER_RAW" | grep -qE '^[0-9]{2}\.[1-4]\.[0-9]+\.[0-9]+$'; then
        ok "版本格式符合滚动方案 <yy>.<q>.x.y（$VER_RAW）"
    else
        bad "版本 '$VER_RAW' 不符合 <yy>.<q>.x.y（年前两位 . 季度1-4 . 主线序 . 修订位）"
    fi
    # PKGBUILD 必须继续用 #tag=v$pkgver：换成裸数字就断了 AUR 可复现性
    grep -q 'source=.*#tag=v\$pkgver' PKGBUILD \
        && ok "PKGBUILD 以 #tag=v\$pkgver 前向引用（随 pkgver 联动）" \
        || bad "PKGBUILD 的 source 不再用 #tag=v\$pkgver，AUR 构建失去可复现保证"
fi

printf '=== 检查 3：API 版本与测试断言一致 ===\n'
RPC_H="$INC_ROOT/media/rpc.h"
API_VER="$(grep1 "$RPC_H" '#define TER_MUSIC_API_VERSION\s+([0-9]+)')"
API_MIN="$(grep1 "$RPC_H" '#define TER_MUSIC_API_MIN_VERSION\s+([0-9]+)')"
if [ -z "$API_VER" ] || [ -z "$API_MIN" ]; then
    bad "$RPC_H 缺少 TER_MUSIC_API_VERSION / TER_MUSIC_API_MIN_VERSION"
else
    ok "TER_MUSIC_API_VERSION=$API_VER  MIN=$API_MIN"
    [ "$API_MIN" -le "$API_VER" ] \
        && ok "MIN_VERSION($API_MIN) ≤ API_VERSION($API_VER)" \
        || bad "MIN_VERSION($API_MIN) 大于 API_VERSION($API_VER)，没有前端能握手成功"
    # 协议检查脚本必须断言同一个数：宏与测试历史上就落后过一版
    if grep -qF "[ \"\$api_version\" = \"$API_VER\" ]" scripts/test/dbus-rpc-check.sh; then
        ok "dbus-rpc-check.sh 期望 core.api_version = $API_VER"
    else
        bad "dbus-rpc-check.sh 仍在断言别的 api_version（rpc.h 是 $API_VER）"
    fi
    if grep -qF "[ \"\$version\" = \"$API_VER\" ]" scripts/test/dbus-rpc-check.sh; then
        ok "dbus-rpc-check.sh 期望 Attach api_version = $API_VER"
    else
        bad "dbus-rpc-check.sh 的 Attach 断言与 rpc.h 的 $API_VER 不一致"
    fi
fi

printf '=== 检查 4：旧命名空间在线上协议层已闭合 ===\n'
# 只判"会炸"的几类；文档、日志、变更历史里提到旧名是必要的迁移说明，不算残留。
if [ -d "src/org.yxzl.ter-music" ] || [ -d "include/org.yxzl.ter-music" ]; then
    bad "旧目录树 src|include/org.yxzl.ter-music 仍存在"
else
    ok "旧目录树已不存在（src/ 与 include/ 各一份）"
fi
STALE_INC="$(git ls-files -z -- src include | xargs -0 grep -lI '#include "org\.yxzl' 2>/dev/null)"
[ -z "$STALE_INC" ] && ok "无 #include \"org.yxzl…\" 残留" || bad "残留旧 include：$STALE_INC"
STALE_LIT="$(git ls-files -z -- src include data | xargs -0 grep -lI '"org\.yxzl\.ter_music' 2>/dev/null)"
[ -z "$STALE_LIT" ] && ok "无 C 字符串字面量 \"org.yxzl.ter_music…\" 残留" || bad "残留旧接口字符串：$STALE_LIT"
STALE_PKG="$(git ls-files -z -- data CMakeLists.txt PKGBUILD debian .SRCINFO scripts/build | xargs -0 grep -lI 'org\.yxzl\.ter-music' 2>/dev/null)"
[ -z "$STALE_PKG" ] && ok "打包/安装链无 org.yxzl.ter-music 残留" || bad "打包链残留旧应用 ID：$STALE_PKG"
# 正则里的转义写法是真正的漏网点（字面替换看不见反斜杠），单独点名测试与构建脚本
STALE_RE="$(git ls-files -z -- scripts | xargs -0 grep -lIE 'org\\\.yxzl|"org\.yxzl\.ter_music' 2>/dev/null)"
[ -z "$STALE_RE" ] && ok "scripts/ 内无仍在使用旧前缀的正则/匹配串" || bad "脚本仍按旧前缀匹配：$STALE_RE"
# 仍提到旧名的文件（迁移说明，允许）只报不计分，方便人工核对范围
PROSE="$(git ls-files -z | xargs -0 grep -lI 'org\.yxzl' 2>/dev/null | tr '\n' ' ')"
info "仍提到 org.yxzl 的文件（历史/迁移说明，允许）：${PROSE:-无}"

printf '=== 检查 5：不该跟着改的身份没被改掉 ===\n'
# 这些名字是对外契约或用户可见路径，迁移时刻意保留；有人"顺手全改"就会立刻打断
# 用户的媒体键、脚本与既有配置。
check_eq "ter-music" "可执行文件名（project 第一参数）" "$BIN_NAME"
grep -q 'add_executable(${PROJECT_NAME}' CMakeLists.txt \
    && ok "add_executable 仍绑定 \${PROJECT_NAME}（不随包名改）" \
    || bad "add_executable 的目标名写法变了，请确认二进制仍叫 ter-music"
for f in media/rpc.h cli/cli.h; do
    if grep -q 'org\.mpris\.MediaPlayer2\.ter_music' "$INC_ROOT/$f"; then
        ok "$f 仍引用标准 MPRIS 总线名"
    else
        bad "$f 丢了 MPRIS 总线名（桌面媒体键依赖它）"
    fi
done
[ -f "data/linyaps/org.mpris.MediaPlayer2.ter_music.service" ] \
    && ok "D-Bus 激活文件仍用标准 MPRIS 名" || bad "data/linyaps/org.mpris.MediaPlayer2.ter_music.service 不见了"
if grep -qF "install(FILES data/linyaps/$APP_ID.service" CMakeLists.txt; then
    ok "CMake 安装的是改名后的单元文件"
else
    bad "CMake 没有在装 data/linyaps/$APP_ID.service"
fi

printf '=== 检查 6：i18n 提示文字里的 ID 与安装名一致 ===\n'
# data/lang 的沙箱指导文字会把应用 ID / 单元名直接念给用户，写错就是误导。
for L in zh_CN en_US zh-PQ; do
    f="data/lang/$L.xml"
    [ -f "$f" ] || { bad "缺少 $f"; continue; }
    if grep -q "$APP_ID" "$f"; then
        ok "$f 的常驻服务指导引用 $APP_ID"
    else
        bad "$f 未引用当前应用 ID（提示文字与安装名脱节）"
    fi
done

printf '\n--- 结果 ---\n'
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
if [ "$FAIL" -gt 0 ] && [ "$REPORT_ONLY" -eq 0 ]; then
    exit 1
fi
exit 0
