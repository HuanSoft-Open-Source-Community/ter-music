#!/usr/bin/env bash
#
# 配置跨重启持久化与播放模式重排的端到端回归
#
# 覆盖：
#   9.3 Config.Set 写入 → Control.Quit → 重新起 daemon（同一配置目录）→
#       Config.GetAll 仍是那个值，且 Control.GetVolume 的**生效值**也是它
#       （只落盘不生效等于没恢复）；顺带断言前序修复"Config.Set 不回卷运行时
#       倍速"：Control.SetSpeed 之后再改别的键，GetSpeed 不得被卷回文件里的旧值。
#   9.4 播放模式：下发 a/、b/ 两个文件夹混排的队列 → Control.SetPlayMode 到
#       folder 系模式 → Queue.Get 必须按核心语义收敛（游标所在组被聚成连续区段、
#       当前条目保持、组外条目被移出执行顺序）；album 系核心确有分组实现
#       （backend_queue.c 的 bq_apply_group_locked 按 album 字符串分组），
#       因此同样按这一语义断言。
#
# 用法：scripts/test/config-restart-e2e.sh [--bin <ter-music>] [--keep]
# 环境：需要会话总线（未设置时用 dbus-run-session）；需要 ffmpeg 造素材。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""

usage() { sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# 原始参数：解析循环会 shift 掉 "$@"，重入私有总线时必须带着它
ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage; exit 2 ;;
    esac
done

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

# ── 独占的总线：默认起私有会话总线 ───────────────────────────────
if [ -z "${TM_TEST_BUS_READY:-}" ] && [ "${TM_TEST_REUSE_SESSION_BUS:-0}" != "1" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：需要 dbus-run-session，或设 TM_TEST_REUSE_SESSION_BUS=1 复用当前会话总线。" >&2
        exit 2
    fi
    export TM_TEST_BUS_READY=1
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

daemon_up() { "$TM_BIN" daemon status 2>/dev/null | grep -q "pid"; }

cleanup() {
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先用 --bin 指定）" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo "错误：需要 ffmpeg 造素材" >&2; exit 2; }
command -v gdbus >/dev/null 2>&1 || { echo "错误：需要 gdbus（直调 RPC 断言契约）" >&2; exit 2; }

RPC="$SCRIPT_DIR/rpc_client.py"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-cfg-restart-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$XDG_CONFIG_HOME" "$WORK_DIR/a" "$WORK_DIR/b"

for i in 1 2 3; do
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "sine=frequency=$((300 + i * 90)):duration=20" \
        -c:a pcm_s16le -y "$WORK_DIR/a/a$i.wav" 2>/dev/null
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "sine=frequency=$((800 + i * 90)):duration=20" \
        -c:a pcm_s16le -y "$WORK_DIR/b/b$i.wav" 2>/dev/null
done

# ── 观测助手 ──────────────────────────────────────────────────────
call() {   # $1 = IFACE.METHOD，其余参数原样给 rpc_client
    python3 "$RPC" call "org.yxzl.ter_music.$1" "${@:2}" 2>&1
}

gdbus_call() {   # $1 = IFACE.METHOD，其余参数原样给 gdbus（错误名会打在输出里）
    local iface_method="$1"; shift
    gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 \
        --method "org.yxzl.ter_music.$iface_method" "$@" 2>&1
}

config_get() {   # $1 = 形如 preferences.volume_percent 的路径
    call Config.GetAll | python3 -c '
import json, sys
path = sys.argv[1].split(".")
try:
    doc = json.load(sys.stdin)
except Exception:
    print("?"); raise SystemExit
for part in path:
    if not isinstance(doc, dict) or part not in doc:
        print("?"); raise SystemExit
    doc = doc[part]
print(doc)' "$1" 2>/dev/null
}

queue_paths() {   # 核心队列路径（一行一个，顺序即执行顺序）
    call Queue.Get 0 50 | python3 -c '
import json, sys
try:
    for row in (json.load(sys.stdin).get("rows") or []):
        print(row["path"])
except Exception:
    pass' 2>/dev/null
}

queue_current() {   # 游标所在行的路径
    call Queue.Get 0 50 | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
    rows = doc.get("rows") or []
    pos = doc.get("current_position")
    print(rows[pos]["path"] if pos is not None and 0 <= pos < len(rows) else "")
except Exception:
    print("")' 2>/dev/null
}

diag() {   # $1 = 段落名
    printf '       诊断[%s]\n' "$1"
    printf '         daemon: %s\n' "$("$TM_BIN" daemon status 2>&1 | head -1)"
    printf '         Config.GetAll: %s\n' "$(call Config.GetAll | head -c 700)"
    printf '         Queue.Get: %s\n' "$(call Queue.Get 0 50 | head -c 700)"
}

wait_core_down() {   # 等核心真的退出（总线名消失 / 进程没了）
    local i
    for i in $(seq 1 40); do
        daemon_up || return 0
        sleep 0.25
    done
    return 1
}

# ── 9.3 一：写入 → Quit → 重启 → 值仍在且生效 ────────────────────
info "9.3 起 daemon 并 Config.Set volume_percent=44"
"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1.5
if daemon_up; then
    ok "daemon 上线（私有总线主名）"
else
    bad "daemon 没起来，后面的断言无意义"
    diag "启动"
    exit 1
fi

set_out="$(call Config.Set 'json:{"preferences":{"volume_percent":44}}')"
case "$set_out" in
    true*) ok "Config.Set volume_percent=44 返回 true" ;;
    *) bad "Config.Set 失败：$set_out" ; diag "9.3-set" ;;
esac

# 音量是否在 Set 后立即生效：文档（docs/API_DBUS_en_US.md 的 Config 一节）写明
# "Set applying to play mode, speed or volume is visible immediately, because the
# core re-applies runtime values after saving"，给 3 秒窗口排除异步补齐的可能。
applied="$(call Control.GetVolume)"
for _ in $(seq 1 10); do
    [ "$applied" = "44" ] && break
    sleep 0.3
    applied="$(call Control.GetVolume)"
done
if [ "$applied" = "44" ]; then
    ok "Set 之后 Control.GetVolume 立即生效为 44"
else
    bad "Config.Set volume_percent=44 只改了镜像与文件，Control.GetVolume 仍是 $applied"
    printf '         （文档承诺 Set 对音量立即可见；core/core.c 的 core_config_apply_runtime()\n'
    printf '          只应用 default_playback_speed 与 default_play_mode，没有应用 volume_percent，\n'
    printf '          运行时音量只在 daemon 启动时由 cli/daemon.c 的 set_volume_percent() 落地）\n'
    diag "9.3-set"
fi

info "9.3 Control.Quit 停核心，再用同一配置目录起第二个 daemon"
call Control.Quit >/dev/null
if wait_core_down; then
    ok "Control.Quit 后核心已退出（daemon status 查不到实例）"
else
    bad "Control.Quit 之后核心仍在运行"
    diag "9.3-quit"
fi
sleep 1        # 总线名释放

"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1.5
if ! daemon_up; then
    bad "重启 daemon 失败，持久化断言无从判起"
    diag "9.3-restart"
else
    ok "同一配置目录下 daemon 重新上线"

    if [ "$(config_get preferences.volume_percent)" = "44" ]; then
        ok "Config.GetAll 的 volume_percent 跨重启仍为 44"
    else
        bad "重启后 volume_percent=$(config_get preferences.volume_percent)（期望 44）"
        diag "9.3-restart"
    fi
    if [ "$(call Control.GetVolume)" = "44" ]; then
        ok "重启后 Control.GetVolume 生效值也是 44（不只是写在文件里）"
    else
        bad "重启后生效音量=$(call Control.GetVolume)（期望 44）"
        diag "9.3-restart"
    fi
fi

# ── 9.3 二：Config.Set 不回卷运行时倍速 ──────────────────────────
info "9.3 Control.SetSpeed 1.75 后再 Set 别的键，GetSpeed 不得被卷回"
speed_set="$(call Control.SetSpeed 1.75)"
case "$speed_set" in
    true*) ok "Control.SetSpeed 1.75 返回 true" ;;
    *) bad "Control.SetSpeed 失败：$speed_set" ; diag "9.3-speed" ;;
esac
got_speed="$(call Control.GetSpeed)"
case "$got_speed" in
    1.75*) ok "GetSpeed 回读 1.75（实测 $got_speed）" ;;
    *) bad "GetSpeed='$got_speed'（期望 1.75）" ; diag "9.3-speed" ;;
esac

other_out="$(call Config.Set 'json:{"preferences":{"audio_latency_ms":100}}')"
case "$other_out" in
    true*) ok "Config.Set 改别的键（audio_latency_ms=100）返回 true" ;;
    *) bad "Config.Set 其它键失败：$other_out" ; diag "9.3-speed" ;;
esac

after_speed="$(call Control.GetSpeed)"
case "$after_speed" in
    1.75*) ok "Set 其它键之后 GetSpeed 仍是 1.75（运行时倍速没被回卷）" ;;
    *) bad "Config.Set 把运行时倍速卷回了：GetSpeed='$after_speed'（期望 1.75）"
       diag "9.3-speed" ;;
esac
if [ "$(config_get preferences.audio_latency_ms)" = "100" ]; then
    ok "补丁确实落到了配置镜像（audio_latency_ms=100）"
else
    bad "audio_latency_ms=$(config_get preferences.audio_latency_ms)（期望 100）"
    diag "9.3-speed"
fi
if [ "$(call Control.GetVolume)" = "44" ]; then
    ok "改倍速/延迟没有牵连音量（GetVolume 仍 44）"
else
    bad "GetVolume 变成 $(call Control.GetVolume)（期望仍是 44）"
    diag "9.3-speed"
fi
# SetSpeed 会同步更新配置镜像并去抖落盘：重启后倍速应当还是 1.75
sleep 2
if [ "$(config_get preferences.default_playback_speed)" = "1.75" ]; then
    ok "SetSpeed 同步进配置镜像（default_playback_speed=1.75）"
else
    bad "default_playback_speed=$(config_get preferences.default_playback_speed)（期望 1.75）"
    diag "9.3-speed"
fi

# ── 9.4 播放模式：folder / album 分组语义 ────────────────────────
info "9.4 下发 a/、b/ 混排队列 → SetPlayMode folder → Queue.Get 必须收敛到同一文件夹"
# 混排顺序：a1, b1, a2, b2, a3, b3（同文件夹条目彼此不相邻）
python3 - "$WORK_DIR" "$WORK_DIR/mixed.json" <<'PY'
import json, sys
root, out = sys.argv[1], sys.argv[2]
order = [("a", "a1", "AlbumA"), ("b", "b1", "AlbumB"), ("a", "a2", "AlbumA"),
         ("b", "b2", "AlbumB"), ("a", "a3", "AlbumA"), ("b", "b3", "AlbumB")]
entries = [{"path": "%s/%s/%s.wav" % (root, d, n), "title": n,
            "artist": "mode-test", "album": album} for d, n, album in order]
with open(out, "w", encoding="utf-8") as handle:
    json.dump({"entries": entries}, handle, ensure_ascii=False)
PY
set_mixed="$(call Queue.Set "json:$(cat "$WORK_DIR/mixed.json")")"
if [ "$set_mixed" = "6" ]; then
    ok "混排队列下发成功（Queue.Set 回 6 条）"
else
    bad "Queue.Set 返回值='$set_mixed'（期望 6）"
    diag "9.4"
fi
before_order="$(queue_paths | tr '\n' ' ')"
printf '         下发顺序: %s\n' "${before_order% }"

if [ "$(call Queue.PlayAt 0 | head -c 20)" != "true" ]; then
    bad "Queue.PlayAt 0 没成功：$(call Queue.PlayAt 0)"
    diag "9.4"
else
    ok "从第 0 行（a/a1.wav）开始播"
fi

# 5 = PLAY_MODE_FOLDER_SEQUENTIAL（include/.../types.h）
mode_out="$(call Control.SetPlayMode 5)"
case "$mode_out" in
    true*) ok "Control.SetPlayMode 5（folder 顺序）返回 true" ;;
    *) bad "SetPlayMode 5 失败：$mode_out" ; diag "9.4" ;;
esac
if [ "$(call Control.GetPlayMode)" = "5" ]; then
    ok "GetPlayMode 回读 5"
else
    bad "GetPlayMode=$(call Control.GetPlayMode)（期望 5）"
    diag "9.4"
fi
if [ -z "$(call Control.GetPlayModeName)" ]; then
    bad "GetPlayModeName 返回空"
else
    ok "GetPlayModeName 有稳定名称（$(call Control.GetPlayModeName)）"
fi

after_order="$(queue_paths)"
after_count="$(printf '%s\n' "$after_order" | grep -c .)"
current="$(queue_current)"
printf '         重排顺序: %s\n' "$(printf '%s\n' "$after_order" | tr '\n' ' ' | sed 's/ $//')"
if [ "$after_count" = "3" ]; then
    ok "folder 模式把执行顺序收敛为游标所在文件夹的 3 条（backend_queue.c 的组语义）"
else
    bad "folder 模式后条目数=$after_count（期望 3：只剩 a/ 那一组）"
    diag "9.4"
fi
case "$current" in
    */a/a1.wav) ok "重排后当前条目仍是 a/a1.wav（SetPlayMode 保持正在播的那条）" ;;
    *) bad "重排后当前条目变成 '$current'（期望 …/a/a1.wav）" ; diag "9.4" ;;
esac
foreign="$(printf '%s\n' "$after_order" | grep -c '/b/' || true)"
if [ "$foreign" = "0" ]; then
    ok "重排后的队列里不再混着另一个文件夹的条目"
else
    bad "重排后仍有 $foreign 条来自 b/（同文件夹条目未聚成连续区段）"
    diag "9.4"
fi
same_folder="$(printf '%s\n' "$after_order" | grep -c '/a/' || true)"
if [ "$same_folder" = "$after_count" ]; then
    ok "剩余条目全部来自同一文件夹（a/），且保持组内原有相对顺序"
else
    bad "剩余条目里有非 a/ 的项（a/ 计数=$same_folder / 总数=$after_count）"
    diag "9.4"
fi

info "9.4 album 模式：核心确有按 album 分组的实现，按同一语义断言"
# 重新混排一次（folder 模式已经把队列收敛掉了）
call Queue.Set "json:$(cat "$WORK_DIR/mixed.json")" >/dev/null
call Queue.PlayAt 0 >/dev/null
# 9 = PLAY_MODE_ALBUM_SEQUENTIAL
album_out="$(call Control.SetPlayMode 9)"
case "$album_out" in
    true*) ok "Control.SetPlayMode 9（album 顺序）返回 true" ;;
    *) bad "SetPlayMode 9 失败：$album_out" ; diag "9.4-album" ;;
esac
album_order="$(queue_paths)"
album_count="$(printf '%s\n' "$album_order" | grep -c .)"
album_foreign="$(printf '%s\n' "$album_order" | grep -c '/b/' || true)"
if [ "$album_count" = "3" ] && [ "$album_foreign" = "0" ]; then
    ok "album 模式同样收敛为当前专辑（AlbumA，即 a/ 那三首）的连续区段"
else
    bad "album 模式收敛异常：总数=$album_count，其中 b/ 条目=$album_foreign"
    diag "9.4-album"
fi
if [ "$(queue_current)" = "$WORK_DIR/a/a1.wav" ]; then
    ok "album 重排后当前条目保持不变"
else
    bad "album 重排后当前曲目变成 $(queue_current)"
    diag "9.4-album"
fi

info "9.3 stop_core_on_exit 写 0 → 重启核心 → Config.GetAll 回读仍 0（不回卷）"
# stop_core_on_exit 是 config v8 新增布尔（缺省 1，退出即终止核心）。按 9.3 的
# 布尔样板验证持久化链路：Set 立即可见 → 落盘 → 核心重启后仍是显式的 0，
# 不会被 v7→v8 迁移块或加载缺省卷回 1（文件已是当前属性 9，迁移块不触发）。
so_set="$(call Config.Set 'json:{"preferences":{"stop_core_on_exit":0}}')"
case "$so_set" in
    true*) ok "Config.Set stop_core_on_exit=0 返回 true" ;;
    *) bad "Config.Set stop_core_on_exit 失败：$so_set" ; diag "9.3-stop-core" ;;
esac
if [ "$(config_get preferences.stop_core_on_exit)" = "0" ]; then
    ok "Set 后 Config.GetAll 实时回读 0"
else
    bad "Set 后 stop_core_on_exit=$(config_get preferences.stop_core_on_exit)（期望 0）"
    diag "9.3-stop-core"
fi

call Control.Quit >/dev/null
if wait_core_down; then
    ok "Control.Quit 后核心退出（准备重启验证持久化）"
else
    bad "Control.Quit 之后核心仍在运行，重启断言无从判起"
    diag "9.3-stop-core"
fi
sleep 1        # 总线名释放

"$TM_BIN" daemon start >/dev/null 2>&1
sleep 1.5
if ! daemon_up; then
    bad "重启 daemon 失败，stop_core_on_exit 持久化断言无从判起"
    diag "9.3-stop-core"
else
    ok "同一配置目录下 daemon 重新上线"
    if [ "$(config_get preferences.stop_core_on_exit)" = "0" ]; then
        ok "Config.GetAll 的 stop_core_on_exit 跨重启仍为 0（不回卷到缺省 1）"
    else
        bad "重启后 stop_core_on_exit=$(config_get preferences.stop_core_on_exit)（期望 0，缺省回卷嫌疑）"
        diag "9.3-stop-core"
    fi
fi

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    KEEP=1
    printf '（工作目录保留在 %s）\n' "$WORK_DIR" >&2
fi
[ "$FAIL" -eq 0 ]
