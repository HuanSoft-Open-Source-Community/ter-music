#!/usr/bin/env python3
"""docs/API_DBUS_en_US.md 与运行实例的一致性检查。

抓两类漂移：
  - 代码里存在但文档未提的方法（客户端按文档写完才发现还有接口）；
  - 文档里写了但代码没有的方法（客户端照着文档调用会拿到 UnknownMethod）。

用法：
  doc_api_check.py DOC_PATH [METHODS_JSON]
    METHODS_JSON 为 core.methods 数组（默认从标准输入读）。

退出码：0 一致；1 有漂移；2 用法错误。
"""

import json
import re
import sys

# 文档里合法出现、但不属于 core.methods 的标识符（信号与接口名）
# 文档中合法出现、但不是方法名的标识符（接口名与信号名）
NON_METHOD_NAMES = {
    "Info", "Control", "Lyrics", "Playlist", "Queue", "Library",
    "Favorites", "History", "DirHistory", "Config", "Remote", "MediaPlayer2",
}

SIGNAL_NAMES = {
    "InfoChanged", "ProgressChanged", "CoverChanged", "LyricsChanged",
    "VisualizerFrame", "StatusMessage", "Error", "PlaylistChanged",
    "QueueChanged", "LibraryChanged", "ConfigChanged", "Seeked",
    "PropertiesChanged",
}


def documented_methods(text):
    """从方法表格第一列/第二列提取 `` `Method` `` 形式的成员名。"""
    found = set()
    for line in text.splitlines():
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) < 2:
            continue
        for cell in cells[:2]:
            for token in re.findall(r"`([^`]+)`", cell):
                token = token.strip()
                if (re.fullmatch(r"[A-Z][A-Za-z0-9]+", token)
                        and token not in SIGNAL_NAMES
                        and token not in NON_METHOD_NAMES):
                    found.add(token)
    return found


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    doc_path = sys.argv[1]
    with open(doc_path, "r", encoding="utf-8") as handle:
        text = handle.read()

    if len(sys.argv) > 2:
        with open(sys.argv[2], "r", encoding="utf-8") as handle:
            methods = json.load(handle)
    else:
        methods = json.load(sys.stdin)

    implemented = {name.split(".", 1)[1]: name for name in methods}
    documented = documented_methods(text)

    missing_docs = sorted(name for name in implemented if name not in documented)
    missing_code = sorted(name for name in documented
                          if name not in implemented and name not in NON_METHOD_NAMES)

    status = 0
    if missing_docs:
        status = 1
        print("文档缺少以下已实现方法：")
        for name in missing_docs:
            print("  %s (%s)" % (name, implemented[name]))
    if missing_code:
        status = 1
        print("文档描述了未实现的方法：")
        for name in missing_code:
            print("  %s" % name)

    if status == 0:
        print("doc/api 一致：%d 个方法全部有文档" % len(implemented))
    return status


if __name__ == "__main__":
    sys.exit(main())
