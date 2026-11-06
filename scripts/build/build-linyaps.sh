#!/usr/bin/env bash
#
# 宿主原生调用 ll-builder 构建 Linyaps：UAB + layer 双产物。
# 不走 Docker；容器化由 ll-builder/ll-box 自己提供。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=common.sh
. "${SCRIPT_DIR}/scripts/build/common.sh"

PROJECT_NAME="ter-music"
APP_ID="cn.org.huansoft.ter-music"
REPO_ROOT="$SCRIPT_DIR"
TEMP_DIR="${TER_MUSIC_LINYAPS_TEMP:-${REPO_ROOT}/.tmp/linyaps-temp}"
OUT_DIR="${REPO_ROOT}/build/linyaps/x86_64"
LINYAPS_CACHE_DIR="${HOME}/.cache/linglong-builder"
# deepin 默认 5 秒连接超时对首次拉取 base/runtime 太短。
export LINGLONG_CONNECT_TIMEOUT="${LINGLONG_CONNECT_TIMEOUT:-120}"

show_help() {
    cat <<EOF
用法: $0 -v VERSION [选项]

在宿主机上直接用 ll-builder 构建 Linyaps，产出 UAB + layer。

选项:
  -v, --version VERSION   必填；三段自动补 .0 为四段
  -a, --arch ARCH         仅支持 x86_64（默认 x86_64）
  -k, --keep-temp         保留 ${TEMP_DIR}
  -o, --offline           强制离线，不拉取 base/runtime
  -r, --refresh           强制从软件源刷新 base/runtime
  -h, --help              显示帮助

输出:
  build/linyaps/x86_64/cn.org.huansoft.ter-music_<V>_x86_64.uab
  build/linyaps/x86_64/<...>_binary.layer
EOF
}

VERSION=""
ARCH="x86_64"
KEEP_TEMP=false
OFFLINE=false
REFRESH=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        -v|--version) VERSION="${2:-}"; shift 2 ;;
        -a|--arch) ARCH="${2:-}"; shift 2 ;;
        -k|--keep-temp) KEEP_TEMP=true; shift ;;
        -o|--offline) OFFLINE=true; shift ;;
        -r|--refresh) REFRESH=true; shift ;;
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

missing=()
for cmd in ll-builder ll-box cmake make python3; do
    command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
done
if [ "${#missing[@]}" -gt 0 ]; then
    tm_log_error "missing build tools: ${missing[*]}"
    tm_log_error "install linglong-builder/linglong-box, cmake, make and python3 first"
    exit 1
fi
tm_log_info "ll-builder: $(ll-builder --version 2>&1 | head -n 1)"
if command -v ll-cli >/dev/null 2>&1; then
    tm_log_info "ll-cli:     $(ll-cli --version 2>&1 | head -n 1)"
fi

# UAB 是 ELF 包：.note.uab.sig 段必须回填 sha256(linglong.meta)。
verify_uab_digest() {
    local uab_file="$1"
    local result
    result=$(python3 - "$uab_file" <<'PY' 2>&1
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

bundle_digest = str(meta_json.get("digest", ""))
if len(bundle_digest) != 64:
    print("FAIL linglong.meta has no valid bundle digest")
    raise SystemExit(0)

print(f"OK digest={expected[:16]} bundle={bundle_digest[:16]} version={meta_json.get('version')}")
PY
) || result="FAIL UAB verification script failed"

    if [[ "$result" == OK* ]]; then
        tm_log_info "UAB signature check: ${result#OK }"
        return 0
    fi
    tm_log_error "UAB signature check failed:"
    printf '%s\n' "$result" | sed 's/^/  /'
    return 1
}

mkdir -p "$OUT_DIR"
rm -rf "$TEMP_DIR"
mkdir -p "$TEMP_DIR"
PROJECT_ROOT="${TEMP_DIR}/${APP_ID}"
mkdir -p "$PROJECT_ROOT"

tm_stage_source "$PROJECT_ROOT/${PROJECT_NAME}" \
    "debian" "rpm" "linglong" "PKGBUILD" ".SRCINFO" "docs" "tools" "scripts" ".github"

sed -e "s/@VERSION@/${VERSION}/g" -e "s/@ARCH@/${ARCH}/g" \
    "${REPO_ROOT}/linglong/linglong.yaml.in" > "${PROJECT_ROOT}/linglong.yaml"

cd "$PROJECT_ROOT"
build_args=()
if [ "$OFFLINE" = true ]; then
    build_args=(--offline)
    tm_log_info "offline build: base/runtime must already be cached"
elif [ "$REFRESH" = true ]; then
    build_args=(--skip-fetch-source)
    tm_log_info "refresh: pulling base/runtime from mirrors"
elif [ -s "${LINYAPS_CACHE_DIR}/states.json" ]; then
    build_args=(--offline)
    tm_log_info "local Linyaps cache found; using --offline"
else
    build_args=(--skip-fetch-source)
fi

build_log="${TEMP_DIR}/ll-builder-output.log"
set +e
ll-builder build "${build_args[@]}" 2>&1 | tee "$build_log"
build_rc=${PIPESTATUS[0]}
set -e
if [ "$build_rc" -ne 0 ]; then
    tm_log_error "ll-builder build failed (see ${build_log})"
    exit 1
fi

uab_ok=false
layer_ok=false
uab_tmp="${PROJECT_ROOT}/output.uab"
uab_final="${OUT_DIR}/${APP_ID}_${VERSION}_${ARCH}.uab"
layer_src=""

tm_log_info "export UAB"
if ll-builder export --ref "main:${APP_ID}/${VERSION}/${ARCH}" -o "$uab_tmp" && [ -s "$uab_tmp" ]; then
    if verify_uab_digest "$uab_tmp"; then
        mv -f "$uab_tmp" "$uab_final"
        chmod 644 "$uab_final" 2>/dev/null || true
        tm_copy_release "$uab_final"
        uab_ok=true
    else
        tm_log_error "UAB export produced an invalid .note.uab.sig"
        rm -f "$uab_tmp"
    fi
else
    tm_log_error "UAB export failed"
    rm -f "$uab_tmp"
fi

tm_log_info "export layer"
if ll-builder export --layer; then
    layer_src="$(find "$PROJECT_ROOT" -maxdepth 1 -name '*_binary.layer' -type f | head -n 1)"
    if [ -n "$layer_src" ]; then
        layer_final="${OUT_DIR}/$(basename "$layer_src")"
        mv -f "$layer_src" "$layer_final"
        chmod 644 "$layer_final" 2>/dev/null || true
        tm_copy_release "$layer_final"
        layer_ok=true
    else
        tm_log_error "layer export finished but no *_binary.layer was found"
    fi
else
    tm_log_error "layer export failed"
fi

tm_fix_ownership
if [ "$KEEP_TEMP" != true ]; then
    rm -rf "$TEMP_DIR"
fi

if [ "$uab_ok" != true ] || [ "$layer_ok" != true ]; then
    tm_log_error "Linyaps requires both UAB and layer; build failed"
    exit 1
fi

echo ""
echo "=========================================="
echo "Linyaps 构建完成"
echo "=========================================="
echo "UAB:   ${uab_final}"
echo "layer: ${OUT_DIR}/$(basename "${layer_src:-layer}")"
echo ""
echo "安装/验证:"
echo "  ll-cli install ${uab_final}"
echo "  ll-cli install <layer 文件>"
echo "  ll-cli run ${APP_ID} -- ter-music version"
