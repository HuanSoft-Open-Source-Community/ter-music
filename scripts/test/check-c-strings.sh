#!/usr/bin/env bash
#
# check-c-strings.sh — C 源码字面量中文门禁（S3）
#
# 背景：状态行/错误文案曾硬编码中文，英文界面下会混进中文（audit S3）。
# 本脚本扫描 src/ 下 *.c/*.h 的**字符串字面量**（先剥离注释，注释里的中文
# 不算问题），发现中文即失败；确属非 UI 文案或本轮范围外的文件走白名单，
# 白名单必须写明理由。
#
# 白名单（三个层级，优先级从高到低）：
#   1. 行内标记：字符串所在行或上一行带 `i18n-exempt: <理由>` 注释；
#   2. 文件白名单：见下方 WHITELIST（按仓库相对路径，含理由）；
#   3. 无——其余一律失败。
#
# 用法：
#   scripts/test/check-c-strings.sh            # 门禁（0=通过，1=失败）
#   scripts/test/check-c-strings.sh --list     # 打印文件白名单
#   scripts/test/check-c-strings.sh --verbose  # 同时打印命中白名单的位置
#
# 依赖：python3（CI gates 已有）。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

exec python3 - "$REPO_ROOT" "$@" <<'PYEOF'
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1])
ARGS = sys.argv[2:]
VERBOSE = "--verbose" in ARGS
LIST = "--list" in ARGS

# 文件白名单：仓库相对路径 -> 理由（必须能自证「不是英文界面里的中文」）。
WHITELIST = {
    "src/cn.org.huansoft.ter-music/audio/karaoke_model.c":
        "S3 第二轮 t6 处理（艺术文件归属：本轮不碰 karaoke_model.c/h）",
    "src/cn.org.huansoft.ter-music/audio/playback_thread.c":
        "分段回退诊断只进 log_warn，不上状态行；不在本轮 S3 允许改文件列表",
    "src/cn.org.huansoft.ter-music/ui/mouse.c":
        "鼠标命中调试日志（log_debug），不上状态行",
    "src/cn.org.huansoft.ter-music/player/player_remote.c":
        "前端远程控制错误诊断（log/CLI 提示），待后续 i18n 任务收口",
    "src/cn.org.huansoft.ter-music/media/rpc_control.c":
        "内嵌 D-Bus XML 协议说明文本（等同注释），不是 UI 文案",
    "src/cn.org.huansoft.ter-music/media/rpc_config.c":
        "状态文本为中英双语拼接（既有风格，同时含英文）",
    "src/cn.org.huansoft.ter-music/ui/ui.c":
        "'中' 为 CJK 宽度/对齐表数据，不是展示文案",
    "src/cn.org.huansoft.ter-music/cli/lang.c":
        "CLI 语言包一次性终端警告，待后续 i18n 任务收口",
    "src/cn.org.huansoft.ter-music/core/core.c":
        "状态文本为中英双语拼接（既有风格，同时含英文）",
    "src/cn.org.huansoft.ter-music/config/config.c":
        "默认音乐目录名 '/音乐'（路径字面量），不是展示文案",
}

CJK = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]")


def strip_comments_keep_strings(text):
    """逐字符扫描：剥离 /* */ 与 // 注释，保留字符串字面量原文。

    返回 [(line_no, literal_start_line, literal_text)]。
    """
    out = []
    i = 0
    n = len(text)
    line = 1
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    line += 1
                i += 1
            i += 2
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "'":
            # 字符常量（如 '"'、'\''、'\n'）不是字符串，必须整体跳过，
            # 否则其中的 '"' 会被误判为字符串起点。
            i += 1
            while i < n:
                ch = text[i]
                if ch == "\\" and i + 1 < n:
                    i += 2
                    continue
                if ch == "'":
                    i += 1
                    break
                if ch == "\n":
                    line += 1
                i += 1
            continue
        if c == '"':
            start_line = line
            i += 1
            buf = []
            while i < n:
                ch = text[i]
                if ch == "\\" and i + 1 < n:
                    buf.append(text[i:i + 2])
                    if text[i + 1] == "\n":
                        line += 1
                    i += 2
                    continue
                if ch == '"':
                    i += 1
                    break
                if ch == "\n":
                    line += 1
                buf.append(ch)
                i += 1
            out.append((start_line, "".join(buf)))
            continue
        if c == "\n":
            line += 1
        i += 1
    return out


def is_exempt(source_lines, literal_line):
    for ln in (literal_line - 1, literal_line):
        if 1 <= ln <= len(source_lines) and "i18n-exempt:" in source_lines[ln - 1]:
            return True
    return False


def rel(path):
    return path.relative_to(ROOT).as_posix()


def main():
    targets = sorted(
        p for p in (ROOT / "src").rglob("*")
        if p.suffix in (".c", ".h") and p.is_file()
    )
    if not targets:
        print("FAIL check-c-strings：src/ 下没有找到 C 源文件")
        return 1

    offenders = []
    exempted = 0
    seen_whitelist = set()
    scanned_literals = 0
    for path in targets:
        text = path.read_text(encoding="utf-8", errors="replace")
        source_lines = text.splitlines()
        for line_no, literal in strip_comments_keep_strings(text):
            scanned_literals += 1
            if not CJK.search(literal):
                continue
            key = rel(path)
            if is_exempt(source_lines, line_no):
                exempted += 1
                continue
            if key in WHITELIST:
                seen_whitelist.add(key)
                if VERBOSE:
                    print(f"  whitelisted {key}:{line_no}: {literal[:80]}")
                continue
            offenders.append((key, line_no, literal))

    stale = sorted(set(WHITELIST) - seen_whitelist)
    print(f"scan: {len(targets)} files, {scanned_literals} string literals")
    if exempted:
        print(f"inline-exempt: {exempted}")
    if stale:
        # 白名单过期不判失败（避免下游 i18n 修复反被门禁卡住），但提示清理。
        print("warning: whitelist entries no longer needed（建议删除）：")
        for key in stale:
            print(f"  - {key}")
    if offenders:
        print(f"FAIL check-c-strings：{len(offenders)} 处 C 字面量中文不在白名单")
        for key, line_no, literal in offenders:
            print(f"  {key}:{line_no}: {literal[:120]}")
        print("修复：改用 i18n_get(\"key\") 并同步 data/lang 三语；"
              "确非 UI 文案/本轮范围外才加白名单（必须写理由）。")
        return 1
    print(f"PASS check-c-strings（{len(seen_whitelist)} 个白名单文件，"
          f"{exempted} 处行内豁免）")
    return 0


if "--list" in ARGS:
    for key in sorted(WHITELIST):
        print(f"{key}: {WHITELIST[key]}")
    sys.exit(0)

sys.exit(main())
PYEOF
