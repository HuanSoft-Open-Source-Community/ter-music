#!/usr/bin/env bash
#
# 大曲库（>500 首）经 TUI 远端前端下发的端到端回归
#
# 覆盖（Task 9.2）：
#   1. 600 首目录由 TUI（remote 前端）装载 → 核心 Queue.Get 的 total==600
#      （分块 Set+Append 契约：单发上限 BQ_SET_MAX=500，前端必须先 Set 再 Append）；
#   2. 游标能落到最后一行（position 599）并真的开始播那一首；
#   3. 越界 position 600 被核心以 Error.OutOfRange 拒绝；
#   4. **单发** 600 条的 Queue.Set 被 Error.InvalidArgs 整体拒绝，且拒绝后
#      队列一条不少（"绝不收下前半截再报成功"的契约仍在）。
#
# CLI 侧的 600 首路径（ter-music play <600 首目录> + queue list）已由
# scripts/test/cli-subcommands-smoke.sh 覆盖，本脚本不重复，只补 TUI 前端与
# 直接 RPC 契约这两块。
#
# 用法：scripts/test/big-library-e2e.sh [--bin <ter-music>] [--keep]
#       [--count N]（默认 600）
# 环境：需要会话总线（未设置时用 dbus-run-session）与 ffmpeg 造素材。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""
BIG_COUNT=600
DRIVER_PID=""
CTL_FILE=""

usage() { sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# 原始参数：解析循环会 shift 掉 "$@"，重入私有总线时必须带着它
ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) TM_BIN="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --count) BIG_COUNT="$2"; shift 2 ;;
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

cleanup() {
    [ -n "$DRIVER_PID" ] && kill -9 "$DRIVER_PID" 2>/dev/null
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先用 --bin 指定）" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo "错误：需要 ffmpeg 造素材" >&2; exit 2; }

DRIVER="$SCRIPT_DIR/lifecycle-driver.py"
RPC="$SCRIPT_DIR/rpc_client.py"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-big-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$XDG_CONFIG_HOME" "$WORK_DIR/big"

info "造素材：$BIG_COUNT 个小 wav（与 cli-subcommands-smoke.sh 同一口径：0.05s 正弦）"
for i in $(seq 1 "$BIG_COUNT"); do
    ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=0.05" \
        -c:a pcm_s16le -y "$WORK_DIR/big/track$(printf '%03d' "$i").wav" 2>/dev/null
done
made="$(find "$WORK_DIR/big" -name '*.wav' | wc -l)"
if [ "$made" = "$BIG_COUNT" ]; then
    ok "素材就绪：$made 个 wav"
else
    bad "只生成了 $made 个 wav（期望 $BIG_COUNT），后面的断言没有意义"
    exit 1
fi

# ── 观测助手 ──────────────────────────────────────────────────────
gdbus_call() {   # $1 = IFACE.METHOD, 其余为参数（原样传给 gdbus）
    local iface_method="$1"; shift
    gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 \
        --method "org.yxzl.ter_music.$iface_method" "$@" 2>&1
}

queue_total() {   # Queue.Get 的 count 字段（与分页 offset/count 无关，是总数）
    python3 "$RPC" call org.yxzl.ter_music.Queue.Get "${1:-0}" "${2:-1}" 2>/dev/null |
        python3 -c '
import json, sys
try:
    print(json.load(sys.stdin).get("count"))
except Exception:
    print("?")' 2>/dev/null
}

queue_row_path() {   # $1 = position
    python3 "$RPC" call org.yxzl.ter_music.Queue.Get "$1" 1 2>/dev/null |
        python3 -c '
import json, sys
try:
    rows = json.load(sys.stdin).get("rows") or []
    print(rows[0]["path"] if rows else "")
except Exception:
    print("")' 2>/dev/null
}

cur_position() {
    python3 "$RPC" call org.yxzl.ter_music.Queue.Get 0 1 2>/dev/null |
        python3 -c '
import json, sys
try:
    print(json.load(sys.stdin).get("current_position"))
except Exception:
    print("?")' 2>/dev/null
}

info_json() {
    python3 "$RPC" call org.yxzl.ter_music.Info.GetInfo 2>/dev/null
}

play_state() {
    info_json | python3 -c '
import json, sys
try:
    print(json.load(sys.stdin)["playback"]["state"])
except Exception:
    print("?")' 2>/dev/null
}

cur_track_path() {
    info_json | python3 -c '
import json, sys
try:
    print((json.load(sys.stdin).get("track") or {}).get("path") or "")
except Exception:
    print("")' 2>/dev/null
}

diag() {   # $1 = 场景名
    printf '       诊断[%s]\n' "$1"
    printf '         daemon: %s\n' "$("$TM_BIN" daemon status 2>&1 | head -1)"
    printf '         Queue.Get(0,2): %s\n' "$(python3 "$RPC" call org.yxzl.ter_music.Queue.Get 0 2 2>/dev/null | head -c 400)"
    printf '         Queue.Get(%d,2): %s\n' "$((BIG_COUNT - 2))" \
        "$(python3 "$RPC" call org.yxzl.ter_music.Queue.Get $((BIG_COUNT - 2)) 2 2>/dev/null | head -c 400)"
    printf '         show --json: %s\n' "$("$TM_BIN" show --json 2>/dev/null | head -c 400)"
    [ -f "$WORK_DIR/tui.log" ] && python3 - "$WORK_DIR/tui.log" <<'PY'
import re, sys
raw = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\(B|[\x00-\x08\x0b-\x1f]", "", raw)
lines = [l.rstrip() for l in text.splitlines() if l.strip()]
for line in lines[-6:]:
    print("  | %s" % line[:100])
PY
}

# ── 1. TUI（remote 前端）装载 600 首目录 ─────────────────────────
info "TUI 装载 $BIG_COUNT 首目录：核心 Queue.Get total 必须等于 $BIG_COUNT"
CTL_FILE="$WORK_DIR/keys.cmd"
: > "$CTL_FILE"
python3 "$DRIVER" drive "$TM_BIN" "$WORK_DIR/big" "$WORK_DIR/tui.log" \
    --ctl "$CTL_FILE" \
    --rc-file "$WORK_DIR/tui.rc" --pid-file "$WORK_DIR/tui.pid" \
    --max-runtime 300 >/dev/null 2>&1 &
DRIVER_PID=$!

total=""
# 600 首要逐页读标签再分块下发，给它足够的耐心（慢机器上这是主耗时项）
for _ in $(seq 1 240); do
    total="$(queue_total 0 1)"
    [ "$total" = "$BIG_COUNT" ] && break
    sleep 0.5
done
if [ "$total" = "$BIG_COUNT" ]; then
    ok "Queue.Get total==$BIG_COUNT（前端分块 Set+Append 全量到达核心）"
else
    bad "Queue.Get total='$total'（期望 $BIG_COUNT）"
    diag "装载"
fi

# ── 2. 最后一行（position 599）可 PlayAt 且真的在播 ──────────────
LAST=$((BIG_COUNT - 1))
info "PlayAt $LAST（最后一行）：必须成功且游标落在该行"
# 素材只有 0.05s，顺序模式下最后一首放完就停；这里先切单曲循环，
# 让"在播这一行"成为可稳定轮询到的状态（测的是游标与下发，不是音频时长）。
gdbus_call Control.SetPlayMode 1 >/dev/null
out_last="$(gdbus_call Queue.PlayAt "$LAST")"
case "$out_last" in
    *true*) ok "Queue.PlayAt $LAST 返回 true" ;;
    *) bad "Queue.PlayAt $LAST 未成功：$out_last" ; diag "PlayAt$LAST" ;;
esac
pos=""
for _ in $(seq 1 20); do
    pos="$(cur_position)"
    [ "$pos" = "$LAST" ] && break
    sleep 0.3
done
if [ "$pos" = "$LAST" ]; then
    ok "核心游标 current_position=$LAST（与请求一致）"
else
    bad "游标不在 $LAST（实测 '$pos'）"
    diag "PlayAt$LAST"
fi
want="$(printf '%s/track%s.wav' "$WORK_DIR/big" "$(printf '%03d' "$BIG_COUNT")")"
got="$(queue_row_path "$LAST")"
if [ "$got" = "$want" ]; then
    ok "第 $LAST 行的路径就是 $want"
else
    bad "第 $LAST 行路径='$got'（期望 $want）"
    diag "PlayAt$LAST"
fi
state=""
track=""
for _ in $(seq 1 20); do
    state="$(play_state)"
    track="$(cur_track_path)"
    [ "$state" = "playing" ] && break
    sleep 0.3
done
if [ "$state" = "playing" ]; then
    ok "播的是最后一行（Info 快照 playback.state=playing）"
else
    bad "PlayAt $LAST 后状态='$state'（期望 playing）"
    diag "PlayAt$LAST"
fi
if [ "$track" = "$want" ]; then
    ok "核心上报的当前曲目就是第 $LAST 行（track.path 一致）"
else
    bad "核心当前曲目='$track'（期望 $want）"
    diag "PlayAt$LAST"
fi

# ── 3. 越界 position 必须被拒 ────────────────────────────────────
info "PlayAt $BIG_COUNT（越界）：必须 Error.OutOfRange"
out_over="$(gdbus_call Queue.PlayAt "$BIG_COUNT")"
case "$out_over" in
    *OutOfRange*) ok "越界 PlayAt 被拒（Error.OutOfRange）" ;;
    *) bad "越界 PlayAt 没有被拒：$out_over" ; diag "越界" ;;
esac

# ── 4. 单发 >500 条 Queue.Set 契约 ───────────────────────────────
info "单发 $BIG_COUNT 条的 Queue.Set：必须 Error.InvalidArgs 且队列不被啃掉一半"
python3 - "$WORK_DIR/big" "$WORK_DIR/payload.json" "$BIG_COUNT" <<'PY'
import json, os, sys
root, out, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
entries = [{"path": os.path.join(root, "track%03d.wav" % i),
            "title": "track%03d" % i, "artist": "big", "album": "big"}
           for i in range(1, n + 1)]
with open(out, "w", encoding="utf-8") as handle:
    json.dump({"entries": entries}, handle, ensure_ascii=False)
PY
before="$(queue_total 0 1)"
out_set="$(gdbus_call Queue.Set "$(cat "$WORK_DIR/payload.json")")"
case "$out_set" in
    *InvalidArgs*) ok "单发 $BIG_COUNT 条 Queue.Set 被拒（Error.InvalidArgs，提示先 Set 后 Append）" ;;
    *) bad "单发 $BIG_COUNT 条 Queue.Set 没有被拒：$(printf '%s' "$out_set" | head -c 120)" ; diag "单发Set" ;;
esac
after="$(queue_total 0 1)"
if [ "$after" = "$before" ]; then
    ok "被拒的 Set 没有改动队列（total 仍为 $after，未收下前半截）"
else
    bad "被拒的 Set 把队列改坏了：$before → $after"
    diag "单发Set"
fi

# 合规的分块路径必须仍然可用：Set 500 + Append 100 → total==600
info "合规分块下发（Set 500 + Append 100）必须成功"
python3 - "$WORK_DIR/big" "$WORK_DIR/set500.json" "$WORK_DIR/append100.json" "$BIG_COUNT" <<'PY'
import json, os, sys
root, set_out, app_out, n = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
def entry(i):
    name = "track%03d.wav" % i
    return {"path": os.path.join(root, name), "title": name[:-4],
            "artist": "chunk", "album": "chunk"}
mid = 500
with open(set_out, "w", encoding="utf-8") as handle:
    json.dump({"entries": [entry(i) for i in range(1, mid + 1)]}, handle, ensure_ascii=False)
with open(app_out, "w", encoding="utf-8") as handle:
    json.dump({"entries": [entry(i) for i in range(mid + 1, n + 1)]}, handle, ensure_ascii=False)
PY
set_rc="$(gdbus_call Queue.Set "$(cat "$WORK_DIR/set500.json")")"
app_rc="$(gdbus_call Queue.Append "$(cat "$WORK_DIR/append100.json")")"
chunked="$(queue_total 0 1)"
if [ "$chunked" = "$BIG_COUNT" ]; then
    ok "Set(500)+Append(100) 后 total==$BIG_COUNT（set 回=$set_rc append 回=$app_rc）"
else
    bad "分块下发后 total='$chunked'（期望 $BIG_COUNT）；set 回=$set_rc append 回=$app_rc"
    diag "分块"
fi

# ── 收尾：前端退出 + 核心必停（不留残留进程）─────────────────────
info "收尾：'q' 退出前端并停掉核心"
printf 'key:q\n' >> "$CTL_FILE" 2>/dev/null || true
sleep 3
if [ -n "$DRIVER_PID" ] && kill -0 "$DRIVER_PID" 2>/dev/null; then
    printf 'kill\n' >> "$CTL_FILE" 2>/dev/null || true
    sleep 2
    bad "前端 'q' 之后按键会话仍在"
    diag "收尾"
else
    ok "按键会话随前端退出而收尾"
fi
"$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
sleep 1
if "$TM_BIN" daemon status >/dev/null 2>&1; then
    bad "脚本收尾后核心仍在运行"
else
    ok "脚本收尾后核心已停（daemon status 退出码非 0）"
fi

info "结果"
printf '%d 通过, %d 失败\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    KEEP=1
    printf '（工作目录保留在 %s，TUI 按键捕获在 %s/tui.log）\n' \
        "$WORK_DIR" "$WORK_DIR" >&2
fi
[ "$FAIL" -eq 0 ]
