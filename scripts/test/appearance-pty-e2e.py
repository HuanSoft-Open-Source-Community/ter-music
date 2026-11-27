#!/usr/bin/env python3
"""pty 运行期验证：终端亮/暗自动适配（外观模式 / OSC 11 / 回退链 / 重探）。

被测程序必须有 pty；每个场景都在全新的临时 HOME / XDG_CONFIG_HOME /
XDG_CACHE_HOME 中运行，绝不触碰真实用户配置。场景用显式
`--frontend=local` 启动，不依赖 D-Bus。

用法：
  scripts/test/appearance-pty-e2e.py [--bin build/ter-music] [--only 场景名]

退出码：0 = 全部通过；1 = 有失败。
"""

import argparse
import fcntl
import os
import pty
import re
import select
import shutil
import signal
import sqlite3
import struct
import sys
import tempfile
import termios
import threading
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vt_grid import Screen  # noqa: E402

SGR = re.compile(rb"\x1b\[([0-9;]*)m")

# 当前配置契约版本（schema.h/types.h 的 CONFIG_CURRENT_VERSION）。
# 夹具默认写当前版本；迁移场景显式传入旧版本号。
CONFIG_CURRENT_VERSION = 12

# 亮色变体专属值（暗色表与旧版一致，绝不出现这些值）
LIGHT_ONLY = {236, 94, 28, 24, 90, 25, 126}
# 暗色预设专属值（亮色变体不会产生）
DARK_ONLY = {37, 198, 208, 75, 118, 203, 93, 14, 11, 13}


class Result:
    def __init__(self):
        self.out = b""
        self.queries = 0
        self.replies = 0
        self.focus_sent = False
        self.status = None
        self.saved = ""
        self.colours = set()
        self.screens = {}
        self.latencies = []   # 每次 latency_keys 输入到下一个 pty 输出的秒数


def _write_config(root, mode, theme_extra, prefs_extra, config_version):
    cfg_dir = os.path.join(root, "config", "ter-music")
    os.makedirs(cfg_dir, exist_ok=True)
    inner = ""
    if mode is not None:
        inner += f"<appearance_mode>{mode}</appearance_mode>"
    inner += prefs_extra
    version = config_version or CONFIG_CURRENT_VERSION
    with open(os.path.join(cfg_dir, "config.xml"), "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
        f.write(f'<ter-music-config version="{version}">\n')
        if theme_extra:
            f.write(f"<theme>{theme_extra}</theme>\n")
        if inner:
            f.write(f"<preferences>{inner}</preferences>\n")
        f.write("</ter-music-config>\n")
    return cfg_dir


def run_case(binary, name, mode=None, reply_after=None, reply=b"", replies=None,
             reply_parts=None, colorfgbg=None, theme_extra="", prefs_extra="",
             config_version=None, duration=3.6, key_steps=None,
             user_layout=None, layout_sel=None, focus_at=None, term=None,
             extra_args=None, snapshots=None, prep=None, wrapper=None,
             latency_keys=None):
    """运行一个 pty 场景，返回 Result（含原始输出、探测次数与落盘配置）。"""
    root = tempfile.mkdtemp(prefix="tm-appearance-", dir="/tmp")
    home = os.path.join(root, "home")
    cache = os.path.join(root, "cache")
    os.makedirs(home)
    os.makedirs(cache)
    cfg_dir = _write_config(root, mode, theme_extra, prefs_extra, config_version)
    if layout_sel:
        with open(os.path.join(cfg_dir, "ui-layout"), "w", encoding="utf-8") as f:
            f.write(layout_sel + "\n")
    if user_layout:
        with open(os.path.join(cfg_dir, "layouts.json"), "w", encoding="utf-8") as f:
            f.write(user_layout)

    env = {
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "TERM": term or os.environ.get("TERM", "xterm-256color"),
        "HOME": home,
        "XDG_CONFIG_HOME": os.path.join(root, "config"),
        "XDG_CACHE_HOME": cache,
        "LANG": "C.UTF-8",
        "LANGUAGE": "C",
    }
    # 在 dbus-run-session 下运行时让子进程共用同一个 session bus（XDG portal 测试）。
    if os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        env["DBUS_SESSION_BUS_ADDRESS"] = os.environ["DBUS_SESSION_BUS_ADDRESS"]
    if colorfgbg is not None:
        env["COLORFGBG"] = colorfgbg

    if prep:
        prep(binary, env, cfg_dir, root)

    result = Result()
    snap_list = sorted(snapshots or [])
    snap_taken = []
    reply_queue = list(replies) if replies is not None else ([reply] if reply else [])
    parts_left = list(reply_parts) if reply_parts else []
    parts_started = None
    parts_sent = 0
    keys_left = list(key_steps) if key_steps else []
    lat_keys = list(latency_keys) if latency_keys else []
    lat_pending = []
    focus_sent = False

    pid, fd = pty.fork()
    if pid == 0:
        argv = [binary, "--frontend=local"] + list(extra_args or [])
        if wrapper:
            argv = list(wrapper) + argv
        try:
            os.execvpe(argv[0], argv, env)
        finally:
            os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))

    started = time.time()
    reply_at = None
    while time.time() - started < duration:
        elapsed = time.time() - started
        while snap_list and elapsed >= snap_list[0][0]:
            snap_taken.append((snap_list[0][1], len(result.out)))
            snap_list.pop(0)
        if keys_left and elapsed >= keys_left[0][0]:
            os.write(fd, keys_left.pop(0)[1])
        while lat_keys and elapsed >= lat_keys[0][0]:
            _t, payload, _label = lat_keys.pop(0)
            os.write(fd, payload)
            lat_pending.append(time.time())
        if focus_at is not None and not focus_sent and elapsed >= focus_at:
            os.write(fd, b"\x1b[I")
            focus_sent = True
        if parts_left and parts_started is not None and \
                time.time() - parts_started >= 0.05 * parts_sent:
            os.write(fd, parts_left.pop(0))
            parts_sent += 1
        if reply_queue and reply_at is not None and \
                (reply_after is None or time.time() - reply_at >= reply_after):
            os.write(fd, reply_queue.pop(0))
            result.replies += 1
            reply_at = None
        try:
            ready, _, _ = select.select([fd], [], [], 0.03)
        except InterruptedError:
            ready = []
        if not ready:
            continue
        try:
            data = os.read(fd, 65536)
        except OSError:
            break
        if not data:
            break
        result.out += data
        if lat_pending:
            now_read = time.time()
            result.latencies.extend(now_read - t0 for t0 in lat_pending)
            lat_pending.clear()
        if b"\x1b]11;?" in data:
            result.queries += 1
            reply_at = time.time()
            if parts_left and parts_started is None:
                parts_started = time.time()
        if elapsed > duration - 0.6:
            os.write(fd, b"q")

    for _ in range(40):
        try:
            wpid, status = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            break
        if wpid == pid:
            result.status = status
            break
        os.write(fd, b"q")
        time.sleep(0.05)
    else:
        try:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except OSError:
            pass

    try:
        os.close(fd)
    except OSError:
        pass

    for match in SGR.finditer(result.out):
        parts = match.group(1).decode().split(";")
        for i, part in enumerate(parts):
            if part == "38" and i + 2 < len(parts) and parts[i + 1] == "5":
                try:
                    result.colours.add(int(parts[i + 2]))
                except ValueError:
                    pass

    for label, length in snap_taken:
        screen = Screen(24, 80)
        screen.feed(result.out[:length])
        # compressed=True 去掉宽字符占位空格，便于中文子串断言。
        result.screens[label] = [line.rstrip() for line in screen.lines(compressed=True)]

    config_path = os.path.join(cfg_dir, "config.xml")
    if os.path.exists(config_path):
        result.saved = open(config_path, encoding="utf-8").read()
    result.cfg_dir = cfg_dir
    result.root = root
    return result


def saved_value(saved, tag):
    match = re.search(rf"<{tag}>(-?[0-9]+)</{tag}>", saved)
    return int(match.group(1)) if match else None


def quick_tui(binary, env, seconds=3.0):
    """跑一次 TUI 再 q 退出；用于让程序自己建好 library.db 骨架。"""
    pid, fd = pty.fork()
    if pid == 0:
        try:
            os.execvpe(binary, [binary, "--frontend=local"], env)
        finally:
            os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            ready, _, _ = select.select([fd], [], [], 0.05)
            if ready:
                os.read(fd, 65536)
        except OSError:
            break
    for _ in range(30):
        try:
            wpid, _status = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            break
        if wpid == pid:
            break
        os.write(fd, b"q")
        time.sleep(0.05)
    else:
        try:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except OSError:
            pass
    try:
        os.close(fd)
    except OSError:
        pass


def write_test_wav(path):
    """写一个 1 秒 440 Hz 单声道 WAV；FFmpeg 扫描器可以直接读。"""
    with wave.open(path, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(8000)
        frames = bytearray()
        for i in range(8000):
            sample = int(12000 * __import__("math").sin(2 * 3.141592653589793 * 440 * i / 8000))
            frames += struct.pack("<h", sample)
        wav.writeframes(bytes(frames))


def deepin_write_file(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def deepin_colorscheme(bg):
    return f"[Background]\nColor={bg}, {bg}, {bg}\n"


def deepin_theme_setup(root, theme="Light", expand="",
                       light_bg=248, dark_bg=37,
                       custom_bg=None, named_name=None, named_bg=250,
                       write_basic=True):
    """在临时 HOME/XDG_CONFIG_HOME 下伪造 deepin-terminal 主题文件。"""
    cfg = os.path.join(root, "config", "deepin", "deepin-terminal")
    deepin_write_file(
        os.path.join(cfg, "config.conf"),
        "[basic.interface.theme]\n"
        f"value={theme}\n"
        "[basic.interface.expand_theme]\n"
        f"value={expand}\n")
    schemes = os.path.join(root, "home", ".local", "share",
                           "terminalwidget6", "color-schemes")
    if write_basic:
        deepin_write_file(os.path.join(schemes, "Light.colorscheme"),
                          deepin_colorscheme(light_bg))
        deepin_write_file(os.path.join(schemes, "Dark.colorscheme"),
                          deepin_colorscheme(dark_bg))
    if named_name:
        deepin_write_file(os.path.join(schemes, named_name + ".colorscheme"),
                          deepin_colorscheme(named_bg))
    if custom_bg is not None:
        deepin_write_file(os.path.join(cfg, "customTheme.colorscheme"),
                          deepin_colorscheme(custom_bg))
    return cfg


def prepare_library(root, binary, env, cfg_dir):
    """建库并注入一条曲目/一个歌单/一条播放历史，供两个模态弹层测试。

    注意：现有 library.c 的 schema DDL 会无条件 `ALTER TABLE tracks ADD COLUMN
    lyrics_source`，同一份 DB 第二次启动会报 duplicate column 并让内容库不可用
    （既有基线行为，不在本任务范围）。本用例需要“先建库 → 注入夹具 → 再跑
    UI”，所以在注入前把该列与 v3 版本标记退回，让下一次启动的 ALTER 重新成功。
    """
    quick_tui(binary, env, 3.0)
    db_path = os.path.join(cfg_dir, "library.db")
    try:
        conn = sqlite3.connect(db_path)
        conn.execute("ALTER TABLE tracks DROP COLUMN lyrics_source")
        conn.execute("DELETE FROM schema_version WHERE version = 3")
        conn.commit()
        conn.close()
    except sqlite3.DatabaseError as exc:
        raise RuntimeError(f"cannot rewind test library schema: {exc}") from exc
    music_dir = os.path.join(root, "music")
    os.makedirs(music_dir, exist_ok=True)
    wav_path = os.path.realpath(os.path.join(music_dir, "tone.wav"))
    write_test_wav(wav_path)
    stat = os.stat(wav_path)
    now = int(time.time())
    conn = sqlite3.connect(db_path)
    conn.execute(
        "INSERT OR IGNORE INTO tracks(path,title,artist,album,genre,year,"
        "duration_seconds,mtime,file_size,added_at,last_played_at,play_count) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        (wav_path, "Regression Tone", "tester", "tests", "", 2026, 1,
         int(stat.st_mtime), stat.st_size, now, 0, 0))
    conn.execute(
        "INSERT OR IGNORE INTO playlists(name,created_at,modified_at) VALUES (?,?,?)",
        ("Regression Playlist", now, now))
    conn.execute(
        "INSERT INTO play_history(track_path,played_at,position) VALUES (?,?,?)",
        (wav_path, now, 0))
    conn.commit()
    conn.close()
    return music_dir


def screen_text(result, label):
    return "\n".join(result.screens.get(label, []))


def favorites_count(cfg_dir):
    db_path = os.path.join(cfg_dir, "library.db")
    if not os.path.exists(db_path):
        return 0
    conn = sqlite3.connect(db_path)
    try:
        row = conn.execute("SELECT COUNT(*) FROM favorites").fetchone()
        return row[0] if row else 0
    except sqlite3.DatabaseError:
        return 0
    finally:
        conn.close()


def run_suite(binary, only=None):
    cases = []

    def case(fn):
        if only is None or fn.__name__ == only:
            cases.append(fn)
        return fn

    @case
    def explicit_light():
        r = run_case(binary, "explicit-light", mode=1)
        checks = [
            (r.queries == 0, "显式亮色不探测终端（零 I/O）"),
            (bool(r.colours & {236, 94, 28, 24, 90}), "亮色变体 SGR 出现"),
            (not (r.colours & {37, 198, 208}), "亮色下不出现暗色预设专属色"),
            (saved_value(r.saved, "appearance_mode") == 1, "appearance_mode=1 落盘"),
        ]
        return r, checks

    @case
    def explicit_dark():
        r = run_case(binary, "explicit-dark", mode=2)
        checks = [
            (r.queries == 0, "显式暗色不探测终端"),
            (bool(r.colours & {37, 198, 208}), "暗色表与旧版一致"),
            (not (r.colours & {236, 94, 28, 90}), "暗色下不出现亮色变体色"),
            (saved_value(r.saved, "appearance_mode") == 2, "appearance_mode=2 落盘"),
        ]
        return r, checks

    @case
    def auto_white():
        r = run_case(binary, "auto-white", mode=0,
                     reply=b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\", reply_after=0.0)
        checks = [
            (r.queries >= 1, "auto 启动时发送 OSC 11 查询"),
            (r.replies == 1, "应答被读入"),
            (bool(r.colours & {236, 94, 28, 24, 90}), "白底 → 亮色变体"),
            (b"]11;rgb:" not in r.out, "应答文本不上屏"),
        ]
        return r, checks

    @case
    def auto_black():
        r = run_case(binary, "auto-black", mode=0,
                     reply=b"\x1b]11;rgb:0000/0000/0000\x1b\\", reply_after=0.0)
        checks = [
            (r.queries >= 1, "auto 启动时发送 OSC 11 查询"),
            (bool(r.colours & {37, 198, 208}), "黑底 → 保持暗色"),
            (not (r.colours & {236, 94, 28, 90}), "黑底不出现亮色变体"),
        ]
        return r, checks

    @case
    def no_reply():
        r = run_case(binary, "no-reply", mode=0)
        checks = [
            (r.queries >= 1, "无应答终端仍会询问一次"),
            (bool(r.colours & {37, 198, 208}), "超时降级为暗色"),
            (b"]11;rgb:" not in r.out, "不上屏"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def colorfgbg():
        r = run_case(binary, "colorfgbg", mode=0, colorfgbg="0;15")
        checks = [
            (bool(r.colours & {236, 94, 28, 24, 90}), "COLORFGBG=0;15 回退为亮色"),
            (not (r.colours & {37, 198, 208}), "回退链结果一致"),
        ]
        return r, checks

    @case
    def late_reply():
        r = run_case(binary, "late-reply", mode=0,
                     reply=b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\", reply_after=0.9)
        checks = [
            (b"]11;rgb:" not in r.out, "迟到应答不上屏（常驻过滤器吞掉）"),
            (r.status == 0, "进程存活并正常退出"),
            (bool(r.colours & {236, 94, 28, 24, 90}), "迟到应答仍能触发变体重算"),
        ]
        return r, checks

    @case
    def half_packet():
        r = run_case(binary, "half-packet", mode=0,
                     reply_parts=[b"\x1b]11;rgb:ffff/", b"ffff/ffff\x1b\\"])
        checks = [
            (bool(r.colours & {236, 94, 28, 24, 90}), "半包应答被拼完整并生效"),
            (b"]11;rgb:" not in r.out, "半包字节不上屏"),
        ]
        return r, checks

    @case
    def focus_redetect():
        r = run_case(binary, "focus-redetect", mode=0,
                     replies=[b"\x1b]11;rgb:0000/0000/0000\x1b\\",
                              b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\"],
                     focus_at=2.7, duration=5.2)
        checks = [
            (r.queries >= 2, "焦点进入触发第二次查询（无需重启）"),
            (bool(r.colours & {236, 94, 28, 24, 90}), "重探后切换为亮色"),
            (b"]11;rgb:" not in r.out, "应答不上屏"),
        ]
        return r, checks

    @case
    def input_safety():
        r = run_case(binary, "input-safety", mode=0,
                     reply=b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\", reply_after=0.0,
                     key_steps=[(0.05, b"j"), (0.4, b"j12"), (0.8, b"\t")])
        checks = [
            (b"]11;rgb:" not in r.out, "启动探测窗口内的按键不会把应答读成文本"),
            (r.status == 0, "进程正常退出"),
            (bool(r.colours & {236, 94, 28, 24, 90}), "探测仍正常完成"),
        ]
        return r, checks

    @case
    def anchorless_reply():
        # 第二道防线：ESC 已被别处消费；约 1s 后到达的合法 anchorless 余片
        # 仍须在“查询应答窗口”内被整段吞掉并触发变体重算（且不上屏）。
        r = run_case(binary, "anchorless-reply", mode=0,
                     duration=4.6,
                     snapshots=[(4.0, "after_reply")],
                     key_steps=[(3.0, b"]11;rgb:ffff/ffff/ffff\x07")])
        screen = screen_text(r, "after_reply")
        checks = [
            (r.queries >= 1, "启动探测已发出 OSC11 查询（窗口开启）"),
            ("rgb:" not in screen and "]11;" not in screen, "无 ESC 锚点的余字节不上屏"),
            (bool(r.colours & {236, 94, 28, 24, 90}), "迟到 anchorless 余片被识别为应答并完成变体重算"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def anchorless_reply_st():
        # t11 F1：ST（ESC '\\'）终止的 anchorless 应答必须与 BEL 一样被整段吞掉。
        r = run_case(binary, "anchorless-reply-st", mode=0, duration=4.6,
                     snapshots=[(4.0, "after_reply")],
                     key_steps=[(3.0, b"]11;rgb:ffff/ffff/ffff\x1b\\")])
        screen = screen_text(r, "after_reply")
        checks = [
            (r.queries >= 1, "启动探测已发出 OSC11 查询（窗口开启）"),
            ("rgb:" not in screen and "]11;" not in screen, "ST anchorless 应答不上屏"),
            (bool(r.colours & {236, 94, 28, 24, 90}),
             "ST anchorless 应答被识别并触发亮色变体"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def literal_main_loop():
        # O1：查询窗口内输入字面量 `]11;` 不得被吞；随后的 q 必须仍能退出。
        r = run_case(binary, "literal-main", mode=0, duration=4.6,
                     snapshots=[(3.4, "main_after")],
                     key_steps=[(2.8, b"]11;"), (3.6, b"q")])
        screen = screen_text(r, "main_after")
        checks = [
            ("播放列表" in screen, "主界面仍在渲染（字面量未造成卡死）"),
            (r.status == 0, "字面量后的 q 仍被主分发收到（未吞键/未卡死）"),
        ]
        return r, checks

    @case
    def literal_search_input():
        # O1：查询窗口内，搜索框输入/粘贴 `]11;hello` 与无终止符的
        # `]11;rgb:...` 文本都必须逐字上屏，不吞字、不取消模态。
        # v12 起主列表搜索要求文件浏览模式已有曲目（空队列按 / 只提示先打开目录），
        # 所以先建一个最小内容库再启动。
        extra = []

        def prep(binary, env, cfg_dir, root):
            music = prepare_library(root, binary, env, cfg_dir)
            extra.extend(["-o", music])

        r = run_case(binary, "literal-search", mode=0, prep=prep,
                     extra_args=extra, duration=7.2,
                     snapshots=[(3.6, "typed_literal"), (5.5, "typed_reply_like")],
                     key_steps=[(2.6, b"/"),
                                (2.9, b"]11;hello"),
                                (4.0, b"\x1b"),
                                (4.4, b"/"),
                                (4.8, b"]11;rgb:1234/5678/9abc"),
                                (5.9, b"\x1b"),
                                (6.3, b"q")])
        typed1 = screen_text(r, "typed_literal")
        typed2 = screen_text(r, "typed_reply_like")
        checks = [
            ("搜索歌曲" in typed1 and "]11;hello" in typed1,
             "字面量 ]11;hello 逐字送达搜索框"),
            ("搜索已取消" not in typed1, "字面量未取消搜索模态"),
            ("搜索歌曲" in typed2 and "]11;rgb:1234/5678/9abc" in typed2,
             "无终止符的 ]11;rgb:... 文本逐字送达搜索框"),
            (r.status == 0, "字面量输入后正常退出"),
        ]
        return r, checks

    @case
    def literal_help_input():
        # O1：帮助搜索框（窄字节输入）同样不吞字面量。
        r = run_case(binary, "literal-help", mode=0, duration=5.8,
                     snapshots=[(4.0, "typed")],
                     key_steps=[(2.6, b"\x1b[19~"), (3.0, b"/"),
                                (3.3, b"]11;hello"),
                                (4.4, b"\x1b"), (4.8, b"q")])
        typed = screen_text(r, "typed")
        checks = [
            ("搜索歌曲" in typed and "]11;hello" in typed,
             "字面量 ]11;hello 逐字送达帮助搜索框"),
            (r.status == 0, "帮助搜索字面量后正常退出"),
        ]
        return r, checks

    @case
    def literal_language_input():
        # O1：语言包路径框（窄字节输入）同样不吞字面量。
        # v12 起“添加语言包”走统一文件选择器，提示为 file_picker 的
        # “文件路径（…）”；提示串比旧版长，字面量本身在 100 列屏上不一定可见，
        # 故用“把字面量作为路径提交 → 添加失败”证明它确实进入了输入缓冲。
        r = run_case(binary, "literal-language", mode=0, duration=7.8,
                     snapshots=[(4.0, "typed"), (5.4, "after_enter")],
                     key_steps=[(2.6, b"\x1b[18~"), (3.0, b"a"),
                                (3.3, b"]11;hello"),
                                (4.6, b"\n"),
                                (6.2, b"\x1b"),
                                (7.0, b"q")])
        typed = screen_text(r, "typed")
        after = screen_text(r, "after_enter")
        checks = [
            ("文件路径（" in typed, "语言包路径框已打开（统一文件选择器）"),
            ("rgb:" not in typed and "]11;" not in typed, "应答文本未注入路径框/屏幕"),
            ("添加失败" in after,
             "字面量 ]11;hello 作为路径提交（未被当作终端应答吞掉）"),
            (r.status == 0, "语言包路径字面量后正常退出"),
        ]
        return r, checks

    @case
    def modal_late_reply():
        # t5 F1：焦点查询后约 1.4s 才到的合法应答落在帮助搜索框打开期间，
        # 仍须被过滤：模态不取消、无 rgb:/]11; 注入、播放模式/收藏不变。
        r = run_case(binary, "modal-late-reply", mode=0, duration=7.2,
                     snapshots=[(4.6, "modal_open"), (5.9, "main_after")],
                     key_steps=[(2.5, b"\x1b[I"),          # focus -> new OSC11 query
                                (2.9, b"\x1b[19~"),        # F8 -> Help
                                (3.3, b"/"),                 # open help search
                                (3.9, b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\"),
                                (5.0, b"\x1b"),             # close search
                                (5.4, b"\x1bOP"),           # F1 -> main
                                (6.4, b"q")])
        modal = screen_text(r, "modal_open")
        main_text = screen_text(r, "main_after")
        checks = [
            ("搜索歌曲" in modal, "迟到合法应答到达后帮助搜索框仍在（未被取消）"),
            ("rgb:" not in modal and "]11;" not in modal, "应答文本未注入模态/屏幕"),
            ("顺序播放" in main_text, "播放模式未被应答改变"),
            (favorites_count(r.cfg_dir) == 0, "收藏未被应答改变"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def modal_search_anchorless_st():
        # t11 F1：搜索框打开期间到达 ST 终止的 anchorless 应答：模态保持、
        # 无文本注入、无“搜索已取消”。
        extra = []

        def prep(binary, env, cfg_dir, root):
            music = prepare_library(root, binary, env, cfg_dir)
            extra.extend(["-o", music])

        r = run_case(binary, "modal-search-st", mode=0, duration=6.2,
                     extra_args=extra,
                     snapshots=[(4.6, "modal_open")],
                     key_steps=[(2.9, b"\x1b[I"),          # fresh query window
                                (3.2, b"/"),
                                (3.8, b"]11;rgb:ffff/ffff/ffff\x1b\\"),
                                (5.2, b"\x1b"),
                                (5.8, b"q")],
                     prep=prep)
        modal = screen_text(r, "modal_open")
        checks = [
            ("搜索歌曲" in modal, "ST anchorless 应答到达后搜索框仍在"),
            ("搜索已取消" not in modal, "未出现“搜索已取消”"),
            ("rgb:" not in modal and "]11;" not in modal, "应答文本未注入搜索框/屏幕"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def modal_help_anchorless_st():
        # t11 F1：帮助搜索框打开期间到达 ST 终止的 anchorless 应答。
        r = run_case(binary, "modal-help-st", mode=0, duration=6.0,
                     snapshots=[(4.4, "modal_open")],
                     key_steps=[(2.5, b"\x1b[I"),          # fresh query window
                                (2.7, b"\x1b[19~"),
                                (3.0, b"/"),
                                (3.5, b"]11;rgb:ffff/ffff/ffff\x1b\\"),
                                (5.0, b"\x1b"),
                                (5.5, b"q")])
        modal = screen_text(r, "modal_open")
        checks = [
            ("搜索歌曲" in modal, "ST anchorless 应答到达后帮助搜索框仍在"),
            ("rgb:" not in modal and "]11;" not in modal, "应答文本未注入帮助搜索框/屏幕"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def modal_language_anchorless_st():
        # t11 F1：语言包路径框打开期间到达 ST 终止的 anchorless 应答。
        # v12 起添加语言包走统一文件选择器（提示“文件路径（…”）：选择器用
        # ENTER(空输入)取消，ESC 只被当作转义序列前缀消费，不是取消键。
        r = run_case(binary, "modal-language-st", mode=0, duration=7.0,
                     snapshots=[(4.4, "modal_open")],
                     key_steps=[(2.5, b"\x1b[I"),          # fresh query window
                                (2.7, b"\x1b[18~"),
                                (3.0, b"a"),
                                (3.5, b"]11;rgb:ffff/ffff/ffff\x1b\\"),
                                (5.0, b"\n"),          # 空输入 = 取消选择器
                                (6.1, b"q")])
        modal = screen_text(r, "modal_open")
        checks = [
            ("文件路径（" in modal, "ST anchorless 应答到达后语言包路径框仍在"),
            ("rgb:" not in modal and "]11;" not in modal, "应答文本未注入路径框/屏幕"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_light_start():
        # t22：deepin-terminal 自身主题 Light → 亮色；Settings 来源“终端主题”。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "deepin-terminal"
            deepin_theme_setup(root, theme="Light")

        r = run_case(binary, "deepin-light", mode=0, prep=prep, duration=4.4,
                     snapshots=[(3.9, "settings")],
                     key_steps=[(2.7, b"\x1bOQ"), (3.1, b"\x1bOC")])
        settings = screen_text(r, "settings")
        checks = [
            (bool(r.colours & LIGHT_ONLY), "deepin 主题 Light 启动即亮色"),
            (not (r.colours & {37, 198, 208}), "亮色下不出现暗色预设专属色"),
            ("终端主题" in settings and "亮色" in settings,
             "Settings 检测行显示来源“终端主题”与亮色结论"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_dark_start():
        # t22：deepin-terminal 自身主题 Dark → 暗色（不误判）。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "deepin-terminal"
            deepin_theme_setup(root, theme="Dark")

        r = run_case(binary, "deepin-dark", mode=0, prep=prep, duration=3.2)
        checks = [
            (bool(r.colours & {37, 198, 208}), "deepin 主题 Dark 启动即暗色"),
            (not (r.colours & LIGHT_ONLY), "暗色下不出现亮色变体色"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_runtime_switch():
        # t22：运行期改写 config.conf（Light→Dark），≤3s 自动跟随并重绘。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "deepin-terminal"
            cfg = deepin_theme_setup(root, theme="Light")

            def switch():
                deepin_write_file(
                    os.path.join(cfg, "config.conf"),
                    "[basic.interface.theme]\nvalue=Dark\n"
                    "[basic.interface.expand_theme]\nvalue=\n")

            timer = threading.Timer(3.0, switch)
            timer.daemon = True
            timer.start()

        r = run_case(binary, "deepin-switch", mode=0, prep=prep, duration=7.4,
                     snapshots=[(6.6, "settings")],
                     key_steps=[(5.0, b"\x1bOQ"), (5.5, b"\x1bOC")])
        settings = screen_text(r, "settings")
        checks = [
            ("终端主题" in settings and "暗色" in settings,
             "运行期改写后 ≤3s 自动切换并在 Settings 显示暗色/终端主题"),
            ("亮色（终端主题）" not in settings,
             "Settings 最终不再显示旧亮色结论"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_expand_named():
        # t22：expand_theme 非空按名解析 terminalwidget6/color-schemes/<name>。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "deepin-terminal"
            deepin_theme_setup(root, theme="Dark", expand="My Solarized",
                               named_name="My Solarized", named_bg=250)

        r = run_case(binary, "deepin-named", mode=0, prep=prep, duration=3.2)
        checks = [
            (bool(r.colours & LIGHT_ONLY),
             "expand_theme 命名主题（bg=250）判定亮色"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_custom():
        # t22：expand_theme=customTheme → 配置目录 customTheme.colorscheme。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "deepin-terminal"
            deepin_theme_setup(root, theme="Light", expand="customTheme",
                               custom_bg=37)

        r = run_case(binary, "deepin-custom", mode=0, prep=prep, duration=3.2)
        checks = [
            (bool(r.colours & {37, 198, 208}) and not (r.colours & LIGHT_ONLY),
             "customTheme 的 bg=37 判定暗色"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_other_terminal_ignored():
        # t22：主题文件存在但身份不是 deepin-terminal → 适配器完全不启用。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "xterm"
            deepin_theme_setup(root, theme="Light")

        r = run_case(binary, "deepin-other", mode=0, prep=prep, duration=3.2)
        checks = [
            (not (r.colours & LIGHT_ONLY),
             "其它终端身份下不读取 deepin 主题（保持暗色回退）"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_term_program_authority():
        # t24 F1：TERM_PROGRAM=vscode 时，即使祖先链含 deepin-terminal 也不启用。
        helper = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "deepin-ancestor.py")

        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "vscode"
            deepin_theme_setup(root, theme="Light")

        r = run_case(binary, "deepin-authority", mode=0, prep=prep,
                     wrapper=[sys.executable, helper], duration=3.2)
        checks = [
            (not (r.colours & LIGHT_ONLY),
             "TERM_PROGRAM=vscode 权威：不读 deepin 祖先的主题"),
            (bool(r.colours & {37, 198, 208}), "冲突场景保持暗色回退"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_ancestor_when_term_program_unset():
        # t24 F1 正常路径：TERM_PROGRAM 未设置时才用祖先链（deepin 仍启用）。
        helper = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "deepin-ancestor.py")

        def prep(binary, env, cfg_dir, root):
            env.pop("TERM_PROGRAM", None)
            deepin_theme_setup(root, theme="Light")

        r = run_case(binary, "deepin-ancestor", mode=0, prep=prep,
                     wrapper=[sys.executable, helper], duration=3.2)
        checks = [
            (bool(r.colours & LIGHT_ONLY),
             "TERM_PROGRAM 未设置时祖先 deepin-terminal 启用适配器"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_no_xdg_data():
        # t22：没有 XDG_DATA_HOME/XDG_DATA_DIRS 时仍从 ~/.local/share 解析。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "deepin-terminal"
            env.pop("XDG_DATA_HOME", None)
            env.pop("XDG_DATA_DIRS", None)
            deepin_theme_setup(root, theme="Light")

        r = run_case(binary, "deepin-no-xdg", mode=0, prep=prep, duration=3.2)
        checks = [
            (bool(r.colours & LIGHT_ONLY),
             "无 XDG_DATA_HOME/DIRS 时仍解析 HOME/.local/share 主题"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_osc_priority():
        # t22：OSC11 实际底色仍高于终端主题适配器。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "deepin-terminal"
            deepin_theme_setup(root, theme="Light")

        r = run_case(binary, "deepin-osc", mode=0, prep=prep, duration=3.6,
                     replies=[b"\x1b]11;rgb:0000/0000/0000\x1b\\"])
        checks = [
            (bool(r.colours & {37, 198, 208}), "OSC11 暗色高于 deepin 主题亮色"),
            (not (r.colours & LIGHT_ONLY), "未被 deepin 主题改成亮色"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_explicit_priority():
        # t22：显式 Dark 模式不探测、不启用适配器。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "deepin-terminal"
            deepin_theme_setup(root, theme="Light")

        r = run_case(binary, "deepin-explicit", mode=2, prep=prep, duration=3.0)
        checks = [
            (r.queries == 0, "显式模式不探测终端（零 I/O）"),
            (bool(r.colours & {37, 198, 208}) and not (r.colours & LIGHT_ONLY),
             "显式 Dark 覆盖 deepin 主题 Light"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_manual_reprobe():
        # t22：手动「重新探测」也走终端主题适配器（显式模式下只更新来源/结论）。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "deepin-terminal"
            cfg = deepin_theme_setup(root, theme="Dark")

            def switch():
                deepin_write_file(
                    os.path.join(cfg, "config.conf"),
                    "[basic.interface.theme]\nvalue=Light\n"
                    "[basic.interface.expand_theme]\nvalue=\n")

            timer = threading.Timer(4.0, switch)
            timer.daemon = True
            timer.start()

        r = run_case(binary, "deepin-reprobe", mode=2, prep=prep,
                     duration=6.0, snapshots=[(5.4, "settings")],
                     key_steps=[(2.6, b"\x1bOQ"), (3.0, b"\x1bOC"),
                                (3.4, b"\x1bOA"),   # Re-detect row
                                (4.5, b"\n")])      # manual re-probe
        settings = screen_text(r, "settings")
        checks = [
            ("终端主题" in settings and "亮色" in settings,
             "手动重探走适配器并反馈亮色/终端主题"),
            (bool(r.colours & {37, 198, 208}),
             "显式 Dark 模式的最终变体不被适配器改写"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def deepin_theme_unknown_falls_back():
        # t22：适配器 unknown（主题文件缺失）→ 原链（COLORFGBG）照旧生效。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "deepin-terminal"
            deepin_theme_setup(root, theme="Light", write_basic=False)

        r = run_case(binary, "deepin-unknown", mode=0, prep=prep, duration=3.2,
                     colorfgbg="0;15")
        checks = [
            (bool(r.colours & LIGHT_ONLY),
             "适配器 unknown 时回退 COLORFGBG 亮色"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def stage_preset():
        r = run_case(binary, "stage-preset", mode=1, layout_sel="stage")
        checks = [
            ({236, 94, 24, 28, 90} <= r.colours, "stage 预设亮色表完整生效"),
            (not (r.colours & {37, 198, 208, 10}), "stage 亮色下无暗色值"),
        ]
        return r, checks

    two_pane_user = (
        '{"version":2,"default":"classic","layouts":[{'
        '"id":"classic","name":"Classic","tree":{"split":"row","ratio":500,'
        '"children":[{"pane":"playlist","fg":51,"bg":-1},'
        '{"pane":"info","fg":201,"bg":-1}]}}]}'
    )
    # 用户从内置 classic 预设复制出的布局：id 与预设同名、fg 仍等于暗色出厂值。
    classic_dark_tree = (
        '{"split":"row","ratio":680,"children":['
        '{"split":"col","ratio":520,"children":['
        '{"pane":"playlist","fg":15,"bg":-1},'
        '{"split":"col","ratio":542,"children":['
        '{"pane":"info","fg":37,"bg":-1},'
        '{"pane":"controls","fg":208,"bg":-1}]}]},'
        '{"split":"col","ratio":200,"children":['
        '{"pane":"spectrum","fg":10,"bg":-1},'
        '{"pane":"lyrics","fg":198,"bg":-1}]}]}'
    )
    classic_preset_copy = (
        '{"version":2,"default":"classic","layouts":[{"id":"classic",'
        '"name":"Classic","tree":' + classic_dark_tree + '}]}'
    )
    classic_modified = classic_preset_copy.replace(
        '"pane":"playlist","fg":15,', '"pane":"playlist","fg":51,')
    env_layout_user = (
        '{"version":2,"default":"envcopy","layouts":[{"id":"envcopy",'
        '"name":"Env Copy","tree":' + classic_dark_tree + '}]}'
    )

    @case
    def user_layout_protected():
        r = run_case(binary, "user-layout", mode=1, duration=2.8,
                     user_layout=two_pane_user)
        after = open(os.path.join(r.cfg_dir, "layouts.json"),
                     encoding="utf-8").read()
        checks = [
            (51 in r.colours and 201 in r.colours, "用户 layouts.json 显式色原样渲染"),
            (236 not in r.colours, "用户布局不参与亮色变体"),
            (after == two_pane_user, "用户 layouts.json 未被改写"),
        ]
        return r, checks

    @case
    def user_preset_copy_adapts():
        # B：USER 布局 id 与预设同名、fg 仍等于预设暗色出厂值 → 参与亮色变体。
        r = run_case(binary, "user-preset-copy", mode=0,
                     replies=[b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\"],
                     user_layout=classic_preset_copy, duration=3.8)
        checks = [
            ({236, 24, 94} <= r.colours,
             "用户预设副本的出厂 fg 命中亮色变体"),
            (15 not in r.colours and 37 not in r.colours,
             "命中值不再渲染为暗色出厂值"),
        ]
        return r, checks

    @case
    def user_modified_value_kept():
        # B：用户改过的 fg 保留；未改的字段仍参与亮色变体。
        r = run_case(binary, "user-modified-fg", mode=0,
                     replies=[b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\"],
                     user_layout=classic_modified, duration=3.8)
        checks = [
            (51 in r.colours, "用户改过的 fg 原样保留"),
            (24 in r.colours, "未改过的 info fg 仍命中亮色变体"),
            (236 not in r.colours, "改过的 playlist fg 不被亮色替换"),
        ]
        return r, checks

    @case
    def env_layout_verbatim():
        # B：TER_MUSIC_LAYOUTS 覆盖保持逐字生效（不参与值匹配）。
        path = None

        def prep(binary, env, cfg_dir, root):
            nonlocal path
            path = os.path.join(root, "env-layouts.json")
            with open(path, "w", encoding="utf-8") as f:
                f.write(env_layout_user)
            env["TER_MUSIC_LAYOUTS"] = path

        r = run_case(binary, "env-layout", mode=0,
                     replies=[b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\"],
                     layout_sel="envcopy", prep=prep, duration=3.8)
        checks = [
            ({37, 208} <= r.colours, "ENV 布局暗色值逐字生效"),
            (236 not in r.colours and 94 not in r.colours,
             "ENV 布局不参与亮色值匹配"),
        ]
        return r, checks

    @case
    def terminal_override_light():
        # D：本终端覆盖优先于全局默认（全局暗色，终端覆盖亮色）。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "test-term"
            path = os.path.join(cfg_dir, "appearance-terminals.json")
            with open(path, "w", encoding="utf-8") as f:
                f.write('{"version":1,"terminals":{"tp:test-term":{"mode":1}}}')

        r = run_case(binary, "terminal-override-light", mode=2, prep=prep,
                     duration=4.8, snapshots=[(4.2, "settings")],
                     key_steps=[(2.6, b"\x1bOQ"), (3.0, b"\x1bOC"),
                                (3.4, b"\x1bOB" * 18)])
        settings = screen_text(r, "settings")
        checks = [
            ({236, 24, 94} <= r.colours,
             "本终端覆盖亮色优先于全局暗色"),
            ("本终端外观" in settings and "亮色" in settings,
             "Settings 显示本终端覆盖"),
            (saved_value(r.saved, "appearance_mode") == 2,
             "按终端覆盖不写全局 appearance_mode（仍为 2）"),
        ]
        return r, checks

    @case
    def terminal_override_inherit():
        # D：Settings 选 Inherit → 删除条目、回落全局默认（暗色）。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "test-term"
            path = os.path.join(cfg_dir, "appearance-terminals.json")
            with open(path, "w", encoding="utf-8") as f:
                f.write('{"version":1,"terminals":{"tp:test-term":{"mode":1}}}')

        r = run_case(binary, "terminal-inherit", mode=2, prep=prep,
                     duration=7.6, snapshots=[(6.0, "settings")],
                     key_steps=[(2.6, b"\x1bOQ"), (3.0, b"\x1bOC"),
                                (3.4, b"\x1bOB" * 18),  # down to appended terminal row
                                (3.9, b"\n"),          # popup (cur=Light)
                                (4.4, b"\x1bOA"), (4.7, b"\x1bOA"),  # -> Inherit
                                (5.0, b"\n"),
                                (6.4, b"\x1b"), (7.0, b"q")])
        settings = screen_text(r, "settings")
        try:
            with open(os.path.join(r.cfg_dir, "appearance-terminals.json"),
                      encoding="utf-8") as f:
                saved = f.read()
        except OSError:
            saved = ""
        checks = [
            ("继承" in settings, "Settings 显示继承全局默认"),
            ("tp:test-term" not in saved or '"mode":1' not in saved,
             "覆盖文件条目已删除"),
            (saved_value(r.saved, "appearance_mode") == 2,
             "清除覆盖不写全局 appearance_mode"),
        ]
        return r, checks

    @case
    def terminal_override_select_light():
        # D：无覆盖时在 Settings 选择 Light → 写入 frontend-local 覆盖文件。
        def prep(binary, env, cfg_dir, root):
            env["TERM_PROGRAM"] = "test-term"

        r = run_case(binary, "terminal-select-light", mode=0, prep=prep,
                     duration=7.6, snapshots=[(6.0, "settings")],
                     key_steps=[(2.6, b"\x1bOQ"), (3.0, b"\x1bOC"),
                                (3.4, b"\x1bOB" * 18),  # down to appended terminal row
                                (3.9, b"\n"),          # popup (cur=Inherit)
                                (4.4, b"\x1bOB"), (4.7, b"\x1bOB"),  # -> Light
                                (5.0, b"\n"),
                                (6.4, b"\x1b"), (7.0, b"q")])
        settings = screen_text(r, "settings")
        try:
            with open(os.path.join(r.cfg_dir, "appearance-terminals.json"),
                      encoding="utf-8") as f:
                saved = f.read()
        except OSError:
            saved = ""
        checks = [
            ("tp:test-term" in saved and '"mode":1' in saved,
             "Settings 写入本终端覆盖文件"),
            ("亮色" in settings and "覆盖" in settings,
             "Settings 显示亮色（覆盖）"),
            ({236, 24, 94} <= r.colours, "选择后立即应用亮色"),
            (saved_value(r.saved, "appearance_mode") == 0,
             "按终端覆盖不写全局 appearance_mode（仍为 auto）"),
        ]
        return r, checks

    @case
    def modal_search_reply():
        extra = []

        def prep(binary, env, cfg_dir, root):
            music = prepare_library(root, binary, env, cfg_dir)
            extra.extend(["-o", music])

        r = run_case(binary, "search-reply", mode=2, duration=7.2,
                     extra_args=extra,
                     snapshots=[(3.9, "search_open"), (5.6, "search_reopen"),
                                (6.1, "main_after")],
                     key_steps=[(2.9, b"/"),                                   # open search
                                (3.3, b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\"),  # reply mid-modal
                                (4.0, b"ab"),                                  # typing still works
                                (4.4, b"\x1b"),                               # cancel search
                                (4.9, b"/"),                                   # keys not lost
                                (5.7, b"\x1b"),
                                (6.3, b"q")],
                     prep=prep)
        open_text = screen_text(r, "search_open")
        reopen_text = screen_text(r, "search_reopen")
        main_text = screen_text(r, "main_after")
        checks = [
            ("搜索歌曲" in open_text, "应答到达后搜索框仍在（未被 ESC 取消）"),
            ("搜索已取消" not in open_text, "没有出现“搜索已取消”状态"),
            ("rgb:" not in open_text and "]11;" not in open_text, "应答文本未注入搜索框/屏幕"),
            ("搜索歌曲" in reopen_text, "取消后可再次打开搜索框（按键未丢失）"),
            ("顺序播放" in main_text, "播放模式未被应答余字节改变"),
            (favorites_count(r.cfg_dir) == 0, "收藏未被应答余字节改变"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def modal_playlist_popup_reply():
        extra = []

        def prep(binary, env, cfg_dir, root):
            music = prepare_library(root, binary, env, cfg_dir)
            extra.extend(["-o", music])

        r = run_case(binary, "playlist-popup-reply", mode=2, duration=8.0,
                     extra_args=extra,
                     snapshots=[(4.4, "popup_open"), (5.9, "history_after"),
                                (6.7, "main_after")],
                     key_steps=[(3.0, b"j"),                                   # select the track row
                                (3.4, b"A"),                                   # add-to-playlist popup
                                (3.9, b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\"),  # reply mid-modal
                                (4.7, b"\x1b"),                               # close popup
                                (5.3, b"h"),                                   # history popup (keys alive)
                                (6.3, b"\x1b"),
                                (6.9, b"q")],
                     prep=prep)
        popup_text = screen_text(r, "popup_open")
        history_text = screen_text(r, "history_after")
        main_text = screen_text(r, "main_after")
        checks = [
            ("选择歌单" in popup_text, "应答到达后“加入歌单”弹层仍在（未取消）"),
            ("rgb:" not in popup_text and "]11;" not in popup_text, "应答文本未注入弹层/屏幕"),
            ("播放历史" in history_text, "弹层关闭后按键仍生效（可打开播放历史）"),
            ("顺序播放" in main_text, "播放模式未被应答余字节改变"),
            (favorites_count(r.cfg_dir) == 0, "收藏未被应答余字节改变"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def modal_history_popup_reply():
        extra = []

        def prep(binary, env, cfg_dir, root):
            music = prepare_library(root, binary, env, cfg_dir)
            extra.extend(["-o", music])

        r = run_case(binary, "history-popup-reply", mode=2, duration=6.2,
                     extra_args=extra,
                     snapshots=[(3.8, "popup_open"), (4.5, "main_after")],
                     key_steps=[(2.9, b"h"),                                   # history popup
                                (3.3, b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\"),
                                (4.1, b"\x1b"),
                                (4.9, b"q")],
                     prep=prep)
        popup_text = screen_text(r, "popup_open")
        main_text = screen_text(r, "main_after")
        checks = [
            ("播放历史" in popup_text, "应答到达后播放历史弹层仍在（未取消）"),
            ("rgb:" not in popup_text and "]11;" not in popup_text, "应答文本未注入弹层/屏幕"),
            ("顺序播放" in main_text, "播放模式未被应答余字节改变"),
            (favorites_count(r.cfg_dir) == 0, "收藏未被应答余字节改变"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def settings_light_names():
        # 亮色变体索引应有 color.* 名称；Auto 行的调色板光标应落在存储值上。
        r = run_case(binary, "settings-light-names", mode=1, duration=7.2,
                     snapshots=[(3.5, "settings_light"), (4.5, "sidebar_row"),
                                (5.5, "palette")],
                     key_steps=[(2.6, b"\x1bOQ"),        # F2 -> Settings
                                (3.0, b"\x1bOC"),        # focus content (playlist fg)
                                (3.8, b"\x1bOB" * 12),   # down to Sidebar Foreground
                                (4.7, b"\x1bOC"),        # open its palette
                                (5.9, b"\x1b"),          # close palette
                                (6.4, b"q")])
        settings = screen_text(r, "settings_light")
        sidebar = screen_text(r, "sidebar_row")
        palette = screen_text(r, "palette")
        checks = [
            ("炭灰" in settings, "亮色索引 236 显示颜色名（不再是“未知”）"),
            ("未知" not in settings, "亮色索引不会显示为 Unknown"),
            ("自动（亮/暗）" in sidebar, "Auto 主题行显示自动标记"),
            ("(6)" in palette and "自动（亮/暗）" in palette,
             "调色板把出厂值 6 标为自动（亮/暗）"),
            ("自定义: 24" not in palette,
             "Auto 行光标按存储值定位（没有落到自定义槽显示生效值 24）"),
        ]
        return r, checks

    @case
    def help_search_reply():
        # F1 残留：帮助搜索输入框也必须接入 appearance 过滤器。
        r = run_case(binary, "help-search-reply", mode=2, duration=7.9,
                     snapshots=[(4.4, "modal_open"), (6.1, "modal_reopen"),
                                (6.6, "main_after")],
                     key_steps=[(2.6, b"\x1b[19~"),                     # F8 -> Help
                                (3.0, b"/"),                            # open search prompt
                                (3.5, b"\x1b]11;rgb:1234/5678/9abc\x1b\\"),  # late reply
                                (4.0, b"ab"),                           # typing still works
                                (4.8, b"\x1b"),                        # close prompt
                                (5.4, b"/"),                            # reopen (keys alive)
                                (6.2, b"\x1b"),
                                (6.3, b"\x1bOP"),                      # F1 -> main
                                (7.2, b"q")])
        opened = screen_text(r, "modal_open")
        reopened = screen_text(r, "modal_reopen")
        main_text = screen_text(r, "main_after")
        checks = [
            ("搜索歌曲" in opened, "应答到达后帮助搜索框仍在（未被取消）"),
            ("rgb:" not in opened and "]11;" not in opened, "应答文本未注入帮助搜索框/屏幕"),
            ("搜索歌曲" in reopened, "关闭后可再次打开帮助搜索框（按键未丢失）"),
            ("顺序播放" in main_text, "播放模式未被应答余字节改变"),
            (favorites_count(r.cfg_dir) == 0, "收藏未被应答余字节改变"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def language_path_reply():
        # F1 残留：语言包路径输入框也必须接入 appearance 过滤器。
        r = run_case(binary, "language-path-reply", mode=2, duration=9.6,
                     snapshots=[(4.4, "modal_open"), (6.2, "modal_reopen"),
                                (8.4, "main_after")],
                     key_steps=[(2.6, b"\x1b[18~"),                     # F7 -> Language
                                (3.0, b"a"),                            # add language pack prompt
                                (3.5, b"\x1b]11;rgb:1234/5678/9abc\x1b\\"),
                                (4.1, b"/tmp/x"),                       # typing still works
                                (4.9, b"\n"),                          # submit path (add fails)
                                (5.8, b"a"),                            # reopen (keys alive)
                                (6.9, b"\n"),                          # empty = cancel picker
                                (7.7, b"\x1b"),                        # ESC -> 主界面
                                (8.9, b"q")])
        opened = screen_text(r, "modal_open")
        reopened = screen_text(r, "modal_reopen")
        main_text = screen_text(r, "main_after")
        checks = [
            ("文件路径（" in opened, "应答到达后语言包路径框仍在（未被取消）"),
            ("rgb:" not in opened and "]11;" not in opened, "应答文本未注入路径框/屏幕"),
            ("文件路径（" in reopened, "关闭后可再次打开路径框（按键未丢失）"),
            ("顺序播放" in main_text, "播放模式未被应答余字节改变"),
            (favorites_count(r.cfg_dir) == 0, "收藏未被应答余字节改变"),
            (r.status == 0, "进程正常退出"),
        ]
        return r, checks

    @case
    def v9_migration():
        r = run_case(binary, "v9-migration", mode=None, config_version=9,
                     theme_extra="<playlist_fg>7</playlist_fg>"
                                 "<controls_fg>15</controls_fg>"
                                 "<highlight_bg>4</highlight_bg>")
        checks = [
            (f'<ter-music-config version="{CONFIG_CURRENT_VERSION}">' in r.saved,
             f"v9 首启迁移到契约 {CONFIG_CURRENT_VERSION}"),
            (saved_value(r.saved, "appearance_mode") == 0, "补写默认 appearance_mode=0"),
            (saved_value(r.saved, "playlist_fg") == 7, "出厂值原样（playlist_fg=7）"),
            (saved_value(r.saved, "controls_fg") == 15, "显式值不被改写（controls_fg=15）"),
            (saved_value(r.saved, "highlight_bg") == 4, "显式值不被改写（highlight_bg=4）"),
        ]
        return r, checks

    @case
    def monochrome():
        r = run_case(binary, "monochrome", mode=1, duration=2.8, term="vt100")
        checks = [
            (r.status == 0, "单色终端（TERM=vt100）正常启动/退出"),
            (b"fatal_signal" not in r.out, "单色下无崩溃"),
            (b"]11;rgb:" not in r.out, "无应答文本上屏"),
        ]
        return r, checks

    @case
    def settings_cycle():
        r = run_case(binary, "settings-cycle", mode=2, duration=8.0,
                     key_steps=[(2.6, b"\x1bOQ"),    # F2 -> Settings
                                (3.0, b"\x1bOC"),    # -> content
                                (3.4, b"\x1bOA"),    # Up -> Re-detect row
                                (3.7, b"\x1bOA"),    # Up -> Appearance Mode row
                                (4.1, b"\x1bOC"),    # Right: Dark -> Auto
                                (4.7, b"\x1bOB"),    # Down -> Re-detect
                                (5.1, b"\n"),        # ENTER: force probe
                                (6.3, b"\x1b"),      # back to main
                                (6.9, b"q")])
        checks = [
            (saved_value(r.saved, "appearance_mode") == 0, "Settings 中 ←/→ 切换并持久化"),
            (r.queries >= 2, "启动同步探测 + 手动重探都发出查询"),
            (r.status == 0, "设置页操作后正常退出"),
        ]
        return r, checks

    failures = 0
    passed = 0
    for fn in cases:
        name = fn.__name__
        r, checks = fn()
        if os.environ.get("DUMP_SCREENS"):
            for label, lines in r.screens.items():
                print(f"--- screen [{name}:{label}]")
                for line in lines:
                    if line.strip():
                        print("   |" + line)
                print("--- end screen")
        bad = [msg for ok, msg in checks if not ok]
        if bad:
            failures += 1
            print(f"FAIL {name}")
            for msg in bad:
                print(f"      - {msg}")
            print(f"      queries={r.queries} replies={r.replies} "
                  f"status={r.status} outlen={len(r.out)}")
            print(f"      colours={sorted(r.colours)}")
            if b"fatal_signal" in r.out:
                print("      fatal signal in child output")
        else:
            passed += 1
            print(f"PASS {name}")
        shutil.rmtree(r.root, ignore_errors=True)

    print(f"{passed} passed, {failures} failed")
    return failures == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="build/ter-music",
                        help="ter-music 可执行文件（默认 build/ter-music）")
    parser.add_argument("--only", default=None,
                        help="只跑一个场景（函数名）")
    args = parser.parse_args()
    binary = os.path.abspath(args.bin)
    if not os.path.isfile(binary) or not os.access(binary, os.X_OK):
        print(f"FAIL 找不到可执行文件 {binary}", file=sys.stderr)
        return 2
    if not os.isatty(0):
        print("SKIP 需要 pty（请在终端或 pty 驱动下运行）", file=sys.stderr)
        return 0
    return 0 if run_suite(binary, args.only) else 1


if __name__ == "__main__":
    sys.exit(main())
