#!/usr/bin/env bash
#
# 每个 EL（7/8/9/10）一个 RPM；仅 FFmpeg 静态。
# 默认只出二进制 RPM；--with-source / --with-debuginfo 按需追加。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=common.sh
. "${SCRIPT_DIR}/scripts/build/common.sh"

PACKAGE_NAME="cn.org.huansoft.ter-music"
REPO_ROOT="$SCRIPT_DIR"

show_help() {
    cat <<EOF
用法: $0 -v VERSION [选项]

构建 ter-music 的 RPM（EL7/8/9/10 容器，仅 FFmpeg 静态）。

选项:
  -v, --version VERSION   必填；三段自动补 .0 为四段
  -a, --arch ARCH         仅支持 x86_64（默认 x86_64）
      --el LIST           目标 EL：7,8,9,10 或 all（默认 all）
  -k, --keep-temp         保留 .tmp/rpm
      --with-source       额外生成 .src.rpm
      --with-debuginfo    额外生成 -debuginfo/-debugsource rpm
      --rebuild-image     强制重建对应 EL 镜像
      --no-cache          docker build 不使用缓存
  -h, --help              显示帮助

输出:
  二进制: build/rpm/el<N>/x86_64/
  源码:   build/rpm/el<N>/source/（仅 --with-source）
EOF
}

VERSION=""
ARCH="x86_64"
EL_LIST="all"
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
        --el) EL_LIST="${2:-}"; shift 2 ;;
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
if [ "$ARCH" != "x86_64" ]; then
    tm_log_error "unsupported architecture: $ARCH (only x86_64)"
    exit 1
fi

if [ -z "$EL_LIST" ]; then
    tm_log_error "--el requires a value (7,8,9,10 or all)"
    exit 1
fi
if [ "$EL_LIST" = "all" ]; then
    EL_LIST="7,8,9,10"
fi
IFS=',' read -r -a EL_ARRAY <<< "$EL_LIST"
if [ "${#EL_ARRAY[@]}" -eq 0 ]; then
    tm_log_error "no EL target selected"
    exit 1
fi
for el in "${EL_ARRAY[@]}"; do
    case "$el" in
        7|8|9|10) ;;
        *) tm_log_error "unsupported EL version: $el"; exit 1 ;;
    esac
done

# 宿主机入口：每个 EL 委托一次 Docker。
if ! tm_is_container; then
    for el in "${EL_ARRAY[@]}"; do
        if [ "$el" = "7" ]; then
            base_image="centos:7"
        else
            base_image="rockylinux:${el}"
        fi
        xb_args=(
            -f scripts/docker/Dockerfile.rpm
            -n "ter-music-rpm-el${el}"
            -s build-rpm.sh
            --build-arg "BASE_IMAGE=${base_image}"
            --build-arg "EL_VERSION=${el}"
        )
        [ "$REBUILD_IMAGE" = true ] && xb_args+=(--rebuild)
        [ "$NO_CACHE" = true ] && xb_args+=(--no-cache)
        inner=(-v "$VERSION" -a "$ARCH" --el "$el")
        [ "$KEEP_TEMP" = true ] && inner+=(-k)
        [ "$WITH_SOURCE" = true ] && inner+=(--with-source)
        [ "$WITH_DEBUGINFO" = true ] && inner+=(--with-debuginfo)
        "${REPO_ROOT}/scripts/docker/docker-build.sh" "${xb_args[@]}" -- "${inner[@]}"
    done
    exit 0
fi

for el in "${EL_ARRAY[@]}"; do
    tm_log_info "RPM build: version=${VERSION} el=${el} source=${WITH_SOURCE} debug=${WITH_DEBUGINFO}"

    WORK="${REPO_ROOT}/.tmp/rpm/el${el}"
    RPMBUILD="${WORK}/rpmbuild"
    SRC="${WORK}/src/${PACKAGE_NAME}-${VERSION}"
    OUT_BIN="${REPO_ROOT}/build/rpm/el${el}/x86_64"
    OUT_SRC="${REPO_ROOT}/build/rpm/el${el}/source"

    rm -rf "$WORK"
    mkdir -p "$RPMBUILD"/{BUILD,RPMS,SOURCES,SPECS,SRPMS} "$OUT_BIN" "$OUT_SRC"

    tm_stage_source "$SRC"
    archive="${RPMBUILD}/SOURCES/${PACKAGE_NAME}-${VERSION}.tar.gz"
    tar -czf "$archive" -C "$(dirname "$SRC")" "$(basename "$SRC")"
    cp -f "${REPO_ROOT}/rpm/${PACKAGE_NAME}.spec" "${RPMBUILD}/SPECS/"

    rpm_args=(
        --define "_topdir ${RPMBUILD}"
        --define "_sourcedir ${RPMBUILD}/SOURCES"
        --define "_specdir ${RPMBUILD}/SPECS"
        --define "_builddir ${RPMBUILD}/BUILD"
        --define "_rpmdir ${RPMBUILD}/RPMS"
        --define "_srcrpmdir ${RPMBUILD}/SRPMS"
        --define "_version ${VERSION}"
        --target "$ARCH"
    )
    if [ "$WITH_DEBUGINFO" = true ]; then
        rpm_args+=(--define "_tm_build_type RelWithDebInfo")
    else
        rpm_args+=(--define "debug_package %{nil}")
    fi
    if [ "$WITH_SOURCE" = true ]; then
        rpm_args+=(-ba)
    else
        rpm_args+=(-bb)
    fi
    rpm_args+=("${RPMBUILD}/SPECS/${PACKAGE_NAME}.spec")

    if ! rpmbuild "${rpm_args[@]}"; then
        tm_log_error "rpmbuild failed for el${el}"
        exit 1
    fi

    found_bin=0
    found_dbg=0
    while IFS= read -r -d '' rpm; do
        name="$(basename "$rpm")"
        case "$name" in
            *-debuginfo-*|*-debugsource-*)
                if [ "$WITH_DEBUGINFO" = true ]; then
                    cp -f "$rpm" "$OUT_BIN/"
                    tm_copy_release "$rpm"
                    found_dbg=$((found_dbg + 1))
                fi
                ;;
            *)
                cp -f "$rpm" "$OUT_BIN/"
                tm_copy_release "$rpm"
                found_bin=$((found_bin + 1))
                ;;
        esac
    done < <(find "${RPMBUILD}/RPMS" -name '*.rpm' -type f -print0)

    if [ "$found_bin" -eq 0 ]; then
        tm_log_error "no binary rpm produced for el${el}"
        exit 1
    fi
    if [ "$WITH_DEBUGINFO" = true ] && [ "$found_dbg" -eq 0 ]; then
        tm_log_error "--with-debuginfo requested but no debuginfo/debugsource rpm was produced"
        exit 1
    fi

    if [ "$WITH_SOURCE" = true ]; then
        found_src=0
        while IFS= read -r -d '' srpm; do
            cp -f "$srpm" "$OUT_SRC/"
            tm_copy_release "$srpm"
            found_src=$((found_src + 1))
        done < <(find "${RPMBUILD}/SRPMS" -name '*.src.rpm' -type f -print0)
        if [ "$found_src" -eq 0 ]; then
            tm_log_error "--with-source requested but no src.rpm was produced for el${el}"
            exit 1
        fi
    fi

    main_rpm="$(find "$OUT_BIN" -maxdepth 1 -name "${PACKAGE_NAME}-${VERSION}-1.el${el}.${ARCH}.rpm" -type f | head -n 1)"
    if [ -z "$main_rpm" ]; then
        main_rpm="$(find "$OUT_BIN" -maxdepth 1 -name "${PACKAGE_NAME}-${VERSION}-1*.${ARCH}.rpm" -type f ! -name '*-debug*' | head -n 1)"
    fi
    if [ -z "$main_rpm" ]; then
        tm_log_error "main rpm not found in $OUT_BIN"
        exit 1
    fi
    requires="$(rpm -qpR "$main_rpm")"
    if ! printf '%s\n' "$requires" | grep -q '^libxml2'; then
        tm_log_error "main rpm is missing explicit libxml2 requirement"
        exit 1
    fi
    if printf '%s\n' "$requires" | grep -q 'libav'; then
        tm_log_error "main rpm has libav* runtime dependency (FFmpeg is not static)"
        exit 1
    fi
    if ! rpm -qpl "$main_rpm" | grep -q '/ter-music$'; then
        tm_log_error "main rpm does not contain /usr/bin/ter-music"
        exit 1
    fi

    if [ "$KEEP_TEMP" != true ]; then
        rm -rf "$WORK"
    fi

    echo ""
    echo "=========================================="
    echo "RPM el${el} 构建完成"
    echo "=========================================="
    echo "二进制: ${OUT_BIN}/"
    ls -lh "$OUT_BIN"/*.rpm 2>/dev/null || true
    if [ "$WITH_SOURCE" = true ]; then
        echo "源码: ${OUT_SRC}/"
        ls -lh "$OUT_SRC"/*.src.rpm 2>/dev/null || true
    fi
done

tm_fix_ownership
