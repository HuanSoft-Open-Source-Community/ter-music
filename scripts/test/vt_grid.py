#!/usr/bin/env python3
"""把 pty 原始字节流还原成字符网格，供 e2e 断言"某个窗格标题画没画"。

由 2026-11-04 排查歌词窗格问题时的临时还原器（.tmp/todo/20261104-173114/
tools/vt.py）正式化而来。支持 ncurses 常用的光标定位、擦除、插删行/字符、
滚动与 UTF-8 文本；跳过 SGR、DECSET/DECRST、字符集选择与 OSC 标题。

**它不是完整的终端仿真器**，只服务于"某段文字当前在不在屏上"这类断言：
  - 文字宽度按 Unicode East Asian Width 估算（W/F 占 2 列，组合字符 0 列），
    不做 wrap、不做双向文本、不模拟滚动条；
  - 不要用它测量窗格宽度或断言列坐标；要量几何请用 `[layout]` 日志或
    `ui_pane_is_visible` 一类的、由程序自己给出的证据。

用法：
  python3 scripts/test/vt_grid.py [--cols 100] [--rows 30] FILE
      打印还原后的网格（诊断用）。
  python3 scripts/test/vt_grid.py --has '歌词|Lyrics' FILE
      在"去掉宽字符占位列"后的行上做 Python 正则搜索；命中退出 0，
      未命中退出 1，读文件失败退出 2。

e2e 的用法见 scripts/test/lyrics-pane-e2e.sh。
"""

import argparse
import re
import sys
import unicodedata

# CSI：参数、中间字节、终止字节。私有模式（?1h 等）与带中间字节的序列
# （如光标形状 `CSI 0 SP q`）都会匹配到，但一律忽略。
CSI = re.compile(rb"\x1b\[([0-9;?]*)([ -/]*)([@-~])")
OSC = re.compile(rb"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")
CHARSET = re.compile(rb"\x1b[()][0-9A-Za-z]")

# 显示宽度为 2 的东亚宽字符/全角字符。
_WIDE = ("W", "F")


def char_width(ch):
    """字符占用的显示列数：组合字符 0，东亚宽字符 2，其余 1。"""
    if unicodedata.combining(ch):
        return 0
    return 2 if unicodedata.east_asian_width(ch) in _WIDE else 1


class Screen:
    """最小 VT 屏。grid[r][c] 为单字符；宽字符的第二列存 None（占位）。"""

    def __init__(self, rows, cols):
        self.rows = rows
        self.cols = cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.r = 0
        self.c = 0
        self.saved = (0, 0)
        # 滚动区域（DECSTBM），0 基、闭区间；默认整屏。
        self.top = 0
        self.bottom = rows - 1

    # ── 基础操作 ──────────────────────────────────────────────

    def _blank_row(self):
        return [" "] * self.cols

    def _clamp(self):
        if self.r < 0:
            self.r = 0
        elif self.r >= self.rows:
            self.r = self.rows - 1
        if self.c < 0:
            self.c = 0
        elif self.c >= self.cols:
            self.c = self.cols - 1

    def put(self, ch):
        width = char_width(ch)
        if width == 0:
            return                      # 组合字符：不推进光标（本屏不叠加渲染）
        if self.c >= self.cols:
            return
        if width == 2 and self.c + 1 >= self.cols:
            # 宽字符落在最后一列：终端行为因实现而异；这里只写第一格。
            self.grid[self.r][self.c] = ch
            self.c = self.cols
            return
        self.grid[self.r][self.c] = ch
        if width == 2:
            self.grid[self.r][self.c + 1] = None
        self.c = min(self.cols, self.c + width)

    def linefeed(self):
        if self.r == self.bottom:
            self.scroll_up(1)
        elif self.r < self.rows - 1:
            self.r += 1

    def reverse_index(self):
        if self.r == self.top:
            self.scroll_down(1)
        elif self.r > 0:
            self.r -= 1

    def scroll_up(self, count):
        count = max(1, min(count, self.bottom - self.top + 1))
        for _ in range(count):
            del self.grid[self.top]
            self.grid.insert(self.bottom, self._blank_row())

    def scroll_down(self, count):
        count = max(1, min(count, self.bottom - self.top + 1))
        for _ in range(count):
            del self.grid[self.bottom]
            self.grid.insert(self.top, self._blank_row())

    @staticmethod
    def _params(raw):
        """CSI 参数字符串 → 整数列表；空参数记 None（表示用默认值）。"""
        if not raw:
            return [None]
        out = []
        for item in raw.decode("ascii", "replace").split(";"):
            out.append(int(item) if item.isdigit() else None)
        return out

    @staticmethod
    def _num(params, index, default):
        if index < len(params) and params[index] is not None:
            return params[index]
        return default

    # ── CSI ─────────────────────────────────────────────────

    def csi(self, raw_params, intermediates, cmd):
        if intermediates:
            return                      # 光标形状等：与本测试无关
        text = raw_params.decode("ascii", "replace")
        if text.startswith("?"):
            return                      # DECSET/DECRST（alt screen、鼠标、隐藏光标…）
        params = self._params(raw_params)
        p0 = self._num(params, 0, 0)
        n = p0 if p0 > 0 else 1

        if cmd in ("H", "f"):
            self.r = max(0, min(self.rows - 1, self._num(params, 0, 1) - 1))
            self.c = max(0, min(self.cols - 1, self._num(params, 1, 1) - 1))
        elif cmd in ("G", "`"):
            self.c = max(0, min(self.cols - 1, self._num(params, 0, 1) - 1))
        elif cmd == "d":
            self.r = max(0, min(self.rows - 1, self._num(params, 0, 1) - 1))
        elif cmd == "A":
            self.r = max(self.top, self.r - n)
        elif cmd == "B":
            self.r = min(self.bottom, self.r + n)
        elif cmd == "C":
            self.c = min(self.cols - 1, self.c + n)
        elif cmd == "D":
            self.c = max(0, self.c - n)
        elif cmd == "E":
            self.r = min(self.bottom, self.r + n)
            self.c = 0
        elif cmd == "F":
            self.r = max(self.top, self.r - n)
            self.c = 0
        elif cmd == "J":
            mode = p0
            if mode == 2 or mode == 3:
                self.grid = [self._blank_row() for _ in range(self.rows)]
                self.r = self.c = 0
            elif mode == 1:
                for rr in range(self.r):
                    self.grid[rr] = self._blank_row()
                for cc in range(self.c + 1):
                    self.grid[self.r][cc] = " "
            else:
                for rr in range(self.r + 1, self.rows):
                    self.grid[rr] = self._blank_row()
                for cc in range(self.c, self.cols):
                    self.grid[self.r][cc] = " "
        elif cmd == "K":
            mode = p0
            if mode == 2:
                self.grid[self.r] = self._blank_row()
            elif mode == 1:
                for cc in range(0, min(self.c + 1, self.cols)):
                    self.grid[self.r][cc] = " "
            else:
                for cc in range(self.c, self.cols):
                    self.grid[self.r][cc] = " "
        elif cmd == "X":
            for cc in range(self.c, min(self.cols, self.c + n)):
                self.grid[self.r][cc] = " "
        elif cmd == "L":                # IL：在光标行插入 n 行空行
            if self.top <= self.r <= self.bottom:
                for _ in range(n):
                    del self.grid[self.bottom]
                    self.grid.insert(self.r, self._blank_row())
                self.c = 0
        elif cmd == "M":                # DL：删除光标行起的 n 行
            if self.top <= self.r <= self.bottom:
                for _ in range(n):
                    del self.grid[self.r]
                    self.grid.insert(self.bottom, self._blank_row())
                self.c = 0
        elif cmd == "P":                # DCH：删除光标处 n 个字符
            row = self.grid[self.r]
            for _ in range(n):
                if self.c < self.cols:
                    del row[self.c]
                    row.append(" ")
        elif cmd == "@":                # ICH：在光标处插入 n 个空格
            row = self.grid[self.r]
            for _ in range(n):
                if self.c < self.cols:
                    row.insert(self.c, " ")
                    del row[self.cols]
        elif cmd == "S":
            self.scroll_up(n)
        elif cmd == "T":
            self.scroll_down(n)
        elif cmd == "r":                # DECSTBM：设置滚动区域
            top = self._num(params, 0, 1) - 1
            bottom = self._num(params, 1, self.rows) - 1
            if 0 <= top < bottom < self.rows:
                self.top, self.bottom = top, bottom
                self.r = self.c = 0
        elif cmd == "s":
            self.saved = (self.r, self.c)
        elif cmd == "u":
            self.r, self.c = self.saved
            self._clamp()
        # m（SGR）、h/l（模式）、t（窗口操作）等一律忽略

    # ── 输入流 ──────────────────────────────────────────────

    def feed(self, data):
        i, total = 0, len(data)
        while i < total:
            byte = data[i]
            if byte == 0x1B:
                match = CSI.match(data, i)
                if match:
                    self.csi(match.group(1), match.group(2), match.group(3).decode())
                    i = match.end()
                    continue
                match = OSC.match(data, i)
                if match:
                    i = match.end()
                    continue
                match = CHARSET.match(data, i)
                if match:
                    i = match.end()
                    continue
                # ESC 后无完整序列：视为字节流在快照处被截断，停止解析。
                if i + 1 >= total:
                    break
                nxt = data[i + 1]
                if nxt == 0x37:                 # ESC 7：保存光标
                    self.saved = (self.r, self.c)
                    i += 2
                    continue
                if nxt == 0x38:                 # ESC 8：恢复光标
                    self.r, self.c = self.saved
                    self._clamp()
                    i += 2
                    continue
                if nxt == 0x63:                 # ESC c：RIS，整屏复位
                    self.grid = [self._blank_row() for _ in range(self.rows)]
                    self.r = self.c = 0
                    self.top, self.bottom = 0, self.rows - 1
                    i += 2
                    continue
                if nxt == 0x4D:                 # ESC M：反向换行
                    self.reverse_index()
                    i += 2
                    continue
                if nxt == 0x44:                 # ESC D：IND
                    self.linefeed()
                    i += 2
                    continue
                if nxt == 0x45:                 # ESC E：NEL
                    self.linefeed()
                    self.c = 0
                    i += 2
                    continue
                if nxt in (0x3D, 0x3E):         # ESC = / ESC >：键盘模式
                    i += 2
                    continue
                # 其余单字节 ESC（如 ESC \ ST、ESC #8）跳过。
                i += 2
                continue
            if byte == 0x0A or byte == 0x0B or byte == 0x0C:
                self.linefeed()
                i += 1
                continue
            if byte == 0x0D:
                self.c = 0
                i += 1
                continue
            if byte == 0x08:
                self.c = max(0, self.c - 1)
                i += 1
                continue
            if byte == 0x09:
                self.c = min(self.cols - 1, (self.c // 8 + 1) * 8)
                i += 1
                continue
            if byte < 0x20 or byte == 0x7F:
                i += 1
                continue

            if byte < 0x80:
                length = 1
            elif byte >> 5 == 0x6:
                length = 2
            elif byte >> 4 == 0xE:
                length = 3
            elif byte >> 3 == 0x1E:
                length = 4
            else:
                i += 1
                continue
            if i + length > total:
                break                       # 快照截断了多字节字符
            try:
                ch = data[i:i + length].decode("utf-8")
            except UnicodeDecodeError:
                i += 1
                continue
            self.put(ch)
            i += length

    # ── 输出 ────────────────────────────────────────────────

    def lines(self, compressed=False):
        """compressed=True 时去掉宽字符占位列，便于做连续子串/正则匹配。"""
        out = []
        for row in self.grid:
            if compressed:
                text = "".join(cell for cell in row if cell is not None)
            else:
                text = "".join(" " if cell is None else cell for cell in row)
            out.append(text.rstrip())
        return out

    def dump(self):
        return "\n".join(self.lines(compressed=False))


def main(argv=None):
    parser = argparse.ArgumentParser(description="把 pty 字节流还原成字符网格")
    parser.add_argument("--cols", type=int, default=100, help="终端列数（默认 100）")
    parser.add_argument("--rows", type=int, default=30, help="终端行数（默认 30）")
    parser.add_argument("--has", dest="pattern",
                        help="在压缩宽字符占位后的行上做正则搜索；命中退出 0")
    parser.add_argument("file", help="pty 原始输出文件")
    args = parser.parse_args(argv)

    if args.cols <= 0 or args.rows <= 0:
        print("vt_grid: --cols/--rows 必须为正数", file=sys.stderr)
        return 2
    try:
        with open(args.file, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        print("vt_grid: 无法读取 %s: %s" % (args.file, exc), file=sys.stderr)
        return 2

    screen = Screen(args.rows, args.cols)
    screen.feed(data)

    if args.pattern:
        try:
            regex = re.compile(args.pattern)
        except re.error as exc:
            print("vt_grid: 正则无效 %r: %s" % (args.pattern, exc), file=sys.stderr)
            return 2
        for line in screen.lines(compressed=True):
            if regex.search(line):
                return 0
        return 1

    print(screen.dump())
    return 0


if __name__ == "__main__":
    sys.exit(main())
