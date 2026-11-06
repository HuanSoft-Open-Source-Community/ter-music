#!/usr/bin/env bash
#
# 构建产物审计：只读检查 build/ 下的 DEB / RPM / AppImage / Linyaps / AUR 产物。
#
# 每条检查输出 [PASS] / [FAIL] / [SKIP]；缺少产物时记 SKIP，不视为失败。
# 需要 docker 的运行期检查可用 --no-runtime 跳过。
#
# 用法: audit-build-artifacts.sh [--root DIR] [--no-runtime] [deb] [rpm] [appimage] [linyaps] [aur]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ROOT="$DEFAULT_ROOT"
NO_RUNTIME=0
TARGETS=()
GLIBC_CEILING="2.28"
ARTIFACT_VERSIONS=()

show_help() {
    cat <<EOF
用法: $(basename "$0") [--root DIR] [--no-runtime] [deb] [rpm] [appimage] [linyaps] [aur]

选项:
  --root DIR      仓库根目录（默认：${DEFAULT_ROOT}）
  --no-runtime    跳过需要 docker 的运行期检查
  -h, --help      显示帮助

目标:
  deb rpm appimage linyaps aur   默认审计全部；未构建的目标记 [SKIP]
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --root) ROOT="${2:-}"; shift 2 ;;
        --root=*) ROOT="${1#*=}"; shift ;;
        --no-runtime) NO_RUNTIME=1; shift ;;
        -h|--help) show_help; exit 0 ;;
        deb|rpm|appimage|linyaps|aur) TARGETS+=("$1"); shift ;;
        *) echo "错误：未知参数 '$1'" >&2; show_help >&2; exit 2 ;;
    esac
done

if [ -z "$ROOT" ] || [ ! -d "$ROOT" ]; then
    echo "错误：仓库根目录不存在：${ROOT:-<空>}" >&2
    exit 2
fi
ROOT="$(cd "$ROOT" && pwd)"

if [ "${#TARGETS[@]}" -eq 0 ]; then
    TARGETS=(deb rpm appimage linyaps aur)
fi

PASS=0
FAIL=0
SKIP=0

section() { printf '\n=== %s ===\n' "$*"; }
pass()    { PASS=$((PASS + 1)); printf '[PASS] %s\n' "$*"; }
fail()    { FAIL=$((FAIL + 1)); printf '[FAIL] %s\n' "$*"; }
skip()    { SKIP=$((SKIP + 1)); printf '[SKIP] %s\n' "$*"; }
info()    { printf '[INFO] %s\n' "$*"; }

have() { command -v "$1" >/dev/null 2>&1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ter-music-audit.XXXXXX")" || exit 2
cleanup() { rm -rf -- "$WORK"; }
trap cleanup EXIT INT TERM

docker_usable() { [ "$NO_RUNTIME" -eq 0 ] && have docker; }
docker_image_present() { have docker && docker image inspect "$1" >/dev/null 2>&1; }

# 去掉 v 前缀，便于与包元数据比较。
norm_ver() { printf '%s\n' "${1:-}" | sed -e 's/^[vV]//' -e 's/[[:space:]]*$//'; }

version_le() { # $1 <= $2 ?
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n1)" = "$2" ]
}

max_glibc() { # 读取 ELF 的最高 GLIBC_* 版本（仅数字部分）
    LC_ALL=C readelf --version-info "$1" 2>/dev/null \
        | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' \
        | sed 's/^GLIBC_//' | sort -uV | tail -n1
}

needed_libs() { # 仅提取 DT_NEEDED 的库名
    LC_ALL=C readelf -d "$1" 2>/dev/null \
        | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'
}

needed_forbidden() { # $1=ELF 输出命中 libav*/libxml2 的项
    needed_libs "$1" | grep -E 'libav|libxml2' || true
}

record_version() { # $1=来源 $2=版本
    local v
    v="$(norm_ver "$2")"
    [ -n "$v" ] || return 0
    info "版本（$1）：$v"
    ARTIFACT_VERSIONS+=("$1=$v")
}

# 在指定发行版容器里真实安装（含依赖解析）并运行，验证可用性。
deb_runtime_check() { # $1=deb 路径 $2=镜像 $3=期望版本
    local deb="$1" img="$2" want="$3"
    if ! docker_image_present "$img"; then
        skip "DEB 运行期（${img}）：本地缺少镜像"
        return 0
    fi
    local out rc
    out="$(timeout 600 docker run --rm -v "${deb}:/tmp/p.deb:ro" "$img" bash -lc '
        # Debian 10/11 已从主镜像移入 archive；buster 的安全仓库 suite 是 buster/updates。
        if grep -qE "VERSION_CODENAME=(buster|bullseye)" /etc/os-release 2>/dev/null; then
            codename="$(sed -n "s/^VERSION_CODENAME=//p" /etc/os-release)"
            security="${codename}-security"
            [ "$codename" = buster ] && security="buster/updates"
            printf "%s\n" \
              "deb http://mirrors.aliyun.com/debian-archive/debian ${codename} main contrib non-free" \
              "deb http://mirrors.aliyun.com/debian-archive/debian-security ${security} main" \
              "deb http://mirrors.aliyun.com/debian-archive/debian ${codename}-updates main" \
              > /etc/apt/sources.list
            rm -f /etc/apt/sources.list.d/*.sources 2>/dev/null || true
        fi
        if [ -f /etc/apt/sources.list.d/debian.sources ]; then
            sed -i -e "s|http://deb.debian.org/debian|http://mirrors.aliyun.com/debian|g" \
                   -e "s|http://security.debian.org/debian-security|http://mirrors.aliyun.com/debian-security|g" \
                   /etc/apt/sources.list.d/debian.sources
        fi
        if [ -f /etc/apt/sources.list ]; then
            sed -i -e "s|http://archive.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|g" \
                   -e "s|http://security.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|g" \
                   -e "s|http://deb.debian.org/debian|http://mirrors.aliyun.com/debian|g" \
                   /etc/apt/sources.list
        fi
        apt-get update -qq >/tmp/apt.log 2>&1 || { tail -n 3 /tmp/apt.log; exit 3; }
        DEBIAN_FRONTEND=noninteractive apt-get install -y -qq /tmp/p.deb >>/tmp/apt.log 2>&1 \
            || { tail -n 5 /tmp/apt.log; exit 4; }
        ter-music --version 2>&1
    ' 2>&1)"
    rc=$?
    if printf '%s' "$out" | grep -q "$want"; then
        pass "DEB 运行期（${img}）：安装后 ter-music --version -> $(printf '%s' "$out" | head -n1)"
    else
        fail "DEB 运行期（${img}）失败（rc=${rc}）：$(printf '%s' "$out" | tail -n1)"
    fi
}

# ── DEB ────────────────────────────────────────────────────────────────
audit_deb() {
    section "DEB"
    local bindir="$ROOT/build/deb/amd64"
    local srcdir="$ROOT/build/deb/source"
    local -a bins=() dbgs=() sources=()
    local f

    if [ -d "$bindir" ]; then
        while IFS= read -r -d '' f; do
            case "$(basename "$f")" in
                *dbgsym*) dbgs+=("$f") ;;
                *) bins+=("$f") ;;
            esac
        done < <(find "$bindir" -maxdepth 1 -type f -name '*.deb' -print0 | sort -z)
    fi
    if [ -d "$bindir" ]; then
        while IFS= read -r -d '' f; do dbgs+=("$f"); done \
            < <(find "$ROOT/build/deb" -maxdepth 2 -type f -name '*.ddeb' -print0 | sort -z)
    fi
    if [ -d "$srcdir" ]; then
        while IFS= read -r -d '' f; do sources+=("$f"); done \
            < <(find "$srcdir" -maxdepth 1 -type f -print0 | sort -z)
    fi

    if [ "${#bins[@]}" -eq 0 ]; then
        skip "DEB：未找到 build/deb/amd64/*.deb"
        [ "${#sources[@]}" -eq 0 ] || info "源码包目录存在但缺少二进制包，已忽略源码包检查"
        return 0
    fi
    have dpkg-deb || { skip "DEB：宿主机缺少 dpkg-deb，无法解包检查"; return 0; }

    local deb name pkg ver arch depends size extract bin needed glibc forbidden
    for deb in "${bins[@]}"; do
        name="$(basename "$deb")"
        info "契约：$name"
        size="$(stat -c '%s' "$deb" 2>/dev/null || echo 0)"
        if [ "$size" -gt 0 ]; then pass "DEB 文件非空（$size 字节）"
        else fail "DEB 文件为空：$name"; continue; fi

        pkg="$(dpkg-deb -f "$deb" Package 2>/dev/null || true)"
        ver="$(dpkg-deb -f "$deb" Version 2>/dev/null || true)"
        arch="$(dpkg-deb -f "$deb" Architecture 2>/dev/null || true)"
        depends="$(dpkg-deb -f "$deb" Depends 2>/dev/null || true)"
        info "Package=$pkg Version=$ver Architecture=$arch"
        case "$ver" in
            *-*) record_version "DEB $name" "${ver%-*}" ;;
            *)   record_version "DEB $name" "$ver" ;;
        esac

        if [ -n "$pkg" ]; then pass "Package 字段存在（$pkg）"; else fail "Package 字段为空"; fi
        if [ "$arch" = "amd64" ]; then pass "Architecture = amd64"; else fail "Architecture 期望 amd64，实际 '${arch:-<空>}'"; fi

        if printf '%s' "$depends" | grep -q 'libxml2'; then
            pass "Depends 含 libxml2"
        else
            fail "Depends 缺少 libxml2：${depends:-<空>}"
        fi
        if printf '%s' "$depends" \
            | grep -Eq 'libxml2[^,]*\|[^,]*libxml2-16|libxml2-16[^,]*\|[^,]*libxml2'; then
            pass "Depends 含 libxml2 | libxml2-16 备选"
        else
            fail "Depends 缺少 libxml2 | libxml2-16 备选"
        fi

        extract="$WORK/deb.$$.$RANDOM"
        rm -rf -- "$extract"
        if ! dpkg-deb -x "$deb" "$extract" >/dev/null 2>&1; then
            fail "dpkg-deb -x 解包失败：$name"
            continue
        fi
        bin="$extract/usr/bin/ter-music"
        if [ -f "$bin" ]; then
            pass "解包后存在 usr/bin/ter-music"
        else
            fail "解包后缺少 usr/bin/ter-music"
            continue
        fi

        forbidden="$(needed_forbidden "$bin")"
        if [ -z "$forbidden" ]; then
            pass "DT_NEEDED 无 libav*/libxml2"
        else
            fail "DT_NEEDED 含禁止依赖：$(printf '%s' "$forbidden" | tr '\n' ' ')"
        fi
        needed="$(needed_libs "$bin" | tr '\n' ' ')"
        info "DT_NEEDED：${needed:-<空>}"

        glibc="$(max_glibc "$bin")"
        if [ -z "$glibc" ]; then
            fail "无法从 readelf --version-info 提取 GLIBC 版本"
        elif version_le "$glibc" "$GLIBC_CEILING"; then
            pass "最高 GLIBC_${glibc} <= ${GLIBC_CEILING}"
        else
            fail "最高 GLIBC_${glibc} 超过基线 ${GLIBC_CEILING}"
        fi

        if ! docker_usable; then
            skip "DEB 运行期：--no-runtime 或 docker 不可用"
        else
            local img
            for img in ${TM_DEB_RUNTIME_IMAGES:-debian:10}; do
                deb_runtime_check "$deb" "$img" "$(norm_ver "${ver%-*}")"
            done
        fi
    done

    if [ "${#dbgs[@]}" -gt 0 ]; then
        for f in "${dbgs[@]}"; do
            name="$(basename "$f")"
            size="$(stat -c '%s' "$f" 2>/dev/null || echo 0)"
            if [ "$size" -gt 0 ]; then pass "调试包非空：$name（$size 字节）"
            else fail "调试包为空：$name"; fi
        done
    else
        info "无调试包（默认不生成）"
    fi
    if [ "${#sources[@]}" -gt 0 ]; then
        for f in "${sources[@]}"; do
            name="$(basename "$f")"
            size="$(stat -c '%s' "$f" 2>/dev/null || echo 0)"
            if [ "$size" -gt 0 ]; then pass "源码包非空：$name（$size 字节）"
            else fail "源码包为空：$name"; fi
        done
    else
        info "无源码包（默认不生成）"
    fi
}
# ── RPM ────────────────────────────────────────────────────────────────
rpm_backend() { # $1=EL 版本；输出 host / docker / 空
    # 优先用对应镜像：宿主机 rpm 只能查询/解包，无法验证依赖安装与运行。
    if docker_usable && docker_image_present "ter-music-rpm-el$1"; then
        printf 'docker\n'
    elif have rpm && have rpm2cpio && have cpio; then
        printf 'host\n'
    fi
}

rpm_query() { # $1=backend $2=EL $3=文件 $4=rpm 参数...
    local backend="$1" el="$2" file="$3"
    shift 3
    case "$backend" in
        host) rpm "$@" "$file" 2>/dev/null ;;
        docker) docker run --rm -v "${file}:/tmp/p.rpm:ro" "ter-music-rpm-el${el}" \
                    rpm "$@" /tmp/p.rpm 2>/dev/null ;;
    esac
}

rpm_extract() { # $1=backend $2=EL $3=文件 $4=目标目录
    local backend="$1" el="$2" file="$3" dest="$4"
    case "$backend" in
        host) ( cd "$dest" && rpm2cpio "$file" 2>/dev/null | cpio -idm --quiet 2>/dev/null ) ;;
        docker) docker run --rm --user "$(id -u):$(id -g)" \
                    -v "${file}:/tmp/p.rpm:ro" -v "${dest}:/tmp/x" \
                    "ter-music-rpm-el${el}" bash -lc \
                    'cd /tmp/x && rpm2cpio /tmp/p.rpm | cpio -idm --quiet' 2>/dev/null ;;
    esac
}

audit_rpm() {
    section "RPM"
    local basedir="$ROOT/build/rpm"
    local -a el_dirs=()
    local d

    if [ -d "$basedir" ]; then
        while IFS= read -r -d '' d; do el_dirs+=("$d"); done \
            < <(find "$basedir" -mindepth 2 -maxdepth 2 -type d -name 'x86_64' -print0 | sort -z)
    fi
    if [ "${#el_dirs[@]}" -eq 0 ]; then
        skip "RPM：未找到 build/rpm/el*/x86_64"
        return 0
    fi

    local name file main size el backend ver arch requires forbidden extract bin
    local -a rpms=()
    for d in "${el_dirs[@]}"; do
        el="$(basename "$(dirname "$d")")"
        el="${el#el}"
        rpms=()
        while IFS= read -r -d '' file; do
            case "$(basename "$file")" in
                *-debuginfo-*|*-debugsource-*) continue ;;
                *) rpms+=("$file") ;;
            esac
        done < <(find "$d" -maxdepth 1 -type f -name '*.rpm' -print0 | sort -z)

        if [ "${#rpms[@]}" -eq 0 ]; then
            skip "RPM el${el}：未找到二进制 RPM"
            continue
        fi

        main=""
        for file in "${rpms[@]}"; do
            if printf '%s' "$(basename "$file")" | grep -Eq -- "-1\.el${el}\.x86_64\.rpm$"; then
                main="$file"; break
            fi
        done
        [ -n "$main" ] || main="${rpms[0]}"
        name="$(basename "$main")"
        info "el${el}：$name"

        size="$(stat -c '%s' "$main" 2>/dev/null || echo 0)"
        if [ "$size" -gt 0 ]; then pass "RPM el${el} 文件非空（$size 字节）"
        else fail "RPM el${el} 文件为空：$name"; continue; fi

        backend="$(rpm_backend "$el")"
        if [ -z "$backend" ]; then
            skip "RPM el${el}：宿主机无 rpm/rpm2cpio，且镜像 ter-music-rpm-el${el} 不可用"
            continue
        fi

        ver="$(rpm_query "$backend" "$el" "$main" -qp --qf '%{VERSION}')"
        arch="$(rpm_query "$backend" "$el" "$main" -qp --qf '%{ARCH}')"
        info "Version=$ver Arch=$arch（backend=$backend）"
        record_version "RPM el${el}" "$ver"
        if [ "$arch" = "x86_64" ]; then pass "RPM el${el} Arch = x86_64"
        else fail "RPM el${el} Arch 期望 x86_64，实际 '${arch:-<空>}'"; fi

        requires="$(rpm_query "$backend" "$el" "$main" -qpR)"
        if printf '%s\n' "$requires" | grep -q 'libxml2'; then
            pass "RPM el${el} Requires 含 libxml2"
        else
            fail "RPM el${el} Requires 缺少 libxml2"
        fi
        if printf '%s\n' "$requires" | grep -q 'libav'; then
            fail "RPM el${el} Requires 含 libav*"
        else
            pass "RPM el${el} Requires 无 libav*"
        fi

        extract="$WORK/rpm.${el}.$$.$RANDOM"
        rm -rf -- "$extract"
        mkdir -p "$extract"
        if ! rpm_extract "$backend" "$el" "$main" "$extract"; then
            fail "RPM el${el} 解包失败"
            continue
        fi
        bin="$extract/usr/bin/ter-music"
        if [ -f "$bin" ]; then
            pass "RPM el${el} 含 usr/bin/ter-music"
        else
            fail "RPM el${el} 缺少 usr/bin/ter-music"
            continue
        fi
        forbidden="$(needed_forbidden "$bin")"
        if [ -z "$forbidden" ]; then
            pass "RPM el${el} 二进制 DT_NEEDED 无 libav*/libxml2"
        else
            fail "RPM el${el} DT_NEEDED 含禁止依赖：$(printf '%s' "$forbidden" | tr '\n' ' ')"
        fi

        if [ "$backend" != "docker" ]; then
            skip "RPM el${el} 运行期：需要 ter-music-rpm-el${el} 镜像，宿主机 rpm 无法验证安装"
        elif [ "$NO_RUNTIME" -ne 0 ]; then
            skip "RPM el${el} 运行期：--no-runtime"
        else
            local out
            out="$(docker run --rm -v "${main}:/tmp/p.rpm:ro" "ter-music-rpm-el${el}" bash -lc \
                'rpm -Uvh --replacepkgs /tmp/p.rpm >/dev/null 2>&1 || true; /usr/bin/ter-music --version 2>&1' 2>&1)"
            if printf '%s' "$out" | grep -q "$(norm_ver "$ver")"; then
                pass "RPM el${el} 运行期：ter-music --version -> $(printf '%s' "$out" | head -n1)"
            else
                skip "RPM el${el} 运行期未能验证：$(printf '%s' "$out" | tail -n1)"
            fi
        fi

        while IFS= read -r -d '' file; do
            size="$(stat -c '%s' "$file" 2>/dev/null || echo 0)"
            if [ "$size" -gt 0 ]; then pass "RPM el${el} 源码包非空：$(basename "$file")"
            else fail "RPM el${el} 源码包为空：$(basename "$file")"; fi
        done < <(find "$d/../source" -maxdepth 1 -type f -name '*.src.rpm' -print0 2>/dev/null | sort -z)
    done
}
# ── AppImage ───────────────────────────────────────────────────────────
audit_appimage() {
    section "AppImage"
    local dir="$ROOT/build/appimage/x86_64"
    local -a apps=()
    local app

    if [ -d "$dir" ]; then
        while IFS= read -r -d '' app; do apps+=("$app"); done \
            < <(find "$dir" -maxdepth 1 -type f -name '*.AppImage' -print0 | sort -z)
    fi
    if [ "${#apps[@]}" -eq 0 ]; then
        skip "AppImage：未找到 build/appimage/x86_64/*.AppImage"
        return 0
    fi

    local name size root out ver
    for app in "${apps[@]}"; do
        name="$(basename "$app")"
        info "契约：$name"
        size="$(stat -c '%s' "$app" 2>/dev/null || echo 0)"
        if [ "$size" -gt 0 ]; then pass "AppImage 文件非空（$size 字节）"
        else fail "AppImage 文件为空：$name"; continue; fi

        ver="$(printf '%s' "$name" | sed -nE 's/^ter-music-([0-9][0-9.]*)-x86_64\.AppImage$/\1/p')"
        [ -n "$ver" ] || ver="$(printf '%s' "$name" | sed -nE 's/.*-([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)-x86_64\.AppImage$/\1/p')"
        record_version "AppImage $name" "$ver"
        if [ -n "$ver" ]; then pass "文件名版本可解析（$ver）"
        else fail "无法从文件名解析版本：$name"; fi

        root="$WORK/appimage.$$.$RANDOM"
        rm -rf -- "$root"
        mkdir -p "$root"
        if ! ( cd "$root" && "$app" --appimage-extract >/dev/null 2>&1 ); then
            fail "AppImage --appimage-extract 失败：$name"
            continue
        fi
        root="$root/squashfs-root"
        if [ -d "$root" ]; then pass "解包出 squashfs-root"
        else fail "解包后缺少 squashfs-root"; continue; fi

        if [ -x "$root/AppRun" ]; then pass "AppRun 可执行"
        else fail "AppRun 不存在或不可执行"; fi

        if [ -f "$root/usr/lib/libxml2.so.2" ]; then pass "usr/lib/libxml2.so.2 存在"
        else fail "usr/lib 缺少 libxml2.so.2"; fi

        out="$(find "$root/usr/lib" -maxdepth 1 -type f -name 'libav*.so*' 2>/dev/null)"
        if [ -z "$out" ]; then pass "usr/lib 无 libav*.so*"
        else fail "usr/lib 混入 FFmpeg 动态库：$(basename "$out" | tr '\n' ' ')"; fi

        if [ -f "$root/usr/bin/ter-music" ]; then
            out="$(needed_forbidden "$root/usr/bin/ter-music")"
            if [ -z "$out" ]; then pass "ter-music DT_NEEDED 无 libav*/libxml2"
            else fail "ter-music DT_NEEDED 含禁止依赖：$(printf '%s' "$out" | tr '\n' ' ')"; fi
        else
            fail "squashfs-root 缺少 usr/bin/ter-music"
        fi

        if [ ! -x "$root/AppRun" ]; then
            skip "AppImage 运行期：AppRun 不可执行"
        elif [ -z "$ver" ]; then
            skip "AppImage 运行期：文件名版本无法解析"
        else
            out="$(cd "$root" && timeout 60 ./AppRun --version 2>&1)"
            if printf '%s' "$out" | grep -q "$ver"; then
                pass "AppImage 运行期：ter-music --version -> $(printf '%s' "$out" | head -n1)"
            else
                fail "AppImage 运行期版本不匹配：$(printf '%s' "$out" | tail -n1)"
            fi
        fi
    done
}

# ── Linyaps ────────────────────────────────────────────────────────────
verify_uab_digest() { # 校验 UAB 内嵌 .note.uab.sig 与 linglong.meta
    local uab="$1"
    have python3 || return 0
    python3 - "$uab" <<'PY'
import hashlib
import json
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as handle:
    data = handle.read()
if data[:4] != b"\x7fELF":
    print("FAIL not an ELF file")
    raise SystemExit(0)

shoff = struct.unpack_from("<Q", data, 0x28)[0]
shentsize = struct.unpack_from("<H", data, 0x3A)[0]
shnum = struct.unpack_from("<H", data, 0x3C)[0]
shstrndx = struct.unpack_from("<H", data, 0x3E)[0]

def section(index):
    base = shoff + index * shentsize
    name, _type, _flags, _addr, offset, size = struct.unpack_from("<IIQQQQ", data, base)
    return name, offset, size

_, stroff, _ = section(shstrndx)

def name_of(offset):
    end = data.index(b"\0", stroff + offset)
    return data[stroff + offset:end].decode()

sections = {}
for index in range(shnum):
    name, offset, size = section(index)
    sections[name_of(name)] = (offset, size)

if "linglong.meta" not in sections or ".note.uab.sig" not in sections:
    print("FAIL missing linglong.meta or .note.uab.sig")
    raise SystemExit(0)

meta_off, meta_size = sections["linglong.meta"]
note_off, note_size = sections[".note.uab.sig"]
meta = data[meta_off:meta_off + meta_size]
note = data[note_off:note_off + note_size]

if note_size < 28 + 64:
    print("FAIL .note.uab.sig section too short")
    raise SystemExit(0)

digest = note[28:92].decode("ascii", "replace")
expected = hashlib.sha256(meta).hexdigest()
if digest != expected:
    print("FAIL .note.uab.sig digest mismatch")
    raise SystemExit(0)

try:
    meta_json = json.loads(meta)
except ValueError as exc:
    print(f"FAIL linglong.meta is not JSON: {exc}")
    raise SystemExit(0)

print(f"OK version={meta_json.get('version')}")
PY
}

audit_linyaps() {
    section "Linyaps"
    local dir=""
    local candidate
    for candidate in "$ROOT/build/linyaps/x86_64" "$ROOT/build/linglong" "$ROOT/build/linyaps"; do
        if [ -d "$candidate" ]; then dir="$candidate"; break; fi
    done
    if [ -z "$dir" ]; then
        skip "Linyaps：未找到 build/linyaps/x86_64"
        return 0
    fi

    local -a uabs=() layers=()
    local f name size ver digest
    while IFS= read -r -d '' f; do uabs+=("$f"); done \
        < <(find "$dir" -maxdepth 1 -type f -name '*.uab' -print0 | sort -z)
    while IFS= read -r -d '' f; do layers+=("$f"); done \
        < <(find "$dir" -maxdepth 1 -type f -name '*.layer' -print0 | sort -z)

    if [ "${#uabs[@]}" -eq 0 ] && [ "${#layers[@]}" -eq 0 ]; then
        skip "Linyaps：目录存在但没有 .uab / *.layer 产物"
        return 0
    fi

    if [ "${#uabs[@]}" -eq 0 ]; then
        fail "Linyaps 缺少 .uab"
    else
        for f in "${uabs[@]}"; do
            name="$(basename "$f")"
            size="$(stat -c '%s' "$f" 2>/dev/null || echo 0)"
            if [ "$size" -gt 0 ]; then pass "UAB 非空：$name（$size 字节）"
            else fail "UAB 为空：$name"; continue; fi
            info "file：$(file -b "$f" 2>/dev/null | cut -c1-100)"
            ver="$(printf '%s' "$name" | sed -nE 's/^.*_([0-9][0-9.]*)_x86_64\.uab$/\1/p')"
            record_version "UAB $name" "$ver"
            digest="$(verify_uab_digest "$f")"
            case "$digest" in
                OK*) pass "UAB 内嵌摘要校验通过（${digest#OK }）" ;;
                FAIL*) fail "UAB 内嵌摘要校验失败：${digest#FAIL }" ;;
                "") skip "UAB 摘要校验：缺少 python3" ;;
                *) fail "UAB 摘要校验异常：$(printf '%s' "$digest" | head -n1)" ;;
            esac
        done
    fi

    if [ "${#layers[@]}" -eq 0 ]; then
        fail "Linyaps 缺少 *_binary.layer"
    else
        for f in "${layers[@]}"; do
            name="$(basename "$f")"
            size="$(stat -c '%s' "$f" 2>/dev/null || echo 0)"
            if [ "$size" -gt 0 ]; then pass "layer 非空：$name（$size 字节）"
            else fail "layer 为空：$name"; fi
            info "file：$(file -b "$f" 2>/dev/null | cut -c1-100)"
        done
    fi

    while IFS= read -r -d '' f; do
        if ( cd "$dir" && sha256sum -c "$(basename "$f")" >/dev/null 2>&1 ); then
            pass "摘要文件校验通过：$(basename "$f")"
        else
            fail "摘要文件校验失败：$(basename "$f")"
        fi
    done < <(find "$dir" -maxdepth 1 -type f -name '*.sha256' -print0 | sort -z)
}

# ── AUR ────────────────────────────────────────────────────────────────
audit_aur() {
    section "AUR"
    local dir="$ROOT/build/aur/x86_64"
    local -a pkgs=()
    local pkg

    if [ -d "$dir" ]; then
        while IFS= read -r -d '' pkg; do pkgs+=("$pkg"); done \
            < <(find "$dir" -maxdepth 1 -type f -name '*.pkg.tar.zst' -print0 | sort -z)
    fi
    if [ "${#pkgs[@]}" -eq 0 ]; then
        skip "AUR：未找到 build/aur/x86_64/*.pkg.tar.zst"
        return 0
    fi
    if ! have tar; then
        skip "AUR：宿主机缺少 tar"
        return 0
    fi

    local name size listing pkginfo ver filever deps
    for pkg in "${pkgs[@]}"; do
        name="$(basename "$pkg")"
        info "契约：$name"
        size="$(stat -c '%s' "$pkg" 2>/dev/null || echo 0)"
        if [ "$size" -gt 0 ]; then pass "AUR 包非空（$size 字节）"
        else fail "AUR 包为空：$name"; continue; fi

        if ! listing="$(tar --zstd -tf "$pkg" 2>/dev/null)"; then
            skip "AUR：无法读取 zstd tar（tar 不支持 --zstd 或 zstd 缺失）"
            continue
        fi
        if printf '%s\n' "$listing" | grep -Eq '(^|/)usr/bin/ter-music$'; then
            pass "包含 usr/bin/ter-music"
        else
            fail "缺少 usr/bin/ter-music"
        fi

        if ! pkginfo="$(tar --zstd -xOf "$pkg" .PKGINFO 2>/dev/null)"; then
            fail "无法读取 .PKGINFO"
            continue
        fi
        # Arch 的 pkgver 在 .PKGINFO 中是 "<pkgver>-<pkgrel>"（如 26.4.1.0-1），
        # 与文件名中的版本段一致；版本一致性比较时去掉 pkgrel。
        ver="$(printf '%s\n' "$pkginfo" | sed -n 's/^pkgver[[:space:]]*=[[:space:]]*//p' | head -n1)"
        info ".PKGINFO pkgver=$ver"
        record_version "AUR $name" "${ver%-*}"
        filever="$(printf '%s' "$name" | sed -nE 's/^cn\.org\.huansoft\.ter-music-([0-9][0-9.]*-[0-9]+)-x86_64\.pkg\.tar\.zst$/\1/p')"
        if [ -n "$filever" ] && [ "$filever" = "$ver" ]; then
            pass "文件名版本与 .PKGINFO 一致（$ver）"
        else
            fail "文件名版本 '${filever:-<无法解析>}' != .PKGINFO '$ver'"
        fi

        deps="$(printf '%s\n' "$pkginfo" | sed -n 's/^depend[[:space:]]*=[[:space:]]*//p')"
        if printf '%s\n' "$deps" | grep -q '^libxml2$'; then
            pass "依赖含 libxml2"
        else
            fail "依赖缺少 libxml2"
        fi
        if printf '%s\n' "$deps" | grep -q '^ffmpeg$'; then
            pass "依赖含 ffmpeg"
        else
            fail "依赖缺少 ffmpeg"
        fi
    done
}

# ── 汇总 ───────────────────────────────────────────────────────────────
check_version_consistency() {
    section "版本一致性"
    local entry values unique
    if [ "${#ARTIFACT_VERSIONS[@]}" -eq 0 ]; then
        skip "没有可供比较的产物版本"
        return 0
    fi
    values="$(printf '%s\n' "${ARTIFACT_VERSIONS[@]}" | sed 's/^[^=]*=//' | sort -u)"
    unique="$(printf '%s\n' "$values" | wc -l | tr -d ' ')"
    if [ "$unique" -le 1 ]; then
        pass "所有产物版本一致：$values"
    else
        fail "产物版本不一致：$(printf '%s' "$values" | tr '\n' ' ')"
        for entry in "${ARTIFACT_VERSIONS[@]}"; do info "$entry"; done
    fi
}

for target in "${TARGETS[@]}"; do
    case "$target" in
        deb) audit_deb ;;
        rpm) audit_rpm ;;
        appimage) audit_appimage ;;
        linyaps) audit_linyaps ;;
        aur) audit_aur ;;
    esac
done

check_version_consistency

printf '\n--- 汇总 ---\n'
printf 'root=%s  runtime=%s\n' "$ROOT" "$([ "$NO_RUNTIME" -eq 1 ] && echo off || echo on)"
printf '%d PASS, %d FAIL, %d SKIP\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
