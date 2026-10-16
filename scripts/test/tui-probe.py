#!/usr/bin/env python3
"""TUI 探针：用 pty 驱动 ter-music，按键后检查存活并抓取实例状态。

用途（M3 验收与回归）：
  - 启动冒烟：界面是否渲染、按键后是否存活、'q' 是否正常退出；
  - 状态对照：同一素材 + 同一按键序列后，把实例经 D-Bus 暴露的状态
    快照写文件，用于迁移前后的 A/B 比对（时间与实例字段会归一化）。

用法：
  tui-probe.py --bin ./build/ter-music --music DIR [--keys " jjn+-\\t"]
               [--state-out FILE] [--wait SECONDS]

--bin 会先规范成绝对路径并校验可执行：路径若是失效的（例如调用方先 cd 到了
别处，相对路径在那一刻已经不存在），探针当场明确报错，而不是把子进程的
traceback 当成“界面已渲染”。要在别的目录里运行探针，请传绝对路径。

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

    # --bin 按**本进程启动时的 CWD** 解析。调用方若先 cd 到别处（config-migration-check.sh
    # 就是这样，好让被测程序的调试日志落在工作目录里），相对路径可能已经失效；
    # 这里当场校验并明确报错，避免子进程只剩一段 traceback 而父进程把它当成
    # “界面已渲染”（那条断言曾因此在 CI 里长期 skip）。
    args.bin = os.path.realpath(os.path.abspath(args.bin))
    if not os.path.isfile(args.bin) or not os.access(args.bin, os.X_OK):
        print("FAIL --bin 不存在或不可执行：%s（相对路径按调用时的 CWD 解析）" % args.bin)
        return 2

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
        try:
            os.execve(args.bin, argv, env)
        except OSError as exc:                       # noqa: BLE001
            # 明确说出原因，而不是让父进程把 traceback 当画面
            os.write(2, ("tui-probe: 无法执行 %s：%s\n" % (args.bin, exc)).encode())
            os._exit(127)

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

    reaped = {"code": None}

    def child_exit_code():
        """子进程已退出时回收并返回退出码，仍在运行返回 None。

        用 waitpid 而不是 kill(pid, 0)：僵尸进程对 kill 是“存在”的，会让
        “按键后仍存活”这种断言在进程早已崩掉时依然通过。回收结果缓存下来，
        后面的退出码检查不会因为这里先回收过就丢掉。"""
        if reaped["code"] is not None:
            return reaped["code"]
        try:
            done, status = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            reaped["code"] = 0
            return 0
        if done == 0:
            return None
        reaped["code"] = os.waitstatus_to_exitcode(status)
        return reaped["code"]

    def alive():
        return child_exit_code() is None

    banner = drain(args.wait)
    if not alive():
        print("FAIL 被测进程启动后立刻退出（rc=%s）：\n%s"
              % (child_exit_code(), banner.decode(errors="replace")[:400]))
        return 1
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
            print("FAIL 按键后进程退出（rc=%s）：%r" % (child_exit_code(), key))
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

    code = child_exit_code()
    if code is None:
        return 0            # 没判定（进程还在收尾）：与既往行为一致
    if code != 0:
        print("FAIL 'q' 退出码 %d" % code)
        return 1
    print("PASS 'q' 正常退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
