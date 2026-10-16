#!/usr/bin/env python3
"""生命周期端到端回归的驱动脚本（由 lifecycle-e2e.sh 调用）。

分成独立脚本而不是塞进 shell，是因为要把「在 pty 里起 TUI / 发按键 / 收
stderr / 取退出码」这四件事做对：shell 里 fork pty 之后主端描述符留在子进程，
父进程既按不了键也拿不到退出码，前面的写法就是这么卡住的。

子命令：
  tui <bin> <music> [--attach-only]     在 pty 里起 TUI，直到退出；输出 rc
  run <bin> <music> <keys> <seconds>    起 TUI，按 `keys`，掐掉，输出 pty 文本
  drive <bin> <music> <out-file> --ctl FILE
                                        交互式会话：TUI 保活，按键从指令文件逐行下发

`run` 一次把键发完就掐掉进程，无法在"按了键"与"看核心状态"之间交替；
`drive` 补的就是这一点——脚本可以先下发一段按键、经 D-Bus 断言核心状态，
再下发下一段（曲库/收藏/历史/搜索的播放回归需要这种节奏）。
指令文件（普通文件，调用方 `printf '%s\n' ... >> file` 追加）每行一条指令：
  key:<按键描述>   向 pty 写入按键（见 decode_keyspec 的转义与 <F9> 命名键）
  wait:<秒>        驱动侧等待（同时继续收屏）
  kill             SIGKILL 掉 TUI 并结束会话
  其余（含空行、# 开头）忽略
用普通文件而不是 FIFO：FIFO 的写端 open 会阻塞到读端就绪，shell 里一旦
驱动没起来就整脚本卡死；追加式文件没有这个问题，也不丢已写的指令。
"""

import os
import pty
import select
import signal
import sys
import time


# 命名键 → 终端实际收到的字节序列。探针统一把 TERM 设成 xterm-256color
# （见 cmd_drive），所以这里就是 xterm 家族的 terminfo 取值：
#   kf1..kf4 = ESC O P/Q/R/S，kf5..kf12 = ESC [ 15/17/18/19/20/21/23/24 ~
# F9 是 `\e[20~`（`\e[24~` 在 xterm 里是 F12，别混）。
NAMED_KEYS = {
    "<enter>": b"\n",
    "<return>": b"\n",
    "<tab>": b"\t",
    "<space>": b" ",
    "<esc>": b"\x1b",
    "<up>": b"\x1b[A",
    "<down>": b"\x1b[B",
    "<right>": b"\x1b[C",
    "<left>": b"\x1b[D",
    "<f1>": b"\x1bOP",
    "<f2>": b"\x1bOQ",
    "<f3>": b"\x1bOR",
    "<f4>": b"\x1bOS",
    "<f5>": b"\x1b[15~",
    "<f6>": b"\x1b[17~",
    "<f7>": b"\x1b[18~",
    "<f8>": b"\x1b[19~",
    "<f9>": b"\x1b[20~",
    "<f10>": b"\x1b[21~",
    "<f11>": b"\x1b[23~",
    "<f12>": b"\x1b[24~",
}

_BYTE_ESCAPES = {"e": b"\x1b", "n": b"\n", "t": b"\t", "r": b"\r", "\\": b"\\"}


def decode_keyspec(text):
    """把一行按键描述解码成要写进 pty 的字节。

    支持：`<F9>`/`<Enter>` 这类命名键（大小写不敏感）、`\\e \\n \\t \\r \\\\`
    与 `\\xHH`，其余字符按 UTF-8 原样发送。多字节序列必须**一次写入**：
    ncurses 只有在一个读包里拿到 `ESC [ 2 0 ~` 才会把它当成 KEY_F(9)，
    逐个字节分开写会被拆成 ESC 加一串字面字符（这是 `run` 按字符发键的
    局限，也是本命令存在的另一个原因）。
    """
    out = bytearray()
    i = 0
    length = len(text)
    while i < length:
        char = text[i]
        if char == "<":
            end = text.find(">", i)
            token = text[i:end + 1].lower() if end != -1 else ""
            if end != -1 and token in NAMED_KEYS:
                out += NAMED_KEYS[token]
                i = end + 1
                continue
        if char == "\\":
            nxt = text[i + 1] if i + 1 < length else ""
            if nxt == "x" and i + 3 < length:
                try:
                    out.append(int(text[i + 2:i + 4], 16))
                    i += 4
                    continue
                except ValueError:
                    pass
            if nxt in _BYTE_ESCAPES:
                out += _BYTE_ESCAPES[nxt]
                i += 2
                continue
        out += char.encode("utf-8")
        i += 1
    return bytes(out)


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


def cmd_drive(argv):
    """交互式会话：起 TUI 后保活，按键由指令文件逐行驱动，屏写到 out-file。

    用法：drive <bin> <music> <out-file> --ctl FILE [--rc-file F] [--pid-file F]
                 [--term T] [--cols N] [--rows N] [--max-runtime S] [-- <附加参数>...]
    """
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    binary, music, out_path = argv[0], argv[1], argv[2]
    rest = argv[3:]
    rc_path = pid_path = ctl_path = None
    term = os.environ.get("TM_TUI_TERM", "xterm-256color")
    cols, rows = 100, 30
    max_runtime = 180.0
    extra = []
    i = 0
    while i < len(rest):
        opt = rest[i]
        if opt == "--rc-file":
            rc_path = rest[i + 1]; i += 2
        elif opt == "--pid-file":
            pid_path = rest[i + 1]; i += 2
        elif opt == "--ctl":
            ctl_path = rest[i + 1]; i += 2
        elif opt == "--term":
            term = rest[i + 1]; i += 2
        elif opt == "--cols":
            cols = int(rest[i + 1]); i += 2
        elif opt == "--rows":
            rows = int(rest[i + 1]); i += 2
        elif opt == "--max-runtime":
            max_runtime = float(rest[i + 1]); i += 2
        elif opt == "--":
            extra = rest[i + 1:]; break
        else:
            print("drive: 未知选项 %s" % opt, file=sys.stderr)
            return 2
    if not ctl_path:
        print("drive: 必须给 --ctl <指令文件>", file=sys.stderr)
        return 2

    env = dict(os.environ)
    env.update({"TERM": term, "COLUMNS": str(cols), "LINES": str(rows),
                "LC_ALL": env.get("LC_ALL", "C.UTF-8")})

    pid, fd = pty.fork()
    if pid == 0:
        err_path = env.get("TM_TUI_STDERR") or "/tmp/tm-drive-tui.stderr"
        try:
            err = os.open(err_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            os.dup2(err, 2)
        except OSError:
            pass
        try:
            os.execve(binary, [binary, "-o", music] + extra, env)
        except OSError as exc:                       # noqa: BLE001
            os.write(2, ("drive: 无法执行 %s：%s\n" % (binary, exc)).encode())
            os._exit(127)

    # 主端在这个进程里，所以退出码/存活情况只能由这里判定后交给 shell 读文件
    if pid_path:
        with open(pid_path, "w") as handle:
            handle.write(str(pid))

    import fcntl
    import struct
    import termios
    try:
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    except OSError:
        pass

    exit_code = {"code": None}

    def poll_exit():
        if exit_code["code"] is not None:
            return exit_code["code"]
        try:
            done, status = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            exit_code["code"] = 0
            return 0
        if done == 0:
            return None
        exit_code["code"] = os.waitstatus_to_exitcode(status)
        return exit_code["code"]

    def spin(seconds, capture):
        """边等边收屏（pty 不读会塞满缓冲区，界面随之卡死）。"""
        end = time.time() + seconds
        while time.time() < end:
            if poll_exit() is not None:
                return
            ready, _, _ = select.select([fd], [], [], 0.05)
            if not ready:
                continue
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                return
            if not chunk:
                return
            capture.write(chunk)

    def run_command(text, capture):
        """执行一条会话指令；返回 False 表示会话应当结束。"""
        verb, _, arg = text.partition(":")
        verb = verb.strip().lower()
        if verb == "key":
            if poll_exit() is not None:
                return False
            try:
                os.write(fd, decode_keyspec(arg))
            except OSError:
                return False
            spin(0.6, capture)
            return True
        if verb == "wait":
            try:
                spin(float(arg), capture)
            except ValueError:
                pass
            return True
        if verb == "kill":
            os.kill(pid, signal.SIGKILL)
            spin(0.3, capture)
            return False
        return True

    def next_commands(state):
        """增量读指令文件里新追加的整行（半行留在 state 里等下一拍）。"""
        lines = []
        try:
            size = os.path.getsize(ctl_path)
        except OSError:
            return lines
        if state["offset"] > size:            # 文件被截断重来
            state["offset"] = 0
            state["pending"] = ""
        if size == state["offset"]:
            return lines
        with open(ctl_path, "r", encoding="utf-8", errors="replace") as handle:
            handle.seek(state["offset"])
            data = handle.read()
            state["offset"] = handle.tell()
        state["pending"] += data
        while "\n" in state["pending"]:
            text, state["pending"] = state["pending"].split("\n", 1)
            text = text.rstrip("\r")
            if not text or text.startswith("#"):
                continue
            lines.append(text)
        return lines

    deadline = time.time() + max_runtime
    state = {"offset": 0, "pending": ""}
    stop = False
    with open(out_path, "wb", buffering=0) as capture:
        spin(1.0, capture)
        while time.time() < deadline and not stop:
            if poll_exit() is not None:
                break
            for text in next_commands(state):
                if not run_command(text, capture):
                    stop = True
                    break
            # 没有指令的节拍里也要持续收屏（顺带充当指令轮询间隔）
            spin(0.15, capture)

        # 收尾：再收一小段屏，然后保证不留孤儿 TUI
        spin(0.3, capture)
        if poll_exit() is None:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            spin(0.3, capture)
            poll_exit()

    code = poll_exit()
    if rc_path:
        with open(rc_path, "w") as handle:
            handle.write("" if code is None else str(code))
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[0]
    binary = argv[1]
    if cmd == "tui":
        music = argv[2]
        attach = "--attach-only" in argv[3:]
        return cmd_tui(binary, music, attach)
    if cmd == "run":
        music, keys, seconds = argv[2], argv[3], argv[4]
        return cmd_run(binary, music, keys, seconds)
    if cmd == "drive":
        return cmd_drive(argv[1:])
    print("未知子命令: %s" % cmd, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
