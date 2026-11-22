#!/usr/bin/env python3
"""e2e helper：以 `deepin-terminal` 作为 comm 的祖先进程启动子进程。

用法：python3 deepin-ancestor.py <binary> [args...]

仅用于 appearance pty e2e 冲突场景：
  - TERM_PROGRAM=vscode + 本 helper（祖先 comm=deepin-terminal）
    → 适配器必须不启用（TERM_PROGRAM 权威）；
  - TERM_PROGRAM 未设置 + 本 helper
    → 适配器必须启用（祖先链正常路径）。
"""

import os
import subprocess
import sys


def set_comm(name):
    try:
        fd = os.open("/proc/self/comm", os.O_WRONLY)
        try:
            os.write(fd, name.encode())
        finally:
            os.close(fd)
        with open("/proc/self/comm", "r", encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def main():
    if not sys.argv[1:]:
        print("deepin-ancestor: missing command", file=sys.stderr)
        return 2
    if set_comm("deepin-terminal") != "deepin-terminal":
        print("deepin-ancestor: cannot set comm", file=sys.stderr)
        return 3
    return subprocess.call(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
