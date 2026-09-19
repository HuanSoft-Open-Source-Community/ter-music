#!/usr/bin/env python3
"""生命周期端到端回归的驱动脚本（由 lifecycle-e2e.sh 调用）。

分成独立脚本而不是塞进 shell，是因为要把「在 pty 里起 TUI / 发按键 / 收
stderr / 取退出码」这四件事做对：shell 里 fork pty 之后主端描述符留在子进程，
父进程既按不了键也拿不到退出码，前面的写法就是这么卡住的。

子命令：
  tui <bin> <music> [--attach-only]     在 pty 里起 TUI，直到退出；输出 rc
  run <bin> <music> <keys> <seconds>    起 TUI，按 `keys`，掐掉，输出 pty 文本
"""

import os
import pty
import select
import signal
import sys
import time


def spawn(binary, argv):
    pid, fd = pty.fork()
    if pid == 0:
        err = os.open("/tmp/tm-life-tui.stderr", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.dup2(err, 2)
        os.execve(binary, [binary] + argv, dict(os.environ))
    return pid, fd


def drain(fd, seconds):
    """读 pty 直到超时或 EOF（TUI 退出时 pty 会 EOF）。"""
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
    return out


def cmd_tui(binary, music, attach_only=False):
    argv = ["-o", music]
    if attach_only:
        argv = ["--attach-only"] + argv
    pid, fd = spawn(binary, argv)
    text = drain(fd, 30)
    try:
        _, status = os.waitpid(pid, 0)
        rc = os.WEXITSTATUS(status)
    except ChildProcessError:
        rc = 0
    screen = open("/tmp/tm-life-tui.stderr", "r", errors="replace").read() if os.path.exists(
        "/tmp/tm-life-tui.stderr") else ""
    sys.stdout.write(text.decode(errors="replace"))
    sys.stderr.write(screen)
    # 退出码写文件：stdout 里混着 TUI 的 ANSI 画面，解析不可靠
    with open("/tmp/tm-life-tui.rc", "w") as handle:
        handle.write(str(rc))
    print("rc=%d" % rc)
    return rc


def cmd_run(binary, music, keys, seconds):
    pid, fd = spawn(binary, ["-o", music])
    out = drain(fd, float(seconds))
    for key in keys:
        os.write(fd, key.encode())
        time.sleep(0.8)
        out += drain(fd, 0.2)
    os.kill(pid, signal.SIGKILL)
    sys.stdout.buffer.write(out)
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    binary = sys.argv[2]
    if cmd == "tui":
        music = sys.argv[3]
        attach = "--attach-only" in sys.argv[4:]
        return cmd_tui(binary, music, attach)
    if cmd == "run":
        music, keys, seconds = sys.argv[3], sys.argv[4], sys.argv[5]
        return cmd_run(binary, music, keys, seconds)
    print("未知子命令: %s" % cmd, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
