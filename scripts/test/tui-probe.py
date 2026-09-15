#!/usr/bin/env python3
"""TUI 探针：用 pty 驱动 ter-music，按键后检查存活并抓取实例状态。

用途（M3 验收与回归）：
  - 启动冒烟：界面是否渲染、按键后是否存活、'q' 是否正常退出；
  - 状态对照：同一素材 + 同一按键序列后，把实例经 D-Bus 暴露的状态
    快照写文件，用于迁移前后的 A/B 比对（时间与实例字段会归一化）。

用法：
  tui-probe.py --bin ./build/ter-music --music DIR [--keys " jjn+-\\t"]
               [--state-out FILE] [--wait SECONDS]

注意：每次运行都要用**全新的** HOME/XDG_CONFIG_HOME 与音乐目录，
否则上一轮留下的队列/配置/曲库会让两次运行不可比（实测踩过）。
"""

import argparse
import json
import os
import pty
import select
import signal
import subprocess
import sys
import time


def parse_keys(text):
    """把 " jjn+-\t" 解析成字节序列（\\t \\n \\e 支持转义）。"""
    keys = []
    for char in text:
        if char == "\\":
            continue
        if char == "t":
            keys.append(b"\t")
        elif char == "n":
            keys.append(b"\n")
        elif char == "e":
            keys.append(b"\x1b")
        else:
            keys.append(char.encode("utf-8"))
    return keys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    parser.add_argument("--music", required=True, help="音乐目录（本次运行专用）")
    parser.add_argument("--home", help="HOME 目录，默认 <music>/../home")
    parser.add_argument("--keys", default=" jjn+-")
    parser.add_argument("--state-out", help="把 D-Bus 状态快照写到该文件")
    parser.add_argument("--wait", type=float, default=2.0, help="启动等待秒数")
    parser.add_argument("--keep-window", action="store_true")
    parser.add_argument("--extra-args", default="",
                        help="追加给被测程序的参数（空格分隔，如 --debug）")
    args = parser.parse_args()

    home = args.home or os.path.join(os.path.dirname(os.path.abspath(args.music)), "home")
    os.makedirs(home, exist_ok=True)

    env = dict(os.environ)
    env.update({
        "HOME": home,
        "XDG_CONFIG_HOME": os.path.join(home, ".config"),
        "TERM": "xterm-256color",
        "LC_ALL": "C.UTF-8",
        "COLUMNS": "100",
        "LINES": "30",
    })

    pid, fd = pty.fork()
    if pid == 0:
        argv = [args.bin, "-o", args.music] + [a for a in args.extra_args.split() if a]
        os.execve(args.bin, argv, env)

    def drain(seconds):
        output = b""
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if not ready:
                continue
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                return output
            if not chunk:
                return output
            output += chunk
        return output

    def alive():
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False

    banner = drain(args.wait)
    if len(banner) < 100:
        print("FAIL 未渲染出界面（收到 %d 字节）" % len(banner))
        os.kill(pid, signal.SIGKILL)
        return 1
    print("PASS TUI 启动并渲染（%d 字节）" % len(banner))

    pressed = []
    for key in parse_keys(args.keys):
        os.write(fd, key)
        pressed.append(key)
        time.sleep(0.45)
        drain(0.1)
        if not alive():
            print("FAIL 按键后进程退出：%r" % key)
            return 1
    print("PASS 按键序列后仍存活：%s" % b"".join(pressed).decode(errors="replace"))

    drain(0.8)

    if args.state_out:
        snapshot = subprocess.run([args.bin, "show", "--json"], env=env,
                                  capture_output=True, text=True, timeout=20).stdout
        try:
            doc = json.loads(snapshot)
        except Exception as exc:                       # noqa: BLE001
            print("FAIL 无法解析状态快照：%s" % exc)
            os.write(fd, b"q")
            return 1

        # 归一化：去掉时间相关与实例相关字段，便于 A/B 比对
        doc.pop("instance", None)
        doc.pop("revision", None)
        doc.pop("text", None)
        playback = doc.get("playback", {})
        for field in ("position_ms", "position", "remaining", "percent"):
            playback.pop(field, None)
        cover = doc.get("cover", {})
        for field in ("text", "art_url"):
            cover.pop(field, None)
        doc.get("core", {}).pop("methods", None)

        with open(args.state_out, "w", encoding="utf-8") as handle:
            json.dump(doc, handle, ensure_ascii=False, indent=1, sort_keys=True)
        print("状态快照已写出：%s" % args.state_out)

    os.write(fd, b"q")
    time.sleep(0.7)
    drain(0.3)

    try:
        _, status = os.waitpid(pid, os.WNOHANG)
    except ChildProcessError:
        status = 0
    if status:
        code = os.waitstatus_to_exitcode(status)
        if code != 0:
            print("FAIL 'q' 退出码 %d" % code)
            return 1
        print("PASS 'q' 正常退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
