#!/usr/bin/env python3
"""CLI 帮助输出、内置分页器与语言优先级的回归检查。

覆盖已批准规格中的三组行为：
  1. 非 TTY：概览尺寸/换行、无裸 i18n key、help <cmd> 与 <cmd> --help 等价、
     未知目标退出码、help all、语言优先级（--lang > 环境变量 > 配置 > locale）；
  2. TTY：内置分页器（pty，不依赖 script(1)）的翻页与退出；
  3. 每条断言输出 PASS/FAIL，任一失败退出 1。

仅使用 Python 3 标准库。
"""

import argparse
import fcntl
import os
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unicodedata

PASS = 0
FAIL = 0

BARE_KEY_RE = re.compile(r"\b(?:cli|main)\.")
CLEAR_SEQ = b"\x1b[2J"
HOME_SEQ = b"\x1b[H"


def ok(name):
    global PASS
    PASS += 1
    print("PASS %s" % name)


def bad(name, detail):
    global FAIL
    FAIL += 1
    print("FAIL %s: %s" % (name, detail))


def display_width(text):
    """按终端列宽计算字符串宽度（CJK 宽字符算 2，组合字符算 0）。"""
    width = 0
    for ch in text:
        if unicodedata.combining(ch):
            continue
        width += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return width


def make_env(home, **overrides):
    """构造确定性的子进程环境；值为 None 表示删除该变量。"""
    env = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "HOME": home,
        "XDG_CONFIG_HOME": os.path.join(home, ".config"),
        "XDG_DATA_HOME": os.path.join(home, ".local", "share"),
        "XDG_CACHE_HOME": os.path.join(home, ".cache"),
        "LC_ALL": "C.UTF-8",
        "TER_MUSIC_CLI_LANG": "zh_CN",
        "TERM": "dumb",
        "COLUMNS": "80",
        "LINES": "24",
    }
    for key, value in overrides.items():
        if value is None:
            env.pop(key, None)
        else:
            env[key] = value
    return env


def run_cli(bin_path, args, env, timeout=20):
    return subprocess.run(
        [bin_path] + args,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )


def first_line(data):
    text = data.decode("utf-8", "replace")
    for line in text.splitlines():
        if line.strip():
            return line
    return ""


# ── 非 TTY 检查 ──────────────────────────────────────────────────────

def check_overview(bin_path, home):
    full = None
    for cols in (40, 80):
        name = "--help 非 tty（COLUMNS=%d）" % cols
        env = make_env(home, COLUMNS=str(cols))
        try:
            result = run_cli(bin_path, ["--help"], env)
        except subprocess.TimeoutExpired:
            bad(name, "超时")
            continue
        if result.returncode != 0:
            bad(name, "退出码 %d" % result.returncode)
            continue
        if cols == 80:
            full = result.stdout
        text = result.stdout.decode("utf-8", "replace")
        lines = text.splitlines()

        over = [(i + 1, display_width(line))
                for i, line in enumerate(lines)
                if display_width(line) > cols]
        if over:
            bad(name, "第 %d 行显示宽度 %d 超过 %d"
                % (over[0][0], over[0][1], cols))
            continue
        if BARE_KEY_RE.search(text):
            bad(name, "输出包含裸 i18n key")
            continue
        if cols == 80 and len(lines) > 60:
            bad(name, "概览 %d 行，超过上限 60" % len(lines))
            continue
        ok(name)


def check_equivalence(bin_path, home, target, subcommand=None):
    env = make_env(home, COLUMNS="80")
    help_args = ["help", target] + ([subcommand] if subcommand else [])
    flag_args = [target] + ([subcommand] if subcommand else []) + ["--help"]
    label = "help " + " ".join(help_args[1:])
    try:
        a = run_cli(bin_path, help_args, env)
        b = run_cli(bin_path, flag_args, env)
    except subprocess.TimeoutExpired:
        bad(label + " 与对应 --help 等价", "超时")
        return
    if a.returncode != 0 or b.returncode != 0:
        bad(label + " 与对应 --help 等价",
            "退出码 help=%d --help=%d" % (a.returncode, b.returncode))
        return
    if a.stdout != b.stdout:
        bad(label + " 与对应 --help 等价", "stdout 字节不一致")
        return
    ok(label + " 与对应 --help 等价")


def check_unknown_target(bin_path, home):
    env = make_env(home)
    try:
        result = run_cli(bin_path, ["help", "nope"], env)
    except subprocess.TimeoutExpired:
        bad("help nope 退出码 1 且 stderr 非空", "超时")
        return
    if result.returncode != 1:
        bad("help nope 退出码 1 且 stderr 非空",
            "退出码 %d" % result.returncode)
        return
    if not result.stderr.strip():
        bad("help nope 退出码 1 且 stderr 非空", "stderr 为空")
        return
    ok("help nope 退出码 1 且 stderr 非空")


def check_help_all(bin_path, home):
    env = make_env(home, TER_MUSIC_NO_PAGER="1")
    try:
        result = run_cli(bin_path, ["help", "all"], env)
    except subprocess.TimeoutExpired:
        bad("help all 非 tty 无分页转义", "超时")
        return
    if result.returncode != 0:
        bad("help all 非 tty 无分页转义", "退出码 %d" % result.returncode)
        return
    if b"\x1b" in result.stdout:
        bad("help all 非 tty 无分页转义", "输出含 ESC 序列")
        return
    ok("help all 非 tty 无分页转义")


def check_languages(bin_path, home_en, home_lang, home_cfg):
    # 1) LANG=en_US.UTF-8，无配置、无显式语言 → 英文
    env = make_env(
        home_en,
        LANG="en_US.UTF-8",
        LC_ALL=None,
        LC_MESSAGES=None,
        TER_MUSIC_CLI_LANG=None,
    )
    name = "LANG=en_US.UTF-8（无配置）→ 英文"
    try:
        result = run_cli(bin_path, ["--help"], env)
    except subprocess.TimeoutExpired:
        bad(name, "超时")
    else:
        line = first_line(result.stdout)
        if result.returncode == 0 and "Usage:" in line:
            ok(name)
        else:
            bad(name, "首行=%r，退出码=%d" % (line, result.returncode))

    # 2) --lang en_US 覆盖 LANG=zh_CN.UTF-8 → 英文
    env = make_env(
        home_lang,
        LANG="zh_CN.UTF-8",
        LC_ALL=None,
        LC_MESSAGES=None,
        TER_MUSIC_CLI_LANG=None,
    )
    name = "--lang en_US 覆盖 LANG=zh_CN.UTF-8 → 英文"
    try:
        result = run_cli(bin_path, ["--lang", "en_US", "--help"], env)
    except subprocess.TimeoutExpired:
        bad(name, "超时")
    else:
        line = first_line(result.stdout)
        if result.returncode == 0 and "Usage:" in line:
            ok(name)
        else:
            bad(name, "首行=%r，退出码=%d" % (line, result.returncode))

    # 3) TER_MUSIC_CLI_LANG=en_US 覆盖 LANG=zh_CN.UTF-8 → 英文
    env = make_env(
        home_lang,
        LANG="zh_CN.UTF-8",
        LC_ALL=None,
        LC_MESSAGES=None,
        TER_MUSIC_CLI_LANG="en_US",
    )
    name = "TER_MUSIC_CLI_LANG=en_US 覆盖 LANG=zh_CN.UTF-8 → 英文"
    try:
        result = run_cli(bin_path, ["--help"], env)
    except subprocess.TimeoutExpired:
        bad(name, "超时")
    else:
        line = first_line(result.stdout)
        if result.returncode == 0 and "Usage:" in line:
            ok(name)
        else:
            bad(name, "首行=%r，退出码=%d" % (line, result.returncode))

    # 4) 配置 ui_language=zh_CN 优先于 LANG=en_US.UTF-8 → 中文
    cfg_dir = os.path.join(home_cfg, ".config", "ter-music")
    os.makedirs(cfg_dir, exist_ok=True)
    with open(os.path.join(cfg_dir, "config.xml"), "w", encoding="utf-8") as fh:
        fh.write(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<ter-music-config version="9">\n'
            "  <preferences>\n"
            "    <ui_language>zh_CN</ui_language>\n"
            "  </preferences>\n"
            "</ter-music-config>\n"
        )
    env = make_env(
        home_cfg,
        LANG="en_US.UTF-8",
        LC_ALL=None,
        LC_MESSAGES=None,
        TER_MUSIC_CLI_LANG=None,
    )
    name = "配置 ui_language=zh_CN 优先于 LANG=en_US.UTF-8 → 中文"
    try:
        result = run_cli(bin_path, ["--help"], env)
    except subprocess.TimeoutExpired:
        bad(name, "超时")
    else:
        line = first_line(result.stdout)
        if result.returncode == 0 and "用法" in line and "Usage:" not in line:
            ok(name)
        else:
            bad(name, "首行=%r，退出码=%d" % (line, result.returncode))


# ── PTY / 分页器检查 ─────────────────────────────────────────────────

def _read_available(fd, buf, wait):
    ready, _, _ = select.select([fd], [], [], wait)
    if not ready:
        return False
    try:
        chunk = os.read(fd, 65536)
    except OSError:
        return False
    if not chunk:
        return False
    buf.extend(chunk)
    return True


def _wait_draws(fd, buf, count, deadline):
    while time.monotonic() < deadline:
        if buf.count(CLEAR_SEQ) >= count:
            return True
        _read_available(fd, buf, 0.05)
    return buf.count(CLEAR_SEQ) >= count


def _wait_exit(pid, fd, buf, deadline):
    while time.monotonic() < deadline:
        wpid, status = os.waitpid(pid, os.WNOHANG)
        if wpid == pid:
            return status
        _read_available(fd, buf, 0.05)
    return None


def run_pty(bin_path, args, env, cols, rows, key_plan, timeout=10):
    """在 pty 中运行；key_plan 为 [(等待第 N 次重绘, 按键或 None), ...]。"""
    pid, fd = pty.fork()
    if pid == 0:
        try:
            winsize = struct.pack("HHHH", rows, cols, 0, 0)
            fcntl.ioctl(0, termios.TIOCSWINSZ, winsize)
            os.execvpe(bin_path, [bin_path] + args, env)
        except BaseException:
            os._exit(127)

    try:
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    except OSError:
        pass

    buf = bytearray()
    deadline = time.monotonic() + timeout
    status = None
    try:
        for draw_index, key in key_plan:
            if not _wait_draws(fd, buf, draw_index,
                               min(deadline, time.monotonic() + 5)):
                break
            if key:
                os.write(fd, key)
        status = _wait_exit(pid, fd, buf, deadline)
        if status is None:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
    return status, bytes(buf)


def check_pager(bin_path, home, full_help_bytes):
    env = make_env(home, TERM="xterm-256color", COLUMNS="80", LINES="24")
    env.pop("TER_MUSIC_NO_PAGER", None)

    # 20x5：首屏 → 空格 → 空格 → q
    name = "pty 20x5：help all 空格×2 后 q 正常退出"
    try:
        status, output = run_pty(
            bin_path, ["help", "all"], env, 20, 5,
            [(1, b" "), (2, b" "), (3, b"q")],
        )
    except OSError as exc:
        bad(name, "pty 失败：%s" % exc)
    else:
        if status is None:
            bad(name, "10 秒内未退出（挂起）")
        elif not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
            bad(name, "异常退出 status=%r" % (status,))
        elif CLEAR_SEQ not in output and HOME_SEQ not in output:
            bad(name, "输出中没有清屏/分页转义")
        elif output.count(CLEAR_SEQ) < 3:
            bad(name, "仅重绘 %d 次，翻页未生效" % output.count(CLEAR_SEQ))
        elif len(output) >= len(full_help_bytes):
            bad(name, "输出长度 %d 不小于一次性完整输出 %d"
                % (len(output), len(full_help_bytes)))
        else:
            ok(name)

    # 40x10：首屏 → q
    name = "pty 40x10：help all q 正常退出"
    try:
        status, output = run_pty(
            bin_path, ["help", "all"], env, 40, 10, [(1, b"q")],
        )
    except OSError as exc:
        bad(name, "pty 失败：%s" % exc)
    else:
        if status is None:
            bad(name, "10 秒内未退出（挂起）")
        elif not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
            bad(name, "异常退出 status=%r" % (status,))
        elif CLEAR_SEQ not in output and HOME_SEQ not in output:
            bad(name, "输出中没有清屏/分页转义")
        else:
            ok(name)


# ── 主流程 ───────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="ter-music CLI 帮助/分页器/语言回归检查")
    parser.add_argument("--bin", default="./build/ter-music",
                        help="ter-music 可执行文件（默认 ./build/ter-music）")
    args = parser.parse_args()

    bin_path = os.path.abspath(args.bin)
    if not os.access(bin_path, os.X_OK):
        print("错误：找不到可执行文件 %s" % bin_path, file=sys.stderr)
        return 2

    root = tempfile.mkdtemp(prefix="tm-cli-help-")
    try:
        home_base = os.path.join(root, "home-base")
        home_en = os.path.join(root, "home-en")
        home_lang = os.path.join(root, "home-lang")
        home_cfg = os.path.join(root, "home-cfg")
        home_pty = os.path.join(root, "home-pty")
        for home in (home_base, home_en, home_lang, home_cfg, home_pty):
            os.makedirs(home, exist_ok=True)

        check_overview(bin_path, home_base)
        check_equivalence(bin_path, home_base, "play")
        check_equivalence(bin_path, home_base, "daemon", "start")
        check_unknown_target(bin_path, home_base)
        check_help_all(bin_path, home_base)
        check_languages(bin_path, home_en, home_lang, home_cfg)

        try:
            full = run_cli(bin_path, ["help", "all"],
                           make_env(home_base, TER_MUSIC_NO_PAGER="1"))
            full_help_bytes = full.stdout
        except subprocess.TimeoutExpired:
            full_help_bytes = b""
        check_pager(bin_path, home_pty, full_help_bytes)
    finally:
        shutil.rmtree(root, ignore_errors=True)

    print("%d passed, %d failed" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
