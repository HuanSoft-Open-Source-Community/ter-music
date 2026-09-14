#!/usr/bin/env bash
#
# ter-music D-Bus RPC 接口面单点验证（里程碑 M2）
#
# 验证内容：
#   1. 自省 XML 中 org.yxzl.ter_music.* 各接口及方法齐备；
#   2. Info.GetInfo 的 core.methods 声明与实际发布的方法一致（防漂移）；
#   3. 各接口的 schema、分页边界与超大响应拒绝（随 M2 逐步补充）；
#   4. 退出时不留残留进程。
#
# 用法：
#   scripts/test/dbus-rpc-check.sh [--bin <ter-music>] [--keep]
#
# 环境：需要会话总线；未设置 DBUS_SESSION_BUS_ADDRESS 时自动用
#       dbus-run-session 起一条私有总线。需要 ffmpeg 生成测试音频。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""
DAEMON_PID=""

PASS=0
FAIL=0

# M2 完成后应存在的接口（随里程碑推进逐个加入本清单）
EXPECTED_INTERFACES=(
    "org.yxzl.ter_music.Lyrics"
    "org.yxzl.ter_music.Info"
    "org.yxzl.ter_music.Control"
    "org.yxzl.ter_music.Playlist"
    "org.yxzl.ter_music.Queue"
    "org.yxzl.ter_music.Library"
    "org.yxzl.ter_music.Favorites"
    "org.yxzl.ter_music.History"
    "org.yxzl.ter_music.DirHistory"
    "org.yxzl.ter_music.Config"
    "org.yxzl.ter_music.Remote"
)

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage; exit 2 ;;
    esac
done

ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

# ── 复用总线：没有会话总线时用 dbus-run-session 重入 ───────────────
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：没有会话总线，且未安装 dbus-run-session。" >&2
        exit 2
    fi
    exec dbus-run-session -- "$0" "$@"
fi

cleanup() {
    # 显式停止实例；用记录下来的 PID 兜底，避免 pgrep 匹配到本脚本自身
    if [ -n "$DAEMON_PID" ]; then
        "$TM_BIN" daemon stop >/dev/null 2>&1 || true
        for _ in $(seq 1 20); do
            kill -0 "$DAEMON_PID" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "$DAEMON_PID" 2>/dev/null || true
    fi
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

# ── 准备隔离环境与测试素材 ─────────────────────────────────────────
setup_fixtures() {
    WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-rpc-XXXXXX")"
    mkdir -p "$WORK_DIR/music" "$WORK_DIR/home"

    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -hide_banner -loglevel error -f lavfi -i anullsrc=r=44100:cl=mono \
            -t 3 -c:a pcm_s16le -y "$WORK_DIR/music/a.wav" 2>/dev/null
        ffmpeg -hide_banner -loglevel error -f lavfi -i anullsrc=r=44100:cl=mono \
            -t 3 -c:a pcm_s16le -y "$WORK_DIR/music/b.wav" 2>/dev/null
    fi

    export HOME="$WORK_DIR/home"
    export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
    export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
}

start_daemon() {
    local pid
    pid="$("$TM_BIN" daemon start --open "$WORK_DIR/music" 2>/dev/null |
           sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
    if [ -z "$pid" ]; then
        # daemon start 的输出格式变化时退回按总线名查询
        pid="$("$TM_BIN" daemon status 2>/dev/null | sed -n 's/.*pid \([0-9]\+\).*/\1/p')"
    fi
    DAEMON_PID="$pid"
    [ -n "$DAEMON_PID" ]
}

# python 客户端：gdbus/busctl 无法传负整数参数，也无法保持连接
rpc_py() {
    timeout 20 python3 "$SCRIPT_DIR/rpc_client.py" "$@" 2>&1
}

dbus_call() {
    local iface="$1" method="$2"; shift 2
    timeout 10 gdbus call --session \
        --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 \
        --method "$iface.$method" "$@" 2>&1
}

introspect() {
    timeout 10 gdbus introspect --session \
        --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 2>&1
}

# 同上，但保留退出码供调用方判断成败
introspect_rc() {
    local out
    out="$(timeout 10 gdbus introspect --session \
        --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 2>&1)"
    local rc=$?
    printf '%s' "$out"
    return $rc
}

# ── 检查 1：接口与方法齐备 ─────────────────────────────────────────
check_interfaces() {
    local xml
    xml="$(introspect_rc)"
    if [ $? -ne 0 ]; then
        bad "自省失败：$(printf '%s' "$xml" | head -1)"
        return
    fi
    for iface in "${EXPECTED_INTERFACES[@]}"; do
        if printf '%s' "$xml" | grep -q "interface $iface"; then
            ok "接口存在 $iface"
        else
            bad "接口缺失 $iface"
        fi
    done
}

# ── 检查 2：声明的方法清单与实际发布一致 ───────────────────────────
check_method_list() {
    local json
    json="$(dbus_call org.yxzl.ter_music.Info GetInfo)"
    if ! printf '%s' "$json" | grep -q '"api_version"'; then
        echo "  skip core.methods 尚未实现（M2.2 起生效）"
        return
    fi

    local declared
    declared="$(printf '%s' "$json" | python3 -c '
import ast, json, sys
raw = sys.stdin.read().strip()
try:
    doc = json.loads(ast.literal_eval(raw)[0])
except Exception as exc:
    print("JSON-PARSE-ERROR", exc)
    raise SystemExit(0)
methods = doc.get("core", {}).get("methods")
if not isinstance(methods, list):
    print("NO-METHODS")
    raise SystemExit(0)
print("\n".join(methods))
')"

    if [ "$declared" = "NO-METHODS" ] || [ -z "$declared" ]; then
        bad "core.methods 缺失或不是数组"
        return
    fi
    case "$declared" in
        JSON-PARSE-ERROR*) bad "GetInfo 返回的不是合法 JSON：$declared"; return ;;
    esac

    local xml
    xml="$(introspect)"
    local checked=0 missing=0
    while IFS= read -r method; do
        [ -z "$method" ] && continue
        checked=$((checked + 1))
        local member="${method##*.}"
        # gdbus introspect 把方法打印为裸签名行（"    Name(args);"），
        # 不带 method 前缀，因此按“行首/非标识符字符 + 名字 + (”匹配
        if ! printf '%s' "$xml" | grep -qE "(^|[^A-Za-z_])$member\\("; then
            bad "core.methods 声明了未实现的方法 $method"
            missing=$((missing + 1))
        fi
    done <<< "$declared"

    if [ "$missing" -eq 0 ]; then
        ok "core.methods 与实际发布一致（$checked 个方法）"
    fi
}


# ── 检查 3：Info 扩展（core 握手对象、可视化、状态、封面字符集） ──
json_field() {
    # json_field <json> <python 表达式>，用于在 shell 里取 JSON 字段
    printf '%s' "$1" | python3 -c '
import ast, json, sys
raw = sys.stdin.read().strip()
try:
    doc = json.loads(ast.literal_eval(raw)[0])
except Exception as exc:
    print("ERR:%s" % exc)
    raise SystemExit(0)
try:
    print(eval(sys.argv[1], {"doc": doc}))
except Exception as exc:
    print("ERR:%s" % exc)
' "$2"
}

check_info_extensions() {
    local info api_version
    info="$(dbus_call org.yxzl.ter_music.Info GetInfo)"
    api_version="$(json_field "$info" 'doc["core"]["api_version"]')"
    if [ "$api_version" = "2" ]; then
        ok "core.api_version=2（握手版本）"
    else
        bad "core.api_version 期望 2，实际 '$api_version'"
    fi

    local methods
    methods="$(json_field "$info" 'len(doc["core"]["methods"])')"
    case "$methods" in
        ''|ERR:*) bad "core.methods 缺失：$methods" ;;
        *) ok "core.methods 声明 $methods 个方法" ;;
    esac

    # GetVisualizer：bands 数量与数组长度必须一致
    local viz bands levels
    viz="$(dbus_call org.yxzl.ter_music.Info GetVisualizer)"
    bands="$(json_field "$viz" 'doc["bands"]')"
    levels="$(json_field "$viz" 'len(doc["levels"])')"
    if [ "$bands" = "$levels" ] && [ -n "$bands" ]; then
        ok "GetVisualizer bands=$bands 与 levels 长度一致"
    else
        bad "GetVisualizer bands=$bands levels=$levels 不一致"
    fi

    # GetStatus：seq 为整数且 message 为字符串
    local status seq
    status="$(dbus_call org.yxzl.ter_music.Info GetStatus)"
    seq="$(json_field "$status" 'doc["seq"]')"
    case "$seq" in
        ''|ERR:*) bad "GetStatus 缺少 seq：$seq" ;;
        *) ok "GetStatus seq=$seq" ;;
    esac

    # GetCoverArt：非法字符集必须被拒绝
    local err
    err="$(dbus_call org.yxzl.ter_music.Info GetCoverArt bogus 8 4)"
    if printf '%s' "$err" | grep -q "InvalidArgs"; then
        ok "GetCoverArt 拒绝未知字符集"
    else
        bad "GetCoverArt 未拒绝未知字符集：$(printf '%s' "$err" | head -1)"
    fi

    # GetCoverArt：三种字符集都要能用（无封面时返回空串也算通过）
    local charset out
    for charset in braille ascii half; do
        out="$(dbus_call org.yxzl.ter_music.Info GetCoverArt "$charset" 8 4)"
        if printf '%s' "$out" | grep -q "^(\|('"; then
            ok "GetCoverArt charset=$charset 可调用"
        else
            bad "GetCoverArt charset=$charset 调用失败：$(printf '%s' "$out" | head -1)"
        fi
    done
}


# ── 检查 4：前端注册表（Attach/Ping/Detach/FrontendInfo） ───────────
check_frontends() {
    local attached token
    attached="$(dbus_call org.yxzl.ter_music.Control Attach tui)"
    token="$(json_field "$attached" 'doc["token"]')"
    if [ -z "$token" ] || [ "${token#ERR:}" != "$token" ]; then
        bad "Attach 未返回 token：$(printf '%s' "$attached" | head -1)"
        return
    fi
    ok "Attach 返回 token=$token"

    local version
    version="$(json_field "$attached" 'doc["api_version"]')"
    [ "$version" = "2" ] && ok "Attach 返回 api_version=2" || bad "Attach api_version=$version"

    # 第二个前端
    local attached2
    attached2="$(dbus_call org.yxzl.ter_music.Control Attach cli)"
    local token2
    token2="$(json_field "$attached2" 'doc["token"]')"

    local list count
    list="$(dbus_call org.yxzl.ter_music.Control FrontendInfo)"
    count="$(json_field "$list" 'doc["count"]')"
    if [ "$count" -ge 2 ] 2>/dev/null; then
        ok "FrontendInfo 列出 $count 个前端"
    else
        bad "FrontendInfo count=$count（期望 >=2）"
    fi

    # 角色校验
    local bad_role
    bad_role="$(dbus_call org.yxzl.ter_music.Control Attach robot)"
    if printf '%s' "$bad_role" | grep -q "InvalidArgs"; then
        ok "Attach 拒绝非法 role"
    else
        bad "Attach 未拒绝非法 role"
    fi

    # 未知 token 的 Ping 返回 false（前端应重新 Attach）
    local ping_unknown
    ping_unknown="$(dbus_call org.yxzl.ter_music.Control Ping ":1.99999")"
    if printf '%s' "$ping_unknown" | grep -q "false"; then
        ok "Ping 对未知 token 返回 false"
    else
        bad "Ping 对未知 token 返回：$(printf '%s' "$ping_unknown" | head -1)"
    fi

    # 已知 token 的 Ping 返回 true
    local ping_known
    ping_known="$(dbus_call org.yxzl.ter_music.Control Ping "$token")"
    if printf '%s' "$ping_known" | grep -q "true"; then
        ok "Ping 刷新已登记 token"
    else
        bad "Ping 已知 token 返回：$(printf '%s' "$ping_known" | head -1)"
    fi

    # Detach 后不再列出
    dbus_call org.yxzl.ter_music.Control Detach "$token" >/dev/null
    dbus_call org.yxzl.ter_music.Control Detach "$token2" >/dev/null
    list="$(dbus_call org.yxzl.ter_music.Control FrontendInfo)"
    count="$(json_field "$list" 'doc["count"]')"
    if [ "$count" = "0" ]; then
        ok "Detach 后注册表清空"
    else
        bad "Detach 后仍有 $count 个前端"
    fi

    # 心跳超时：登记后不再 Ping，超过 RPC_FRONTEND_TIMEOUT_MS(6s) 应被清除
    attached="$(dbus_call org.yxzl.ter_music.Control Attach app)"
    token="$(json_field "$attached" 'doc["token"]')"
    local waited=0
    while [ "$waited" -lt 9 ]; do
        sleep 1
        waited=$((waited + 1))
        count="$(json_field "$(dbus_call org.yxzl.ter_music.Control FrontendInfo)" 'doc["count"]')"
        [ "$count" = "0" ] && break
    done
    if [ "$count" = "0" ]; then
        ok "无心跳 ${waited}s 后被判定离开"
    else
        bad "无心跳 ${waited}s 后仍登记着（count=$count）"
    fi
}


# ── 检查 5：Playlist / Queue（分页边界、过滤、队列编辑） ─────────────
check_playlist_queue() {
    local tree total
    tree="$(dbus_call org.yxzl.ter_music.Playlist GetTree)"
    total="$(json_field "$tree" 'doc["count"]')"
    case "$total" in
        ''|ERR:*) bad "GetTree 失败：$tree" ; return ;;
        *) ok "GetTree count=$total" ;;
    esac

    # 分页：offset 越界返回 0 行而不是报错
    local page count
    page="$(dbus_call org.yxzl.ter_music.Playlist GetPage 100000 10)"
    count="$(json_field "$page" 'doc["count"]')"
    [ "$count" = "0" ] && ok "GetPage 越界 offset 返回空页" || bad "GetPage 越界返回 count=$count"

    # 分页：count 超上限应被拒绝
    local over
    over="$(dbus_call org.yxzl.ter_music.Playlist GetPage 0 5000)"
    if printf '%s' "$over" | grep -q "InvalidArgs"; then
        ok "GetPage 拒绝超过上限的 count（1000）"
    else
        bad "GetPage 未拒绝超大 count"
    fi

    # 过滤：SetFilter 后 GetPage 反映过滤结果，清空后恢复。
    # 注意 GetTree.count 是曲目数，GetPage.total 是可见行数（树模式含目录行），
    # 因此用同一接口的前后对比，而不是与 GetTree 比较。
    local baseline
    baseline="$(json_field "$(dbus_call org.yxzl.ter_music.Playlist GetPage 0 10)" 'doc["total"]')"
    dbus_call org.yxzl.ter_music.Playlist SetFilter "a" >/dev/null
    local filtered_n
    filtered_n="$(json_field "$(dbus_call org.yxzl.ter_music.Playlist GetPage 0 10)" 'doc["total"]')"
    dbus_call org.yxzl.ter_music.Playlist SetFilter "" >/dev/null
    local restored
    restored="$(json_field "$(dbus_call org.yxzl.ter_music.Playlist GetPage 0 10)" 'doc["total"]')"
    if [ "$restored" = "$baseline" ] && [ "$filtered_n" -lt "$baseline" ]; then
        ok "SetFilter 生效（$filtered_n < $baseline 行）并可清空恢复"
    else
        bad "SetFilter 行为异常：baseline=$baseline filtered=$filtered_n restored=$restored"
    fi

    # 队列：Get 与写操作
    local queue qcount
    queue="$(dbus_call org.yxzl.ter_music.Queue Get 0 200)"
    qcount="$(json_field "$queue" 'doc["count"]')"
    case "$qcount" in
        ''|ERR:*) bad "Queue.Get 失败：$queue"; return ;;
        *) ok "Queue.Get count=$qcount" ;;
    esac

    local rc
    rc="$(dbus_call org.yxzl.ter_music.Queue Append 0)"
    printf '%s' "$rc" | grep -q "true" && ok "Queue.Append 接受合法曲目" || bad "Queue.Append 失败：$rc"

    rc="$(dbus_call org.yxzl.ter_music.Queue Append 999999)"
    printf '%s' "$rc" | grep -q "OutOfRange" && ok "Queue.Append 拒绝越界曲目" || bad "Queue.Append 未拒绝越界：$rc"

    rc="$(dbus_call org.yxzl.ter_music.Queue MoveUp 999999)"
    printf '%s' "$rc" | grep -q "OutOfRange" && ok "Queue.MoveUp 拒绝越界位置" || bad "Queue.MoveUp 未拒绝越界：$rc"

    # 队列变更信号
    gdbus monitor --session --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 > "$WORK_DIR/queue-monitor.txt" 2>&1 &
    local monitor_pid=$!
    sleep 0.8
    dbus_call org.yxzl.ter_music.Queue Append 0 >/dev/null
    sleep 0.8
    kill "$monitor_pid" 2>/dev/null
    if grep -q "QueueChanged" "$WORK_DIR/queue-monitor.txt"; then
        ok "队列写操作广播 QueueChanged"
    else
        bad "未捕获 QueueChanged 信号"
    fi
}


# ── 检查 6：曲库与收藏/历史（分页、搜索、序列化） ────────────────────
check_library() {
    local status available tracks
    status="$(dbus_call org.yxzl.ter_music.Library Status)"
    available="$(json_field "$status" 'doc["available"]')"
    tracks="$(json_field "$status" 'doc["tracks"]')"
    case "$available" in
        ''|ERR:*) bad "Library.Status 失败：$status"; return ;;
        *) ok "Library.Status available=$available tracks=$tracks" ;;
    esac

    # 按扫描根的路径重扫一次，验证异步扫描与进度上报
    local root="$WORK_DIR/music"
    dbus_call org.yxzl.ter_music.Library Rescan "$root" >/dev/null
    local waited=0 scanned=0
    while [ "$waited" -lt 10 ]; do
        sleep 1
        waited=$((waited + 1))
        scanned="$(json_field "$(dbus_call org.yxzl.ter_music.Library Status)" 'doc["tracks"]')"
        [ "${scanned:-0}" -gt 0 ] 2>/dev/null && break
    done
    if [ "${scanned:-0}" -gt 0 ] 2>/dev/null; then
        ok "Library.Rescan 编入 $scanned 首曲目（${waited}s）"
    else
        bad "Library.Rescan 后 tracks=$scanned"
    fi

    # 空路径必须被拒绝（全根扫描是阻塞调用，未暴露）
    local empty_rc
    empty_rc="$(dbus_call org.yxzl.ter_music.Library Rescan "")"
    printf '%s' "$empty_rc" | grep -q "InvalidArgs" && ok "Rescan 拒绝空路径" || bad "Rescan 未拒绝空路径"

    # 非法视图名
    local bad_kind
    bad_kind="$(dbus_call org.yxzl.ter_music.Library GetPage bogus "" 0 10)"
    printf '%s' "$bad_kind" | grep -q "InvalidArgs" && ok "GetPage 拒绝未知 kind" || bad "GetPage 未拒绝未知 kind"

    # 分页与总数一致
    local page count total
    page="$(dbus_call org.yxzl.ter_music.Library GetPage tracks "" 0 10)"
    count="$(json_field "$page" 'doc["count"]')"
    total="$(json_field "$page" 'doc["total"]')"
    if [ "${count:-0}" -le "${total:-0}" ] 2>/dev/null; then
        ok "曲库分页 count=$count total=$total"
    else
        bad "曲库分页 count=$count > total=$total"
    fi

    # 搜索（filter JSON）
    local search_n
    search_n="$(json_field "$(dbus_call org.yxzl.ter_music.Library Search '{"query":"a"}')" 'doc["item_count"]')"
    case "$search_n" in
        ''|ERR:*) bad "Library.Search 失败：$search_n" ;;
        *) ok "Library.Search 命中 $search_n 条" ;;
    esac

    # 收藏往返：Add → Has → List → Remove
    local fav_path="$WORK_DIR/music/a.wav"
    dbus_call org.yxzl.ter_music.Favorites Add "$fav_path" >/dev/null
    local has
    has="$(dbus_call org.yxzl.ter_music.Favorites Has "$fav_path")"
    printf '%s' "$has" | grep -q "true" && ok "Favorites.Add 后可 Has" || bad "Favorites.Has 返回 $has"
    local fav_total
    fav_total="$(json_field "$(dbus_call org.yxzl.ter_music.Favorites List 0 10)" 'doc["total"]')"
    [ "${fav_total:-0}" -ge 1 ] 2>/dev/null && ok "Favorites.List total=$fav_total" || bad "Favorites.List total=$fav_total"
    dbus_call org.yxzl.ter_music.Favorites Remove "$fav_path" >/dev/null

    # 历史往返：Add → List → Clear → 空
    dbus_call org.yxzl.ter_music.History Add "$fav_path" 0 >/dev/null
    local hist_total
    hist_total="$(json_field "$(dbus_call org.yxzl.ter_music.History List 0 10)" 'doc["total"]')"
    dbus_call org.yxzl.ter_music.History Clear >/dev/null
    local hist_after
    hist_after="$(json_field "$(dbus_call org.yxzl.ter_music.History List 0 10)" 'doc["total"]')"
    if [ "${hist_total:-0}" -ge 1 ] && [ "${hist_after:-1}" = "0" ]; then
        ok "History Add/List/Clear 往返正常（$hist_total → 0）"
    else
        bad "History 往返异常：add=$hist_total after_clear=$hist_after"
    fi

    # 目录历史往返
    dbus_call org.yxzl.ter_music.DirHistory Add "$WORK_DIR/music" >/dev/null
    local dir_total
    dir_total="$(json_field "$(dbus_call org.yxzl.ter_music.DirHistory List 0 10)" 'doc["total"]')"
    dbus_call org.yxzl.ter_music.DirHistory Clear >/dev/null
    local dir_after
    dir_after="$(json_field "$(dbus_call org.yxzl.ter_music.DirHistory List 0 10)" 'doc["total"]')"
    if [ "${dir_total:-0}" -ge 1 ] && [ "${dir_after:-1}" = "0" ]; then
        ok "DirHistory Add/List/Clear 往返正常（$dir_total → 0）"
    else
        bad "DirHistory 往返异常：add=$dir_total after_clear=$dir_after"
    fi
}


# ── 检查 7：Config / Remote ─────────────────────────────────────────
check_config_remote() {
    local all volume
    all="$(rpc_py call org.yxzl.ter_music.Config.GetAll)"
    volume="$(printf '%s' "$all" | python3 -c '
import json,sys
try:
    doc = json.load(sys.stdin)
    print(doc["preferences"]["volume_percent"])
except Exception as exc:
    print("ERR:%s" % exc)
')"
    case "$volume" in
        ''|ERR:*) bad "Config.GetAll 失败：$volume" ;;
        *) ok "Config.GetAll 可读（volume_percent=$volume）" ;;
    esac

    # 局部设置：落盘 + 运行时生效
    rpc_py call org.yxzl.ter_music.Config.Set 'json:{"preferences":{"volume_percent":37,"default_playback_speed":1.75}}' >/dev/null
    local applied
    applied="$(rpc_py call org.yxzl.ter_music.Control.GetSpeed)"
    local stored
    stored="$(rpc_py call org.yxzl.ter_music.Config.GetAll | python3 -c '
import json,sys
doc = json.load(sys.stdin)
print("%s/%s" % (doc["preferences"]["volume_percent"], doc["preferences"]["default_playback_speed"]))
')"
    if [ "$stored" = "37/1.75" ] && printf '%s' "$applied" | grep -q "1.75"; then
        ok "Config.Set 局部生效并落盘（volume/speed=$stored，运行时 speed=$applied）"
    else
        bad "Config.Set 异常：stored=$stored applied=$applied"
    fi

    # 未知键整体失败且不生效
    local before after
    before="$(rpc_py call org.yxzl.ter_music.Config.GetAll | python3 -c 'import json,sys; print(json.load(sys.stdin)["preferences"]["volume_percent"])')"
    local unknown_rc
    unknown_rc="$(rpc_py call org.yxzl.ter_music.Config.Set 'json:{"preferences":{"volume_percent":91,"nope":1}}')"
    after="$(rpc_py call org.yxzl.ter_music.Config.GetAll | python3 -c 'import json,sys; print(json.load(sys.stdin)["preferences"]["volume_percent"])')"
    if printf '%s' "$unknown_rc" | grep -q "Unsupported" && [ "$before" = "$after" ]; then
        ok "未知键被拒绝且整体不生效（volume 仍为 $after）"
    else
        bad "未知键处理异常：rc=$unknown_rc before=$before after=$after"
    fi

    # 越界值钳制
    rpc_py call org.yxzl.ter_music.Config.Set 'json:{"preferences":{"volume_percent":999}}' >/dev/null
    local clamped
    clamped="$(rpc_py call org.yxzl.ter_music.Config.GetAll | python3 -c 'import json,sys; print(json.load(sys.stdin)["preferences"]["volume_percent"])')"
    [ "$clamped" = "100" ] && ok "越界值被钳制到 100" || bad "钳制失败：$clamped"

    # Remote：新增服务器 + 密码不回明文
    rpc_py call org.yxzl.ter_music.Remote.SaveServer -1 \
        'json:{"name":"t","protocol":"sftp","host":"127.0.0.1","port":2222,"password":"s3cret"}' >/dev/null
    local servers leak
    servers="$(rpc_py call org.yxzl.ter_music.Remote.ListServers)"
    leak="$(rpc_py call org.yxzl.ter_music.Config.GetAll | python3 -c '
import json,sys
doc = json.load(sys.stdin)
rc = doc["remote_connections"][0]
print("%s|%s|%s" % (rc["password_set"], "password" in rc, bool(rc["password_encrypted"])))
')"
    if printf '%s' "$servers" | grep -q '"count":1' && [ "$leak" = "True|False|True" ]; then
        ok "Remote.SaveServer 生效，密码仅以密文回传（$leak）"
    else
        bad "Remote 密码处理异常：servers=$servers leak=$leak"
    fi

    local bad_proto
    bad_proto="$(rpc_py call org.yxzl.ter_music.Remote.SaveServer -1 'json:{"name":"x","protocol":"gopher","host":"h"}')"
    printf '%s' "$bad_proto" | grep -q "InvalidArgs" && ok "Remote 拒绝未知协议" || bad "Remote 未拒绝未知协议"

    rpc_py call org.yxzl.ter_music.Remote.DeleteServer 0 >/dev/null
    local remaining
    remaining="$(rpc_py call org.yxzl.ter_music.Remote.ListServers | python3 -c 'import json,sys; print(json.load(sys.stdin)["count"])')"
    [ "$remaining" = "0" ] && ok "Remote.DeleteServer 生效" || bad "删除后仍有 $remaining 条"

    # 前端注册表（保持连接）
    (rpc_py attach tui 5 >/dev/null 2>&1 &)
    sleep 1
    local frontends
    frontends="$(rpc_py call org.yxzl.ter_music.Control.FrontendInfo | python3 -c 'import json,sys; print(json.load(sys.stdin)["count"])')"
    [ "${frontends:-0}" -ge 1 ] 2>/dev/null && ok "长连接前端登记成功（count=$frontends）" || bad "长连接前端未登记：$frontends"

    # 信号：Config.Set 应同时广播 ConfigChanged 与 StatusMessage
    (rpc_py monitor 3 > "$WORK_DIR/m26-signals.txt" 2>&1 &)
    sleep 0.6
    rpc_py call org.yxzl.ter_music.Config.Set 'json:{"preferences":{"volume_percent":52}}' >/dev/null
    sleep 2.6
    if grep -q "Config.ConfigChanged" "$WORK_DIR/m26-signals.txt" &&
       grep -q "Control.StatusMessage" "$WORK_DIR/m26-signals.txt"; then
        ok "Config.Set 广播 ConfigChanged + StatusMessage"
    else
        bad "未捕获预期信号：$(tr '\n' ' ' < "$WORK_DIR/m26-signals.txt")"
    fi

    rpc_py call org.yxzl.ter_music.Config.Reset >/dev/null
}

# ── 主流程 ─────────────────────────────────────────────────────────
info "准备隔离环境"
setup_fixtures

if [ ! -x "$TM_BIN" ]; then
    echo "错误：找不到可执行文件 $TM_BIN（先构建，或用 --bin 指定）" >&2
    exit 2
fi

info "启动测试实例"
if ! start_daemon; then
    bad "daemon 启动失败"
    echo "（工作目录保留在 $WORK_DIR）" >&2
    KEEP=1
    exit 1
fi
ok "daemon 已启动（pid $DAEMON_PID）"

info "检查 1：接口齐备"
check_interfaces

info "检查 2：方法清单一致性"
check_method_list

info "检查 3：Info 扩展（M2.2）"
check_info_extensions

info "检查 4：前端注册表（M2.3）"
check_frontends

info "检查 5：Playlist / Queue（M2.4）"
check_playlist_queue

info "检查 6：曲库与收藏/历史（M2.5）"
check_library

info "检查 7：Config / Remote（M2.6）"
check_config_remote

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
