#!/usr/bin/env bash
#
# 前端本地内容功能（曲库浏览 / 收藏 / 历史 / 搜索）→ 播放的端到端回归
#
# 覆盖（Task 9.1）：
#   A 曲库/文件浏览：浏览器视图里选中某曲按 Enter → 核心队列出现该路径、
#     游标落在该行、MPRIS 元数据与状态如实上报 Playing；并对该曲按 'f' 收藏。
#   C 播放历史：换一个目录后（原曲目不在内容列表里），'h' 历史弹窗选中 →
#     Enter 直播（append_playlist 兜底 + 门面重推）→ 核心确实回到那首；
#     顺带断言 SQLite play_history 因 add_history_entry 链路新增了行。
#   B 收藏：F5 收藏视图里 Enter 播放**未加载**的收藏曲目 → 核心队列出现该
#     路径并在播（收藏直播兜底链路的回归）。
#   D 搜索：'/' 输入曲名片段 → 结果里 Enter → 核心播该结果。
#   收尾前提：'q' 退出前端后断言核心仍在总线上，以 stop_core_on_exit=0 夹具为
#     前提（核心启动前写入，见"起会话"前的注释）；config v8 起缺省为 1，正常
#     退出会 Control.Quit 终止核心，缺省态（=1）由 lifecycle-e2e.sh 的 S8 专门覆盖。
#
# 按键以**代码**为准（不读帮助文本）：
#   ui.c      浏览器视图：'j'/'k' 移动光标，Enter(10)/空格 播放树里选中的文件；
#             'f'/'F' = 收藏当前曲目（add_to_favorites）；'o' = 打开目录输入框；
#             '/'/'S' = search_prompt()，输入后再 Enter 触发异步搜索，
#             结果态下 Enter/' ' 播放选中结果；'h' = 播放历史弹窗（选中项 Enter 播放）。
#   menus.c   KEY_F(5) → VIEW_FAVORITES（收藏视图）。
#   favorites.c / history 弹窗：Tab(9)/KEY_RIGHT 把焦点从侧栏移到内容区，
#             Enter(10)/空格 播放选中条目（不在列表则 append_playlist 后播）。
#   浏览器树首行是根目录节点（playlist.c：root expanded 的 DIRECTORY 节点），
#   所以第 n 个文件要按 n+1 次 'j'——脚本按 Queue.Get 实测序号自适应。
#
# 用法：scripts/test/content-playback-e2e.sh [--bin <ter-music>] [--keep]
# 环境：需要会话总线（未设置时用 dbus-run-session）、ffmpeg 造素材、
#       python3 + gi（rpc_client.py）与 gdbus（MPRIS 属性断言）。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TM_BIN="${TM_BIN:-$REPO_ROOT/build/ter-music}"
KEEP=0
WORK_DIR=""
DRIVER_PID=""
CTL_FILE=""

usage() { sed -n '2,31p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

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
SKIP=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
skip() { SKIP=$((SKIP + 1)); printf '  SKIP %s\n' "$1"; }
info() { printf -- '--- %s\n' "$1"; }

# ── 独占的总线：默认起私有会话总线 ───────────────────────────────
# 只允许本脚本拉起的核心在总线上（否则前端会接到桌面残留的实例上）；需要复用
# 当前总线时显式传 TM_TEST_REUSE_SESSION_BUS=1。TM_TEST_BUS_READY 是再入标记。
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
    # 顺序：先收尾 TUI 会话（驱动 + pty 里的前端），再停核心，最后删临时目录。
    # commit 4bf57fc 的约定：脚本退出必须把拉起来的核心停掉，不留残留进程。
    [ -n "$DRIVER_PID" ] && kill -9 "$DRIVER_PID" 2>/dev/null
    "$TM_BIN" daemon stop --all >/dev/null 2>&1 || true
    if [ "$KEEP" -eq 0 ] && [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

[ -x "$TM_BIN" ] || { echo "错误：找不到可执行文件 $TM_BIN（先用 --bin 指定）" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo "错误：需要 ffmpeg 造素材" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "错误：需要 python3" >&2; exit 2; }

DRIVER="$SCRIPT_DIR/lifecycle-driver.py"
RPC="$SCRIPT_DIR/rpc_client.py"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tm-content-XXXXXX")"
export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home/.config"
export XDG_CACHE_HOME="$WORK_DIR/home/.cache"
mkdir -p "$XDG_CONFIG_HOME" "$WORK_DIR/dir1" "$WORK_DIR/dir2"

# 30 秒的音：足够长，断言窗口里不会自己切到下一首（短音会让"在播 X"变成
# "X 放完了自动进了下一首"，那是时序问题而不是被测行为）。
tone() {   # $1 = 频率, $2 = 路径
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "sine=frequency=$1:duration=30" -c:a pcm_s16le -y "$2" 2>/dev/null
}
tone 300 "$WORK_DIR/dir1/01-alpha.wav"
tone 350 "$WORK_DIR/dir1/02-bravo.wav"
tone 400 "$WORK_DIR/dir1/03-charlie.wav"
tone 700 "$WORK_DIR/dir2/10-xray.wav"
tone 750 "$WORK_DIR/dir2/11-zulu.wav"

# ── 观测助手 ──────────────────────────────────────────────────────
queue_json() {   # $1 = offset, $2 = count
    python3 "$RPC" call org.yxzl.ter_music.Queue.Get "${1:-0}" "${2:-50}" 2>/dev/null
}

play_state() {   # 门面上的播放状态（show 经 D-Bus 取核心快照）
    "$TM_BIN" show --json 2>/dev/null | python3 -c '
import json, sys
try:
    print(json.load(sys.stdin)["playback"]["state"])
except Exception:
    print("?")' 2>/dev/null
}

cur_path() {   # 核心队列游标所在行的路径
    queue_json 0 50 | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
    rows = doc.get("rows") or []
    pos = doc.get("current_position")
    if pos is None or pos < 0 or pos >= len(rows):
        print("")
    else:
        print(rows[pos]["path"])
except Exception:
    print("")' 2>/dev/null
}

queue_paths() {   # 核心队列全部路径（一行一个，顺序即执行顺序）
    queue_json 0 50 | python3 -c '
import json, sys
try:
    for row in (json.load(sys.stdin).get("rows") or []):
        print(row["path"])
except Exception:
    pass' 2>/dev/null
}

rank_of() {   # $1 = 文件名（尾部匹配）→ 在核心队列里的 0-based 序号，找不到回 -1
    queue_paths | python3 -c '
import sys, os
want = sys.argv[1]
for i, line in enumerate(sys.stdin.read().splitlines()):
    if os.path.basename(line) == want:
        print(i)
        break
else:
    print(-1)' "$1" 2>/dev/null
}

mpris_prop() {   # $1 = MPRIS Player 属性名（gdbus 原文输出）
    if ! command -v gdbus >/dev/null 2>&1; then
        echo "NO_GDBUS"
        return 0
    fi
    gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
        --object-path /org/mpris/MediaPlayer2 \
        --method org.freedesktop.DBus.Properties.Get \
        org.mpris.MediaPlayer2.Player "$1" 2>/dev/null
}

mpris_meta_field() {   # $1 = 元数据键（如 xesam:title）→ 值
    # gdbus 把 a{sv} 打成 {'xesam:title': <'03-charlie'>, 'xesam:url': <'file:///…'>, …}
    # 键名带着单引号，冒号在引号之后，取值在 <'…'> 里。
    mpris_prop Metadata | python3 -c '
import re, sys
key = sys.argv[1]
raw = sys.stdin.read()
m = re.search(re.escape(key) + r"[\x27\"]?:\s*<[\x27\"]([^\x27\"]*)[\x27\"]>", raw)
print(m.group(1) if m else "")' "$1" 2>/dev/null
}

db_query() {   # $1 = SQL → 每行一个结果
    python3 - "$XDG_CONFIG_HOME/ter-music/library.db" "$1" <<'PY'
import sqlite3, sys
try:
    con = sqlite3.connect(sys.argv[1])
    for row in con.execute(sys.argv[2]):
        print(row[0] if len(row) == 1 else row)
    con.close()
except Exception as exc:                                   # noqa: BLE001
    print("ERROR: %s" % exc, file=sys.stderr)
    sys.exit(1)
PY
}

screen_tail() {   # TUI 屏的尾部（去掉转义序列），失败诊断用
    python3 - "$WORK_DIR/tui.log" <<'PY'
import re, sys
try:
    raw = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
except OSError:
    print("(没有 TUI 捕获)")
    raise SystemExit
text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\(B|[\x00-\x08\x0b-\x1f]", "", raw)
lines = [l.rstrip() for l in text.splitlines() if l.strip()]
for line in lines[-8:]:
    print("  | %s" % line[:100])
PY
}

diag() {   # $1 = 场景名：失败时把观测面原样摊开
    printf '       诊断[%s]\n' "$1"
    printf '         daemon: %s\n' "$("$TM_BIN" daemon status 2>&1 | head -1)"
    printf '         Queue.Get: %s\n' "$(queue_json 0 50 | head -c 700)"
    printf '         show --json: %s\n' "$("$TM_BIN" show --json 2>/dev/null | head -c 500)"
    printf '         播放状态: %s\n' "$(play_state)"
    printf '         当前游标: %s\n' "$(cur_path)"
    printf '         TUI 屏尾:\n'
    screen_tail
}

# ── 按键会话（lifecycle-driver.py drive）─────────────────────────
start_session() {   # $1 = 音乐目录
    CTL_FILE="$WORK_DIR/keys.cmd"
    : > "$CTL_FILE"
    python3 "$DRIVER" drive "$TM_BIN" "$1" "$WORK_DIR/tui.log" \
        --ctl "$CTL_FILE" \
        --rc-file "$WORK_DIR/tui.rc" --pid-file "$WORK_DIR/tui.pid" \
        --max-runtime 240 >/dev/null 2>&1 &
    DRIVER_PID=$!
}

send() { printf 'key:%s\n' "$1" >> "$CTL_FILE" 2>/dev/null || true; }
tick() { sleep "${1:-1}"; }

session_alive() {
    [ -n "$DRIVER_PID" ] && kill -0 "$DRIVER_PID" 2>/dev/null
}

tui_alive() {   # pty 里那个前端的 pid 由驱动写进 --pid-file
    local pid
    pid="$(cat "$WORK_DIR/tui.pid" 2>/dev/null)"
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

wait_session_stop() {   # 等驱动会话收尾（TUI 退出后驱动会写 rc 并结束）
    local i
    for i in $(seq 1 24); do
        [ -n "$DRIVER_PID" ] && kill -0 "$DRIVER_PID" 2>/dev/null || return 0
        sleep 0.5
    done
    return 1
}

wait_for_queue() {   # $1 = 期望条目数, $2 = 超时秒
    local want="$1" limit="${2:-25}" i
    for i in $(seq 1 "$((limit * 2))"); do
        [ "$(queue_paths | wc -l)" = "$want" ] && return 0
        sleep 0.5
    done
    return 1
}

wait_for_play() {   # $1 = 期望文件名, $2 = 超时秒 → 置 GLOBAL 实测游标
    local want="$1" limit="${2:-12}" i
    CUR=""
    for i in $(seq 1 "$((limit * 2))"); do
        CUR="$(cur_path)"
        [ "${CUR##*/}" = "$want" ] && return 0
        sleep 0.5
    done
    return 1
}

# ── stop_core_on_exit=0 夹具：本脚本 'q' 收尾断言的前提 ────────────────
# 末尾的“前端退出后核心仍在总线上”依赖 'q' 退出前端时不终止核心：config v8
# 起缺省 stop_core_on_exit=1，remote 前端正常退出（q/F9/Ctrl+C/SIGTERM）会发
# Control.Quit 把核心进程终止。必须在 TUI 拉起核心之前把开关显式写成 0（前端
# 退出时经 Config.GetAll 实时读的就是核心的这个值）。缺省态（=1）的行为由
# lifecycle-e2e.sh 的 S8 段专门覆盖，这里不重复验证；version="8" 配置里属性 8
# 的显式值不会被迁移块回卷，夹具写一次即稳定生效。
mkdir -p "$XDG_CONFIG_HOME/ter-music"
cat > "$XDG_CONFIG_HOME/ter-music/config.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ter-music-config version="8">
  <preferences>
    <stop_core_on_exit>0</stop_core_on_exit>
  </preferences>
</ter-music-config>
EOF

# ── 起会话：TUI（remote 前端）拉起核心并装载 dir1 ────────────────
info "起 TUI 会话（-o dir1），等它把内容下发给核心"
start_session "$WORK_DIR/dir1"

if ! wait_for_queue 3 30; then
    bad "dir1 的 3 条内容没有到达核心（实测 $(queue_paths | wc -l) 条）"
    diag "启动"
    info "结果"
    printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
    exit 1
fi
ok "前端把 dir1 的 3 条内容下发给了核心（Queue.Get count=3）"

if ! session_alive || ! tui_alive; then
    bad "TUI 会话已经退出，后面的按键无从下发"
    diag "启动"
    info "结果"
    printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
    exit 1
fi

# ── 场景 A：文件浏览器选中某曲 → Enter 播放 ──────────────────────
info "A 文件浏览器：选中 03-charlie → Enter 播放"
TARGET_A="03-charlie.wav"
RANK_A="$(rank_of "$TARGET_A")"
if [ "${RANK_A:- -1}" -lt 0 ]; then
    bad "Queue.Get 里找不到 $TARGET_A，无法定位浏览器光标"
    diag "A"
else
    # 树首行是根目录节点：第 rank 个文件要按 rank+1 次 'j'
    for _ in $(seq 1 $((RANK_A + 1))); do
        send "j"
    done
    tick 2
    send "<Enter>"
    if wait_for_play "$TARGET_A"; then
        ok "Enter 播放了浏览器里选中的 $TARGET_A（游标=$(cur_path)）"
    else
        bad "Enter 之后核心游标不在 $TARGET_A（实测 '${CUR:-空}'）"
        diag "A"
    fi

    state="$(play_state)"
    if [ "$state" = "playing" ] || [ "$state" = "Playing" ]; then
        ok "核心状态为 Playing（show --json playback.state=$state）"
    else
        bad "核心状态不是 Playing（实测 '$state'）"
        diag "A"
    fi

    if [ "$(mpris_prop PlaybackStatus)" = "NO_GDBUS" ]; then
        skip "gdbus 不可用：跳过 MPRIS 属性断言"
    else
        mpris_status="$(mpris_prop PlaybackStatus)"
        case "$mpris_status" in
            *Playing*) ok "MPRIS PlaybackStatus = Playing" ;;
            *) bad "MPRIS PlaybackStatus 不是 Playing：$mpris_status" ; diag "A" ;;
        esac
        mpris_title="$(mpris_meta_field xesam:title)"
        if [ "$mpris_title" = "${TARGET_A%.wav}" ]; then
            ok "MPRIS Metadata xesam:title 与所选曲目一致（$mpris_title）"
        else
            bad "MPRIS Metadata xesam:title='$mpris_title'（期望 '${TARGET_A%.wav}'）"
            diag "A"
        fi
        mpris_url="$(mpris_meta_field xesam:url)"
        if [ "$mpris_url" = "file://$WORK_DIR/dir1/$TARGET_A" ]; then
            ok "MPRIS Metadata xesam:url 指向所选曲目（$mpris_url）"
        else
            bad "MPRIS Metadata xesam:url='$mpris_url'（期望 file://$WORK_DIR/dir1/$TARGET_A）"
            diag "A"
        fi
    fi
fi

# ── 场景 A 收尾：'f' 收藏另一首（02-bravo），供场景 B 使用 ────────
info "A 'f' 收藏 02-bravo.wav（收藏当前曲目键，ui.c case 'f'）"
TARGET_B="02-bravo.wav"
RANK_B="$(rank_of "$TARGET_B")"
if [ "${RANK_B:- -1}" -lt 0 ]; then
    bad "Queue.Get 里找不到 02-bravo.wav，无法把光标移过去收藏"
    diag "A-f"
else
    # 光标现在在 rank_A+1 行（树节点与内容序号同序，扁平目录实测一致）
    delta=$((RANK_B - RANK_A))
    if [ "$delta" -gt 0 ]; then
        for _ in $(seq 1 "$delta"); do send "j"; done
    elif [ "$delta" -lt 0 ]; then
        for _ in $(seq 1 $((0 - delta))); do send "k"; done
    fi
    tick 2
    send "f"
    tick 2
    fav_rows="$(db_query "select track_path from favorites" 2>/dev/null)"
    case "$fav_rows" in
        *"$WORK_DIR/dir1/$TARGET_B"*) ok "收藏写入 SQLite favorites 表（$TARGET_B）" ;;
        *) bad "SQLite favorites 表里没有 $TARGET_B（实测：$(printf '%s' "$fav_rows" | tr '\n' ' ')）"
           diag "A-f" ;;
    esac
fi

# ── 场景 C：换目录后从历史弹窗播放（append 兜底 + 历史落库）────────
info "C 'o' 换成 dir2 目录，再从 'h' 历史弹窗播放 03-charlie"
send "o"
tick 1
send "$WORK_DIR/dir2<Enter>"
if ! wait_for_queue 2 30; then
    bad "'o' 打开 dir2 后核心队列不是 2 条（实测 $(queue_paths | wc -l) 条）"
    diag "C-open"
else
    ok "'o' 打开目录生效：核心队列换成 dir2 的 2 条"
fi

# 换目录时前端会 player_stop()，此处先确认播放确实停了
tick 1
hist_before="$(db_query "select count(*) from play_history" 2>/dev/null)"
case "$hist_before" in
    ''|*[!0-9]*) bad "读不到 play_history 行数（'$hist_before'）" ; diag "C-hist" ;;
    0) bad "播过 03-charlie 之后 play_history 仍然是 0 行（add_history_entry 链路没写进去）"
       diag "C-hist" ;;
    *) ok "play_history 已有 $hist_before 行（A 场景的播放被记进历史）" ;;
esac

send "h"
tick 1
send "<Enter>"
if wait_for_play "03-charlie.wav" 12; then
    ok "历史弹窗 Enter 直播成功：核心游标回到 03-charlie.wav"
else
    bad "历史弹窗 Enter 之后核心游标不是 03-charlie.wav（实测 '${CUR:-空}'）"
    diag "C-play"
fi
state="$(play_state)"
if [ "$state" = "playing" ] || [ "$state" = "Playing" ]; then
    ok "历史播放后核心在播（playback.state=$state）"
else
    bad "历史播放后核心状态='$state'（期望 playing）"
    diag "C-play"
fi
# 直播的那首必须真的进了核心队列（append_playlist → 门面重推）
case "$(queue_paths)" in
    *"$WORK_DIR/dir1/03-charlie.wav"*) ok "03-charlie.wav 被追加进核心队列（不在 dir2 内容里也能播）" ;;
    *) bad "核心队列里没有 03-charlie.wav，追加链路没生效" ; diag "C-play" ;;
esac
hist_newest="$(db_query "select track_path from play_history order by played_at desc, id desc limit 1" 2>/dev/null)"
if [ "$hist_newest" = "$WORK_DIR/dir1/03-charlie.wav" ]; then
    ok "SQLite 最新一行历史就是 03-charlie.wav"
else
    bad "SQLite 最新历史行='$hist_newest'（期望 $WORK_DIR/dir1/03-charlie.wav）"
    diag "C-hist"
fi

# ── 场景 B：收藏视图播放未加载的曲目 ─────────────────────────────
info "B F5 收藏视图 → Tab 进内容区 → Enter 播放未加载的 02-bravo.wav"
send "<F5>"
tick 2
send "<Tab>"
tick 1
send "<Enter>"
if wait_for_play "02-bravo.wav" 12; then
    ok "收藏视图 Enter 播出了 02-bravo.wav（原本不在任何已加载目录里）"
else
    bad "收藏视图 Enter 之后核心游标不是 02-bravo.wav（实测 '${CUR:-空}'）"
    diag "B"
fi
state="$(play_state)"
if [ "$state" = "playing" ] || [ "$state" = "Playing" ]; then
    ok "收藏直播后核心在播（playback.state=$state）"
else
    bad "收藏直播后核心状态='$state'（期望 playing）"
    diag "B"
fi
case "$(queue_paths)" in
    *"$WORK_DIR/dir1/02-bravo.wav"*) ok "02-bravo.wav 经 append_playlist 进入核心队列" ;;
    *) bad "核心队列里没有 02-bravo.wav（收藏直播兜底没生效）" ; diag "B" ;;
esac

# ── 场景 D：搜索 → 播放结果 ──────────────────────────────────────
info "D '/' 搜索 'zulu' → 结果里 Enter 播放"
send "/"
tick 1
send "zulu"
tick 1
send "<Enter>"
tick 3            # 异步搜索：等结果落地再按 Enter，否则会撞上 in_progress 分支
send "<Enter>"
if wait_for_play "11-zulu.wav" 12; then
    ok "搜索结果 Enter 播出了 11-zulu.wav"
else
    bad "搜索后核心游标不是 11-zulu.wav（实测 '${CUR:-空}'）"
    diag "D"
fi
state="$(play_state)"
if [ "$state" = "playing" ] || [ "$state" = "Playing" ]; then
    ok "搜索播放后核心在播（playback.state=$state）"
else
    bad "搜索播放后核心状态='$state'（期望 playing）"
    diag "D"
fi

# ── 场景 M：曲库浏览器（M 键）→ 导航 → Enter 播放 ────────────────
# browser.c 语义：home 视图只用方向键上下（KEY_DOWN）+ Enter 进入子视图，
# ESC 返回上层/退出；track 列表（LIBRARY_ALL_TRACKS）Enter = 播放第 0 行。
# 第 0 行 = library_get_all_track_rowids 的 rowids[0]，其查询是
# "SELECT rowid FROM tracks ORDER BY title COLLATE NOCASE"——断言用同一条
# ORDER BY 从库里反查期望路径。播放链路：路径 → playlist_find_track_index_by_path
# → 未命中 append_playlist → ui_play_track_index（前端门面，不直调 play_audio）。
# 方向键必须发 SS3 形态（\eO[B A）：真终端里 keypad(TRUE) 发 smkx 后 xterm
# 就发这个形态，ncurses 按键树里登记的也是 terminfo kcud1=\EOB；驱动 <Down>
# 的 CSI 形态（\e[B）属"普通模式"，实测（探针 kbprobe）在该 pty 里会拆成
# ESC/'['/'B' 三个裸键，浏览器会把 ESC 当退出——所以这里用 \eOB。
send_down() { printf 'key:\\eOB\n' >> "$CTL_FILE" 2>/dev/null || true; }

info "M 曲库浏览器：M 进入 → 方向下 x3 到 All Tracks → Enter → Enter 播放第 0 行"
TARGET_M="$(db_query 'select path from tracks order by title collate nocase limit 1' 2>/dev/null)"
if [ -z "$TARGET_M" ]; then
    bad "曲库 tracks 表为空（前序场景的 library_scan_file 应已入库），M 场景无从断言"
    diag "M"
else
    printf '         期望直播: %s\n' "$TARGET_M"
    send "M"                      # 进入浏览器 home（M 激活视图，只渲染不接键＝修复前缺陷）
    tick 1
    send_down
    send_down
    send_down                     # selected_index 0→3 = All Tracks（home 只认方向键）
    tick 1
    send "<Enter>"                # 进入 LIBRARY_ALL_TRACKS
    tick 1
    send "<Enter>"                # 播放第 0 行（Enter 播完即退出浏览器）
    if wait_for_play "${TARGET_M##*/}" 12; then
        ok "浏览器 Enter 直播成功：核心游标在 ${TARGET_M##*/}"
    else
        bad "浏览器 Enter 之后核心游标不是 ${TARGET_M##*/}（实测 '${CUR:-空}'）"
        diag "M"
    fi
    case "$(queue_paths)" in
        *"$TARGET_M"*) ok "目标曲目出现在核心队列（find/append → 门面下发）" ;;
        *) bad "核心队列里没有 $TARGET_M" ; diag "M" ;;
    esac
    state="$(play_state)"
    if [ "$state" = "playing" ] || [ "$state" = "Playing" ]; then
        ok "浏览器播放后核心在播（playback.state=$state）"
    else
        bad "浏览器播放后核心状态='$state'（期望 playing）"
        diag "M"
    fi

    # 再进一次并用 ESC 退出：验证浏览器视图的 ESC 语义接回且不退化成死视图
    send "M"
    tick 1
    send "<Esc>"                  # home 视图 ESC = library_browser_exit
    tick 1
    if session_alive && tui_alive; then
        ok "M 进入后 ESC 退出：前端会话仍存活（浏览器可进可出）"
    else
        bad "M/ESC 之后前端会话没了"
        diag "M-esc"
    fi
fi

# ── 收尾：q 退出前端，核心必须还在（与 lifecycle-e2e 的 q 断言同口径）──
info "收尾：'q' 退出前端"
send "q"
if wait_session_stop; then
    tui_rc="$(cat "$WORK_DIR/tui.rc" 2>/dev/null)"
    if tui_alive; then
        bad "前端 'q' 之后 TUI 进程仍在（pid $(cat "$WORK_DIR/tui.pid" 2>/dev/null)）"
        diag "quit"
    elif [ "$tui_rc" = "0" ]; then
        ok "前端 'q' 正常退出（退出码 0）"
    else
        bad "前端 'q' 退出码不是 0（实测 '${tui_rc:-空}'）"
        diag "quit"
    fi
else
    bad "前端 'q' 之后驱动会话仍未收尾（按键会话卡住）"
    diag "quit"
fi
if daemon_up; then
    ok "前端退出后核心仍在总线上（daemon status 可见）"
else
    bad "前端退出后核心也没了"
fi
# 万一 'q' 没生效（会话还挂着），显式让驱动收尾，不给脚本留孤儿
printf 'kill\n' >> "$CTL_FILE" 2>/dev/null || true
wait_session_stop || true
DRIVER_PID=""

info "结果"
printf '%d 通过, %d 失败, %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -ne 0 ]; then
    KEEP=1
    printf '（工作目录保留在 %s，TUI 按键捕获在 %s/tui.log）\n' \
        "$WORK_DIR" "$WORK_DIR" >&2
fi
[ "$FAIL" -eq 0 ]
