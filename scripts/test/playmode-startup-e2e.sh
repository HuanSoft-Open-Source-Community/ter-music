#!/usr/bin/env bash
#
# 播放模式在远端前端启动/选曲时的端到端回归
#
# 覆盖前后端分离后容易漏掉的两条核心契约：
#   1. 前端把内容 Queue.Set/Append 给核心后，必须重新应用**核心当前**播放模式；
#      否则配置 default_play_mode=9（专辑顺序）启动后，核心仍保留整份内容，
#      组模式形同虚设。
#   2. 用户在 TUI 里选中“当前组之外”的曲目时，前端必须先把完整内容补推给核心，
#      PlayAt 目标后再以目标为锚应用模式；否则目标被组模式裁掉，点了没反应。
#
# 用法：scripts/test/playmode-startup-e2e.sh [--bin <ter-music>] [--keep]
# 环境：需要会话总线（未设置时用 dbus-run-session）；需要 ffmpeg 造素材。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""

usage() { sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

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
SKIP=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
skip() { SKIP=$((SKIP + 1)); printf '  SKIP %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

finish() {
    info "结果"
    printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
    [ "$FAIL" -eq 0 ]
}

cleanup() {
    if [ -n "$TM_BIN" ] && [ -x "$TM_BIN" ]; then
        "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    fi
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先用 --bin 指定）" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo "错误：需要 ffmpeg 造素材" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "错误：需要 python3" >&2; exit 2; }

# 已有会话总线时直接复用；否则用 dbus-run-session 重入。
ORIG_ARGS=("$@")
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "错误：没有会话总线，且未安装 dbus-run-session。" >&2
        exit 2
    fi
    exec dbus-run-session -- "$0" "${ORIG_ARGS[@]}"
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-playmode-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$XDG_CONFIG_HOME/ter-music" "$XDG_CACHE_HOME" "$WORK_DIR/music"

info "造素材：同一目录下两个 album 各 2 首（专辑组模式）"
for album in A B; do
    for i in 1 2; do
        ffmpeg -hide_banner -loglevel error -f lavfi \
            -i "sine=frequency=$((300 + i * 100)):duration=0.4" \
            -metadata album="Album$album" -metadata title="$album-$i" \
            -c:a pcm_s16le -y "$WORK_DIR/music/$(printf '%s-%d.wav' "$(printf '%s' "$album" | tr '[:upper:]' '[:lower:]')" "$i")" 2>/dev/null
    done
done

cat > "$XDG_CONFIG_HOME/ter-music/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="9">
  <preferences>
    <stop_core_on_exit>0</stop_core_on_exit>
    <default_play_mode>9</default_play_mode>
  </preferences>
</ter-music-config>
EOF

queue_count() {
    python3 "$SCRIPT_DIR/rpc_client.py" call cn.org.huansoft.ter_music.Queue.Get 0 1 2>/dev/null |
        python3 -c 'import json,sys; print(json.load(sys.stdin).get("count"))' 2>/dev/null
}
queue_rows() {
    python3 "$SCRIPT_DIR/rpc_client.py" call cn.org.huansoft.ter_music.Queue.Get 0 10 2>/dev/null |
        python3 -c '
import json,sys
doc=json.load(sys.stdin)
for row in doc.get("rows") or []:
    print(row.get("path",""))' 2>/dev/null
}

info "第一次启动：内容下发后必须应用专辑模式"
python3 "$SCRIPT_DIR/tui-probe.py" --bin "$TM_BIN" --music "$WORK_DIR/music" \
    --home "$HOME" --keys "" --wait 5 >"$WORK_DIR/first.out" 2>&1 || true
sleep 1
first_count="$(queue_count)"
first_rows="$(queue_rows)"
if [ "$first_count" = "2" ] &&
   ! printf '%s\n' "$first_rows" | grep -q '/music/b-'; then
    ok "启动后队列收敛到当前专辑（count=2，仅 a-*）"
else
    bad "启动后未应用文件夹模式：count=${first_count:-?} rows=$(printf '%s' "$first_rows" | tr '\n' ' ')"
fi

info "第二次启动：从 a/ 组选中 b/ 曲目，必须跨组切到 b/"
# 内容顺序 a-1,a-2,b-1,b-2；三次 Down（树视图首行是根目录）到 b 组，Enter 播放。
python3 "$SCRIPT_DIR/tui-probe.py" --bin "$TM_BIN" --music "$WORK_DIR/music" \
    --home "$HOME" --keys "jjj<enter>" --wait 5 \
    --state-out "$WORK_DIR/second-state.json" >"$WORK_DIR/second.out" 2>&1 || true
sleep 1
second_count="$(queue_count)"
second_rows="$(queue_rows)"
if [ "$second_count" = "2" ] &&
   ! printf '%s\n' "$second_rows" | grep -q '/music/a-'; then
    ok "跨组选曲后队列切到目标组（count=2，仅 b-*）"
else
    bad "跨组选曲未生效：count=${second_count:-?} rows=$(printf '%s' "$second_rows" | tr '\n' ' ')"
fi

info "CLI play 未显式 --mode：也必须应用核心当前模式"
"$TM_BIN" play "$WORK_DIR/music" >/dev/null 2>&1
cli_rc=$?
sleep 1
cli_count="$(queue_count)"
cli_rows="$(queue_rows)"
if [ "$cli_rc" -eq 0 ] && [ "$cli_count" = "2" ] &&
   ! printf '%s\n' "$cli_rows" | grep -q '/music/b-'; then
    ok "CLI play 应用 default_play_mode=9（count=2，仅 a-*）"
else
    bad "CLI play 未应用默认模式：rc=$cli_rc count=${cli_count:-?} rows=$(printf '%s' "$cli_rows" | tr '\n' ' ')"
fi

info "本地前端（TUI 即核心）：同样必须启动应用模式并支持跨组选曲"
"$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
sleep 1
LOCAL_CTL="$WORK_DIR/local.ctl"
: > "$LOCAL_CTL"
python3 "$SCRIPT_DIR/lifecycle-driver.py" drive "$TM_BIN" "$WORK_DIR/music" \
    "$WORK_DIR/local-tui.log" --ctl "$LOCAL_CTL" \
    --rc-file "$WORK_DIR/local.rc" --pid-file "$WORK_DIR/local.pid" \
    --max-runtime 30 -- --frontend local >/dev/null 2>&1 &
LOCAL_DRV=$!
sleep 4
local_count="$(queue_count)"
local_rows="$(queue_rows)"
if [ "$local_count" = "2" ] &&
   ! printf '%s\n' "$local_rows" | grep -q '/music/b-'; then
    ok "本地前端启动后按专辑模式收敛（count=2，仅 a-*）"
else
    bad "本地前端启动未应用模式：count=${local_count:-?} rows=$(printf '%s' "$local_rows" | tr '\n' ' ')"
fi
printf 'key:j\nkey:j\nkey:j\nkey:<Enter>\n' >> "$LOCAL_CTL"
sleep 2
local_after="$(queue_count)"
local_after_rows="$(queue_rows)"
if [ "$local_after" = "2" ] &&
   ! printf '%s\n' "$local_after_rows" | grep -q '/music/a-'; then
    ok "本地前端跨组选曲后切到目标专辑（count=2，仅 b-*）"
else
    bad "本地前端跨组选曲未生效：count=${local_after:-?} rows=$(printf '%s' "$local_after_rows" | tr '\n' ' ')"
fi
printf 'kill\n' >> "$LOCAL_CTL"
wait "$LOCAL_DRV" 2>/dev/null || true
"$TM_BIN" daemon stop --all >/dev/null 2>&1 || true

finish
exit $?
