#!/bin/bash

set -e

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
SCRIPT_DIR="$(pwd)"
PROJECT_NAME="ter-music"
APP_ID="org.yxzl.ter-music"
OUTPUT_DIR="${SCRIPT_DIR}/build/linyaps"
TEMP_DIR="${TER_MUSIC_LINYAPS_TEMP:-${SCRIPT_DIR}/.tmp/linyaps-temp}"

# 确保路径是绝对路径的函数
ensure_absolute_path() {
    local path="$1"
    if [[ "$path" != /* ]]; then
        path="$(pwd)/$path"
    fi
    echo "$path"
}

log_info() {
    echo "[INFO] $1"
}

log_error() {
    echo "[ERROR] $1" >&2
}

log_clean() {
    echo "[CLEAN] $1"
}

log_warn() {
    echo "[WARN] $1" >&2
}

# 复制构建结果到 build/release 目录
copy_to_release() {
    local source_file="$1"
    local release_dir="${SCRIPT_DIR}/build/release"
    
    if [ -f "$source_file" ]; then
        mkdir -p "$release_dir"
        if cp "$source_file" "$release_dir/"; then
            log_info "构建结果已复制到: ${release_dir}/$(basename "$source_file")"
        fi
    fi
}

show_help() {
    cat << EOF
用法: $0 [选项]

直接从源码构建 ter-music Linyaps（如意玲珑）软件包

选项:
    -h, --help          显示此帮助信息
    -v, --version VERSION  指定版本号（默认：自动检测）
    -a, --arch ARCH     指定目标架构（默认：自动检测）
    -k, --keep-temp     保留临时构建文件（用于调试）
    -o, --offline       强制离线：不拉取源码与依赖（要求 base/runtime 已在本地缓存中）
    -r, --refresh       强制拉取依赖：从软件源刷新 base/runtime（默认仅在缓存为空时拉取）
    --layer-only        只导出 layer，不导出 UAB（本地拿不到 builder.utils ≥0.0.4.0 时用）

构建方式（原生，不再经过 Docker）:
    Linyaps 自带容器化（ll-box）：ll-builder build 会拉起构建容器安装
    buildext 依赖并编译。外面再套一层 Docker 只会引入两类问题——容器内工具链
    与宿主 ll-cli 版本错配（UAB 签名段不回填 → 安装报 invalid digest），以及
    嵌套 overlayfs 无法挂载（Docker 的 /tmp 就在 overlay 上，不能当另一个
    overlay 的 upperdir）。因此本脚本直接调用宿主的 ll-builder。

    前置条件：宿主已安装 linglong-builder/linglong-box，且 ll-builder --version
    与 ll-cli --version 匹配（版本错配会导致 UAB 签名段不回填）。

缓存说明:
    base/runtime 与构建层缓存在 ~/.cache/linglong-builder。
    首次构建会下载数百 MB，之后构建直接复用，不再重新下载。
    缓存非空时默认自动进入离线模式（等价于 --offline），避免重复下载。
    需要拉取新的 base/runtime（例如修改了 linglong.yaml 的 base 版本）时加 --refresh。

示例:
    $0                  使用自动检测版本和架构构建 Linyaps 包
    $0 -v 1.1.2         使用指定版本号构建 Linyaps 包
    $0 --keep-temp      构建后保留临时文件

输出:
    Linyaps 包将输出到: ${OUTPUT_DIR}/<arch>/

EOF
}

check_dependencies() {
    local target_arch="${1:-}"
    
    log_info "检查构建依赖..."

    local missing_deps=()

    if ! command -v ll-builder &> /dev/null; then
        missing_deps+=("linglong-builder (ll-builder)")
    fi

    if ! command -v ll-box &> /dev/null; then
        missing_deps+=("linglong-box (ll-box)")
    fi

    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi

    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
    fi

    if [ ${#missing_deps[@]} -gt 0 ]; then
        log_error "缺少以下构建工具:"
        for dep in "${missing_deps[@]}"; do
            echo "  - $dep"
        done
        echo ""
        log_error "请使用以下命令安装缺失的工具:"
        echo "  Debian/Ubuntu 系: sudo apt install linglong-builder linglong-box cmake make"
        echo "  RPM 系 (Fedora/openEuler): sudo dnf install linglong-builder linglong-box cmake make"
        exit 1
    fi

    # 工具链版本必须留痕：镜像时代的故障（UAB 签名段不回填）就是版本错配造成的
    log_info "  ll-builder: $(ll-builder --version 2>&1 | head -1)"
    if command -v ll-cli >/dev/null 2>&1; then
        log_info "  ll-cli:     $(ll-cli --version 2>&1 | head -1)"
    fi

    log_info "所有构建依赖已满足"
}

detect_version() {
    local default_version="2.3.0"

    if [ -d "${SCRIPT_DIR}/.git" ] && command -v git >/dev/null 2>&1; then
        local git_version=$(git describe --tags --abbrev=0 2>/dev/null || true)
        if [[ $git_version =~ ^v?([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
            echo "${BASH_REMATCH[1]}"
            return
        fi
    fi

    if [ -f "${SCRIPT_DIR}/include/org.yxzl.ter-music/types.h" ]; then
        local match
        match=$(grep -E 'APP_VERSION' "${SCRIPT_DIR}/include/org.yxzl.ter-music/types.h" | head -1)
        if [[ $match =~ ([0-9]+\.[0-9]+\.[0-9]+) ]]; then
            echo "${BASH_REMATCH[1]}"
            return
        fi
    fi

    echo "$default_version"
}

detect_architecture() {
    # Only x86_64 is supported
    echo "x86_64"
}

validate_architecture() {
    local arch="$1"
    if [ "$arch" != "x86_64" ]; then
        log_error "不支持的架构: $arch（仅支持 x86_64）"
        return 1
    fi
    return 0
}

prepare_linyaps_structure() {
    local temp_dir="$1"
    local app_id="$2"

    log_info "准备 Linyaps 项目目录结构..."

    PROJECT_ROOT_OUTPUT="${temp_dir}/${app_id}"

    mkdir -p "$PROJECT_ROOT_OUTPUT/${PROJECT_NAME}"

    log_info "复制源码到构建目录..."
    # 必须排除 .tmp（临时构建目录就在 ${TEMP_DIR} = .tmp/linyaps-temp 下，不排除会
    # 把上一轮产物递归拷进源码树）与 .cache/.linyaps_temp（历次容器构建留下的
    # root 属主缓存，rsync 读不动会直接以 code 23 失败）。
    rsync -a --exclude=.git --exclude=build --exclude=.tmp --exclude=.cache \
          --exclude=.linyaps_temp \
          "${SCRIPT_DIR}/" "${PROJECT_ROOT_OUTPUT}/${PROJECT_NAME}/"

    log_info "目录结构创建完成"
}

generate_linglong_yaml() {
    local project_root="$1"
    local app_id="$2"
    local version="$3"
    local target_arch="$4"

    log_info "生成 linglong.yaml 配置文件..."

    # Linyaps要求版本号为四段式，如果用户提供的是三段式，添加一个.0
    local linyaps_version="$version"
    if [[ "$linyaps_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        linyaps_version="${linyaps_version}.0"
    fi

    cat > "${project_root}/linglong.yaml" << EOF
# SPDX-License-Identifier: LGPL-3.0-or-later
version: "1"

package:
  id: ${app_id}
  name: ${PROJECT_NAME}
  version: ${linyaps_version}
  kind: app
  description: |
    ter-music 是一个基于 ncurses 的轻量级终端音乐播放器
  architecture: ${target_arch}

command:
  - /opt/apps/${app_id}/files/bin/${PROJECT_NAME}

base: org.deepin.base/25.2.2

sources:
  - kind: file
    name: ${PROJECT_NAME}
    url: ./${PROJECT_NAME}

build: |
  cd ${PROJECT_NAME}
  mkdir -p build
  cd build
  cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=\${PREFIX} -DCMAKE_INSTALL_RPATH='\$ORIGIN/../lib' -DINSTALL_LINYAPS_INTEGRATION=ON
  make -j\$(nproc)
  make install
  mkdir -p \${PREFIX}/lib
  for dep in libblas.so.3 liblapack.so.3 libmpg123.so.0; do
    dep_path=\$(find /usr/lib /lib \\( -type f -o -type l \\) -name "\${dep}" 2>/dev/null | head -n 1)
    if [ -z "\${dep_path}" ]; then
      echo "required runtime library not found: \${dep}" >&2
      exit 1
    fi
    install -Dm755 "\$(readlink -f "\${dep_path}")" "\${PREFIX}/lib/\${dep}"
  done

buildext:
  apt:
    build_depends:
      - build-essential
      - cmake
      - pkg-config
      - libncurses-dev
      - libavformat-dev
      - libavcodec-dev
      - libswresample-dev
      - libtag1-dev
      - libpulse-dev
      - libavutil-dev
      - libavdevice-dev
      - libavfilter-dev
      - libswscale-dev
      - libpostproc-dev
      - libblas3
      - liblapack3
      - libpng-dev
      - libjpeg-dev
      - libxml2-dev
      - libcurl4-openssl-dev
      - libsqlite3-dev
      - libdbus-1-dev
      - libasound2-dev
    depends:
      - libavformat60
      - libavcodec60
      - libswresample4
      - libavutil58
      - libavfilter9
      - libtag1v5
      - libpulse0
      - libblas3
      - liblapack3
      - libgfortran5
      - libpng16-16
      - libjpeg62-turbo
      - libxml2
      - libcurl4
      - libswscale7
      - libmpg123-0
      - libvorbis0a
      - libvorbisenc2
      - libvorbisfile3
      - libopus0
      - libtheora0
      - libx264-160
      - libx265-199
      - libvpx7
      - libzstd1
      - liblzma5
EOF

    log_info "linglong.yaml 已生成: ${project_root}/linglong.yaml"
}

build_linyaps() {
    local project_root="$1"

    log_info "执行 ll-builder 构建（原生，容器由 ll-box 提供）..."
    cd "$project_root"

    # 捕获 ll-builder 输出以便分析失败原因
    local build_log="${TEMP_DIR}/ll-builder-output.log"

    # 运行 ll-builder（保留 stderr 以显示构建过程）
    # 依赖拉取策略（详见 show_help 的“缓存说明”）：
    #   --offline       强制离线
    #   --refresh       强制从软件源拉取 base/runtime
    #   默认            本地缓存非空时自动离线，缓存为空时正常拉取
    # 源码始终已在本地（--skip-fetch-source），无需从网络获取。
    local builder_args=(--skip-fetch-source)
    if [ "${OFFLINE_BUILD}" = "true" ]; then
        builder_args=(--offline)
        log_info "离线构建：不拉取源码与依赖（使用本地缓存）"
    elif [ "${REFRESH_DEPS}" = "true" ]; then
        log_info "强制刷新依赖：将从软件源拉取 base/runtime"
    elif linyaps_cache_ready; then
        builder_args=(--offline)
        USED_AUTO_OFFLINE="true"
        log_info "检测到本地 Linyaps 缓存，自动离线构建（跳过依赖拉取，不重新下载）"
        log_info "  缓存目录: ${LINYAPS_CACHE_DIR}"
        log_info "  如需拉取新的 base/runtime，请使用 --refresh"
    fi
    set +e
    ll-builder build "${builder_args[@]}" 2>&1 | tee "$build_log"
    local rc=${PIPESTATUS[0]}
    set -e

    if [ "$rc" -eq 0 ]; then
        log_info "Linyaps 容器构建完成"
        return 0
    fi

    # 构建返回非零，分析失败原因：区分"编译失败"和"Runtime Check 失败"。
    # 原生构建下 Runtime Check 失败就是真失败（镜像时代它多半是 Docker 里
    # 嵌套 overlayfs 挂不上的连带现象，那套绕过已随去 Docker 化删除）。
    if grep -q '\[Commit Contents\]' "$build_log" 2>/dev/null && \
       grep -q 'committing' "$build_log" 2>/dev/null && \
       grep -qE 'Runtime check failed|stage runtime check error|OverlayFS mount failed' "$build_log" 2>/dev/null; then
        log_error "编译与提交成功，但 Runtime Check 失败（原生构建下应为真失败）"
        log_info "  可尝试删除 ${LINYAPS_CACHE_DIR} 后重建缓存；若持续失败请检查 ll-box/内核 overlayfs 支持"
        return 1
    fi

    # 自动离线构建失败时，最常见原因是本地缓存缺少所需的 base/runtime
    if [ "${USED_AUTO_OFFLINE}" = "true" ] && \
       grep -qiE 'not found|no such|failed to pull|unreachable|connection refused|no route' "$build_log" 2>/dev/null; then
        log_error "自动离线构建失败：本地缓存可能缺少所需的 base/runtime"
        log_info "请加 --refresh 重新拉取依赖后重试（或删除 ${LINYAPS_CACHE_DIR} 后重新构建）"
    fi

    log_error "Linyaps 构建失败"
    return 1
}

# ── UAB 签名段校验 ────────────────────────────────────────────────
# UAB 是 ELF 包：`linglong.meta` 段放元数据，`.note.uab.sig` 段的 note 描述符里
# 存 64 字节 ASCII 十六进制，值是 sha256(linglong.meta)。新版 ll-cli 安装前强制
# 校验它；旧版 ll-builder（1.13.x）不认识该段，导出后仍是占位符 '!'+零，包能生成
# 但装不上（实测报 "section .note.uab.sig has an invalid digest"）。
# 这里在打包阶段就断言，避免把问题推到安装现场。
verify_uab_digest() {
    local uab_file="$1"

    if ! command -v python3 >/dev/null 2>&1; then
        log_warn "未找到 python3，跳过 UAB 签名段校验"
        return 0
    fi

    # 注意 set -e：命令替换失败会直接终止脚本，这里显式兜住，改成可读的失败信息
    local result
    result=$(python3 - "$uab_file" <<'PY' 2>&1
import hashlib, json, struct, sys

path = sys.argv[1]
with open(path, "rb") as handle:
    data = handle.read()

if data[:4] != b"\x7fELF":
    print("FAIL 不是 ELF 文件（UAB 应为 ELF 包）")
    raise SystemExit(0)

shoff = struct.unpack_from("<Q", data, 0x28)[0]
shentsize = struct.unpack_from("<H", data, 0x3a)[0]
shnum = struct.unpack_from("<H", data, 0x3c)[0]
shstrndx = struct.unpack_from("<H", data, 0x3e)[0]

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
    print("FAIL 缺少 linglong.meta 或 .note.uab.sig 段")
    raise SystemExit(0)

meta_off, meta_size = sections["linglong.meta"]
note_off, note_size = sections[".note.uab.sig"]
meta = data[meta_off:meta_off + meta_size]
note = data[note_off:note_off + note_size]

# note 结构：12 字节头 + 对齐后的名字 + 64 字节 digest（偏移 28）
if note_size < 28 + 64:
    print(f"FAIL .note.uab.sig 段过短（{note_size} 字节）")
    raise SystemExit(0)
digest = note[28:92].decode("ascii", "replace")
# 占位符里是 '!' + 一堆 NUL：直接打印会把 NUL 混进输出（bash 命令替换会警告并丢弃），
# 因此显示时把不可打印字符换成 '.'。
display = "".join(ch if 32 <= ord(ch) < 127 else "." for ch in digest)

expected = hashlib.sha256(meta).hexdigest()
if digest != expected:
    print(f"FAIL .note.uab.sig digest 未回填或与 meta 不符\n"
          f"     段内值: {display}\n"
          f"     应为值: {expected}\n"
          f"     原因多为 ll-builder 版本过旧（需与 ll-cli 版本匹配）")
    raise SystemExit(0)

try:
    meta_json = json.loads(meta)
except ValueError as exc:
    print(f"FAIL linglong.meta 不是合法 JSON: {exc}")
    raise SystemExit(0)

bundle_digest = str(meta_json.get("digest", ""))
if len(bundle_digest) != 64:
    print(f"FAIL linglong.meta 缺少有效的 bundle digest: '{bundle_digest}'")
    raise SystemExit(0)

print(f"OK digest={expected[:16]}… bundle={bundle_digest[:16]}… version={meta_json.get('version')}")
PY
) || result="FAIL UAB 校验脚本执行失败（见上方 python 报错）"

    if [[ "$result" == OK* ]]; then
        log_info "UAB 签名段校验通过：${result#OK }"
        return 0
    fi

    log_error "UAB 签名段校验失败："
    echo "$result" | sed 's/^/  /'
    return 1
}

export_uab() {
    local project_root="$1"
    local output_dir="$2"
    local app_id="$3"
    local version="$4"
    local target_arch="$5"

    log_info "导出 UAB 格式包..."

    # Linyaps 会自动处理版本号，不需要我们添加四段式到输出文件名
    local linyaps_version="$version"
    if [[ "$linyaps_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        linyaps_version="${linyaps_version}.0"
    fi

    mkdir -p "$output_dir"

    # 导出 UAB 需要 --ref 参数
    # 必须使用绝对路径，ll-builder 不支持相对路径
    local temp_uab="${project_root}/output.uab"
    local final_uab="${output_dir}/${app_id}_${version}_${target_arch}.uab"
    local ref="main:${app_id}/${linyaps_version}/${target_arch}"

    # 确保使用绝对路径（ll-builder 要求）
    temp_uab=$(cd "$project_root" && pwd)/output.uab
    final_uab=$(cd "$output_dir" && pwd)/${app_id}_${version}_${target_arch}.uab
    output_dir=$(cd "$output_dir" && pwd)

    # 分别导出 UAB 和 layer，两者都需成功；UAB 缺失即判失败
    local uab_ok=false
    local layer_ok=false

    # ── 1) 导出 UAB ──
    if [ "$LAYER_ONLY" = "true" ]; then
        log_info "已指定 --layer-only：跳过 UAB 导出"
    else
        log_info "导出 UAB 格式包..."
        if ll-builder export --ref "$ref" -o "$temp_uab" && [ -s "$temp_uab" ]; then
            if ! verify_uab_digest "$temp_uab"; then
                log_error "UAB 已生成但签名段无效，判定导出失败（该包用 ll-cli 安装会被拒绝）"
                rm -f "$temp_uab"
                return 1
            fi
            mv "$temp_uab" "$final_uab"
            chmod 644 "$final_uab" 2>/dev/null || true
            log_info "UAB 包导出完成: $final_uab"
            copy_to_release "$final_uab"
            EXPORTED_UAB_FILE="$final_uab"
            uab_ok=true
        else
            log_error "UAB 导出失败"
            rm -f "$temp_uab"
            # 最常见的两类原因给出可执行的下一步，而不是只留一句 "code -1"。
            # 注意：构建日志是 build_linyaps() 写的，这里按路径读，不要引用那个局部变量。
            local build_log="${TEMP_DIR}/ll-builder-output.log"
            if grep -q "builder utils for target architecture" "$build_log" 2>/dev/null ||
               grep -q "builder-utils" "$build_log" 2>/dev/null; then
                log_info "  原因：本地仓库解析不到 cn.org.linyaps.builder.utils（ll-builder 1.14 要求 ≥ 0.0.4.0，"
                log_info "        而官方 stable 源目前只到 0.0.2.0）。可用 linglong 源码根目录的 linglong.yaml"
                log_info "        （版本号即 0.0.4.0）执行 ll-builder build 自建该层，再让其可被本机仓库解析。"
            fi
            log_info "  若暂时只需要可安装产物：用 --layer-only（layer 可直接 ll-cli install）"
        fi
    fi

    # ── 2) 导出 layer ──
    log_info "导出 layer 格式包..."
    if ll-builder export --layer; then
        local layer_file
        layer_file=$(find "$project_root" -maxdepth 1 -name "*_binary.layer" | head -1)
        if [ -n "$layer_file" ]; then
            local final_layer
            final_layer="$(cd "$output_dir" && pwd)/$(basename "$layer_file")"
            mv "$layer_file" "$final_layer"
            chmod 644 "$final_layer" 2>/dev/null || true
            log_info "layer 包导出完成: $final_layer"
            copy_to_release "$final_layer"
            EXPORTED_LAYER_FILE="$final_layer"
            layer_ok=true
        fi
    fi
    if [ "$layer_ok" = false ]; then
        log_error "layer 导出失败"
    fi

    # UAB 必须成功（layer-only 模式除外）；UAB 缺失即使 layer 已成功也判失败
    if [ "$uab_ok" = false ] && [ "$LAYER_ONLY" != "true" ]; then
        log_error "UAB 导出失败，linyaps 任务判定为失败（即使 layer 可能已成功）"
        return 1
    fi
    if [ "$LAYER_ONLY" = "true" ] && [ "$layer_ok" = false ]; then
        log_error "layer-only 模式但 layer 导出失败"
        return 1
    fi

    return 0
}

# ── 修复容器内构建产物的所有权 ────────────────────────────
# Docker 容器以 root 运行时产物属于 root，通过 chown 恢复为宿主用户
fix_output_ownership() {
    if [ -z "${HOST_UID:-}" ] || [ -z "${HOST_GID:-}" ]; then
        return 0
    fi

    # Non-root users cannot chown; just ensure permissions are sane
    if [ "$(id -u)" != "0" ]; then
        log_info "非 root 用户，确保构建产物权限可读写..."
        if [ -d "${OUTPUT_DIR}" ]; then
            chmod -R u+rwX "${OUTPUT_DIR}" 2>/dev/null || true
        fi
        if [ -d "${SCRIPT_DIR}/build/release" ]; then
            chmod -R u+rwX "${SCRIPT_DIR}/build/release" 2>/dev/null || true
        fi
        return 0
    fi

    # Root in privileged container: chown files to host user
    if [ "${HOST_UID}" != "0" ]; then
        log_info "修复构建产物所有权为宿主用户 (${HOST_UID}:${HOST_GID})..."
        if [ -d "${OUTPUT_DIR}" ]; then
            chown -R "${HOST_UID}:${HOST_GID}" "${OUTPUT_DIR}" 2>/dev/null || \
                chmod -R u+rwX,go+rX "${OUTPUT_DIR}" 2>/dev/null || true
        fi
        if [ -d "${SCRIPT_DIR}/build/release" ]; then
            chown -R "${HOST_UID}:${HOST_GID}" "${SCRIPT_DIR}/build/release" 2>/dev/null || \
                chmod -R u+rwX,go+rX "${SCRIPT_DIR}/build/release" 2>/dev/null || true
        fi
        # 修复 Linyaps 构建缓存所有权（Docker 内以 root 运行会留下 root-owned 文件，
        # 阻止后续容器复用缓存）
        local cache_dir
        for cache_dir in "${SCRIPT_DIR}/.tmp/linyaps/runtime" "${SCRIPT_DIR}/.cache/linglong"; do
            if [ -d "$cache_dir" ]; then
                chown -R "${HOST_UID}:${HOST_GID}" "$cache_dir" 2>/dev/null || \
                    chmod -R u+rwX,go+rX "$cache_dir" 2>/dev/null || true
            fi
        done
    fi
}

cleanup() {
    local keep_temp="$1"

    if [ "$keep_temp" != "true" ]; then
        log_clean "清理临时文件..."
        # Ensure temp files are writable before removal (ll-builder may create
        # files with restrictive permissions inside the container)
        if [ -d "${TEMP_DIR}" ]; then
            chmod -R u+rwX "${TEMP_DIR}" 2>/dev/null || true
            rm -rf "${TEMP_DIR}" || true
        fi
        log_clean "临时文件已清理"
    else
        log_info "保留临时文件: ${TEMP_DIR}"
    fi
}

show_summary() {
    local target_arch="$1"
    local uab_file="$2"
    local layer_file="$3"

    echo ""
    echo "=========================================="
    echo "Linyaps 构建完成！"
    echo "=========================================="
    echo ""
    echo "目标架构: $target_arch"
    echo "输出目录: $(dirname "$uab_file")/"
    echo ""
    echo "生成的 Linyaps 包:"
    ls -lh "$uab_file" 2>/dev/null || echo "  未找到 UAB 包"
    ls -lh "$layer_file" 2>/dev/null || echo "  未找到 layer 包"
    echo ""
    echo "安装命令:"
    if [ -n "$uab_file" ] && [ -f "$uab_file" ]; then
        echo "  ll-cli install $uab_file"
    fi
    echo ""
    echo "运行命令:"
    echo "  ll-cli run $APP_ID"
    echo ""
}

main() {
    local version=""
    local keep_temp="false"
    local target_arch=""
    local offline="false"

    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -v|--version)
                version="$2"
                shift 2
                ;;
            -a|--arch)
                target_arch="$2"
                shift 2
                ;;
            -k|--keep-temp)
                keep_temp="true"
                shift
                ;;
            -o|--offline)
                offline="true"
                OFFLINE_BUILD="true"
                shift
                ;;
            -r|--refresh)
                REFRESH_DEPS="true"
                shift
                ;;
            --layer-only)
                LAYER_ONLY="true"
                shift
                ;;
            --in-container)
                # 去 Docker 化后该参数没有意义；保留识别只为不让旧脚本/旧文档
                # 直接报"未知选项"失败，同时明确提示已废弃。
                log_warn "--in-container 已废弃（Linyaps 构建已改为宿主原生），该参数被忽略"
                shift
                ;;
            *)
                log_error "未知选项: $1"
                show_help
                exit 1
                ;;
        esac
    done

    echo "=========================================="
    echo "Ter-Music Linyaps 构建脚本"
    echo "=========================================="
    echo ""

    log_info "构建环境信息:"
    log_info "  操作系统: $(uname -s)"
    log_info "  内核版本: $(uname -r)"
    log_info "  主机架构: $(uname -m)"
    echo ""

    if [ -z "$version" ]; then
        version=$(detect_version)
        if [ "$version" != "1.0.0" ]; then
            log_info "从 Git/CMakeLists.txt 检测到版本: $version"
        else
            log_info "无法检测版本，使用默认版本: $version"
        fi
    else
        log_info "使用指定版本: $version"
    fi

    if [ -z "$target_arch" ]; then
        target_arch=$(detect_architecture)
        if [ $? -eq 0 ]; then
            log_info "自动检测到架构: $target_arch"
        else
            log_error "无法检测系统架构"
            exit 1
        fi
    else
        if ! validate_architecture "$target_arch"; then
            exit 1
        fi
        log_info "使用指定架构: $target_arch"
    fi
    
    check_dependencies "$target_arch"
    mkdir -p "${OUTPUT_DIR}/${target_arch}"

    if [ -d "${TEMP_DIR}" ]; then
        chmod -R u+rwX "${TEMP_DIR}" 2>/dev/null || true
        rm -rf "${TEMP_DIR}" || true
    fi
    mkdir -p "${TEMP_DIR}"
    # 调用函数，通过全局变量返回结果
    prepare_linyaps_structure "$TEMP_DIR" "$APP_ID"
    local project_root="$PROJECT_ROOT_OUTPUT"

    generate_linglong_yaml "$project_root" "$APP_ID" "$version" "$target_arch"

    if build_linyaps "$project_root"; then
        if export_uab "$project_root" "${OUTPUT_DIR}/${target_arch}" "$APP_ID" "$version" "$target_arch"; then
            fix_output_ownership
            cleanup "$keep_temp"
            show_summary "$target_arch" "${EXPORTED_UAB_FILE:-}" "${EXPORTED_LAYER_FILE:-}"
        else
            fix_output_ownership
            cleanup "$keep_temp"
            log_error "UAB 导出失败"
            exit 1
        fi
    else
        log_error "Linyaps 构建过程失败"
        cleanup "$keep_temp"
        exit 1
    fi
}

# OFFLINE_BUILD / REFRESH_DEPS 由 --offline / --refresh 设置，供 build_linyaps() 读取
OFFLINE_BUILD="${OFFLINE_BUILD:-false}"
REFRESH_DEPS="${REFRESH_DEPS:-false}"
LAYER_ONLY="${LAYER_ONLY:-false}"
USED_AUTO_OFFLINE="false"

# ll-builder 的本地层/依赖缓存（宿主原生构建直接用它，不再由 docker-build.sh 挂载）
LINYAPS_CACHE_DIR="${HOME}/.cache/linglong-builder"

# deepin 镜像默认 5 秒连接超时太短：实测在拉取 builder.utils 时直接超时失败
# （"failed to search remote packages from stable: Timeout was reached"）。
# Dockerfile 时代这个变量由 docker run -e 注入，去 Docker 化后必须在脚本里给。
export LINGLONG_CONNECT_TIMEOUT="${LINGLONG_CONNECT_TIMEOUT:-120}"

# 本地缓存是否已具备 base/runtime：ll-builder 用 states.json 记录已缓存的层
linyaps_cache_ready() {
    [ -s "${LINYAPS_CACHE_DIR}/states.json" ]
}

main "$@"
