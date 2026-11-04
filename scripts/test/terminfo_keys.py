#!/usr/bin/env python3
"""按 TERM 从 terminfo 取命名键的字节序列（供 pty 测试驱动复用）。

`infocmp -1 <TERM>` 的输出是 terminfo 源文本：`\\E` 代表 ESC，`^[` 代表
控制字符，八进制 `\\123` 代表单字节。这里把对应能力还原成真正要写进 pty
的字节，避免每套测试各手写一份硬编码表。

固定语义（不随 TERM 变）：`enter` 仍发 LF（0x0a），`tab` 发 HT，`space`
发空格，`esc` 发 ESC——TUI 里它们与 ncurses 的 KEY_ENTER 路径等价，且旧
脚本一直按这个约定发键。方向键、Home/End/PgUp/PgDn/Ins/Del、F1-F12、
Backspace、Shift+Tab 则取 terminfo。
"""

import subprocess

# 命名键 → terminfo 能力名
CAPABILITIES = {
    "up": "kcuu1",
    "down": "kcud1",
    "left": "kcub1",
    "right": "kcuf1",
    "home": "khome",
    "end": "kend",
    "pgup": "kpp",
    "pgdn": "knp",
    "ins": "kich1",
    "del": "kdch1",
    "backspace": "kbs",
    "btab": "kcbt",
}
for _n in range(1, 13):
    CAPABILITIES["f%d" % _n] = "kf%d" % _n

# infocmp 不可用时的回退表（xterm-256color 在 keypad(TRUE) 下的实际序列）。
# 方向键是 SS3 形态（\\EOA..\\EOD），不是普通模式的 CSI 形态——后者在
# ncurses 的 terminfo 按键树里匹配不上，会被拆成 ESC + 字面字符。
XTERM_FALLBACK = {
    "enter": b"\n",
    "return": b"\n",
    "tab": b"\t",
    "space": b" ",
    "esc": b"\x1b",
    "up": b"\x1bOA",
    "down": b"\x1bOB",
    "right": b"\x1bOC",
    "left": b"\x1bOD",
    "home": b"\x1bOH",
    "end": b"\x1bOF",
    "pgup": b"\x1b[5~",
    "pgdn": b"\x1b[6~",
    "ins": b"\x1b[2~",
    "del": b"\x1b[3~",
    "backspace": b"\x7f",
    "btab": b"\x1b[Z",
    "f1": b"\x1bOP",
    "f2": b"\x1bOQ",
    "f3": b"\x1bOR",
    "f4": b"\x1bOS",
    "f5": b"\x1b[15~",
    "f6": b"\x1b[17~",
    "f7": b"\x1b[18~",
    "f8": b"\x1b[19~",
    "f9": b"\x1b[20~",
    "f10": b"\x1b[21~",
    "f11": b"\x1b[23~",
    "f12": b"\x1b[24~",
}

_BYTE_ESCAPES = {
    "E": 0x1b,
    "e": 0x1b,
    "n": 0x0a,
    "r": 0x0d,
    "t": 0x09,
    "b": 0x08,
    "f": 0x0c,
    "s": 0x20,
    "0": 0x00,
    "\\": 0x5c,
    ",": 0x2c,
    ":": 0x3a,
    "^": 0x5e,
}

# xterm SGR 鼠标编码（tui-probe.py 与 lifecycle-driver.py 共用）。
# 按钮号：0=左键，1=中键，2=右键，64=滚轮上，65=滚轮下；列/行从 1 开始，
# ncurses 各减 1 之后才是程序里的 0 基坐标。实测（ncursesw，TERM=xterm-256color）：
#   · 左键按下+松开同一格 → 只报一个 BUTTON1_CLICKED（不会再补 RELEASED）；
#   · 两次快速左击      → 只报一个 BUTTON1_DOUBLE_CLICKED（第一次的 CLICKED
#                       被合掉），所以整段必须一次写入 pty 才能触发双击；
#   · 右键按下          → 程序只申请 BUTTON3_PRESSED，按下即一条事件；
#   · 滚轮              → 按下时报 BUTTON4/BUTTON5_PRESSED，松开不再报；
#   · 老式 X10 编码（ESC [ M 三字节）在该 terminfo 下不被解析，故只用 SGR。
_MOUSE_BUTTONS = {"click": 0, "double": 0, "right": 2, "right-click": 2,
                  "wheel-up": 64, "wheelup": 64,
                  "wheel-down": 65, "wheeldown": 65}


def _sgr_mouse_event(button, col, row):
    """一次完整的按下+松开，两个字节串合成一段（调用方一次写入）。"""
    return ("\x1b[<%d;%d;%dM\x1b[<%d;%d;%dm" % (button, col, row,
                                                button, col, row)).encode()


def mouse_sequence(token):
    """`<click:列:行>` / `<double:列:行>` / `<right:列:行>` /
    `<wheel-up:列:行>` / `<wheel-down:列:行>` → 写入 pty 的字节串；
    不是鼠标键时返回 None。

    双击是两个"按下+松开"连在一起：ncurses 靠时间间隔判定双击，分开两次
    写入就只会得到两次单击。
    """
    parts = token.split(":")
    kind = parts[0]
    if kind not in _MOUSE_BUTTONS:
        return None
    if len(parts) != 3:
        raise ValueError("鼠标键需要 <名:列:行>，收到 <%s>" % token)
    try:
        col, row = int(parts[1]), int(parts[2])
    except ValueError:
        raise ValueError("鼠标键的列/行必须是整数，收到 <%s>" % token)
    if col < 1 or row < 1:
        raise ValueError("鼠标键的列/行从 1 开始，收到 <%s>" % token)
    once = _sgr_mouse_event(_MOUSE_BUTTONS[kind], col, row)
    if kind == "double":
        return once + once
    return once


def xterm_defaults():
    return dict(XTERM_FALLBACK)


def decode_terminfo_string(value):
    """把 infocmp 的转义文本还原为字节。"""
    out = bytearray()
    i = 0
    length = len(value)
    while i < length:
        char = value[i]
        if char == "^" and i + 1 < length:
            out.append(ord(value[i + 1]) & 0x1f)
            i += 2
            continue
        if char == "\\" and i + 1 < length:
            nxt = value[i + 1]
            if nxt in _BYTE_ESCAPES:
                out.append(_BYTE_ESCAPES[nxt])
                i += 2
                continue
            if nxt.isdigit():
                digits = nxt
                j = i + 2
                while j < length and len(digits) < 3 and value[j].isdigit():
                    digits += value[j]
                    j += 1
                out.append(int(digits, 8) & 0xff)
                i = j
                continue
            # 未知转义：保留反斜杠后的字符（terminfo 源里少见）
            out.extend(nxt.encode("utf-8"))
            i += 2
            continue
        out.extend(char.encode("utf-8"))
        i += 1
    return bytes(out)


def _parse_infocmp(term):
    try:
        proc = subprocess.run(
            ["infocmp", "-1", term],
            capture_output=True, text=True, timeout=10,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return None, str(exc)
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip().splitlines()
        return None, detail[0] if detail else "infocmp 退出码 %d" % proc.returncode

    caps = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        line = line.rstrip(",")
        if "=" not in line:
            continue
        name, _, value = line.partition("=")
        caps[name.strip()] = value
    return caps, None


def named_keys(term):
    """返回 (命名键映射, 警告文本或 None)。

    terminfo 读取失败时退回 xterm 默认表并给出警告；调用方应把警告打到
    stderr（或据此跳过依赖终端序列的断言），不要静默假装成功。
    """
    term = term or "xterm-256color"
    caps, error = _parse_infocmp(term)
    mapping = xterm_defaults()
    if caps is None:
        return mapping, ("无法读取 TERM=%s 的 terminfo（%s），"
                         "回退到 xterm 默认序列" % (term, error))

    for name, capability in CAPABILITIES.items():
        value = caps.get(capability)
        if value is None:
            continue
        mapping[name] = decode_terminfo_string(value)
    return mapping, None
