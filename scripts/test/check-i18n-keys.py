#!/usr/bin/env python3
"""i18n 键一致性校验（缺失键回归防线）

检查项：
  1. 内置 zh_CN / en_US / zh-PQ 三语言文件键集完全一致；
  2. 无重复键、无空值；
  3. 源码与头文件中每一条 i18n_get("字面量") 的键在两种语言中都存在；
  4. 两个间接引用清单同样必须完整：
     - settings.c 的 settings_options[]（设置菜单项文案键）；
     - keymap/actions.c 的 ACT("id" ...) → keyaction.<id>（动作描述键）。

缺键时 i18n_get() 返回键名本身（i18n.c），界面会直接露出裸键字符串，
所以这类问题必须在 CI 里拦下。

用法：python3 scripts/test/check-i18n-keys.py
退出码：0 = 通过；1 = 有缺失/不一致。
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LANG_DIR = ROOT / "data" / "lang"
SRC_DIRS = [ROOT / "src", ROOT / "include"]

# C 源里 i18n_get 的字面量键
DIRECT_CALL = re.compile(r'i18n_get\("([^"]+)"\)')
ACT_ID = re.compile(r'ACT\("([^"]+)"')
COMMENT_BLOCK = re.compile(r"/\*.*?\*/", re.S)
COMMENT_LINE = re.compile(r"//[^\n]*")


def load_lang(path):
    tree = ET.parse(path)
    return [(e.get("key"), (e.text or "")) for e in tree.iter("string")]


def strip_comments(text):
    return COMMENT_LINE.sub("", COMMENT_BLOCK.sub("", text))


def iter_sources():
    for base in SRC_DIRS:
        for path in base.rglob("*"):
            if path.suffix in (".c", ".h"):
                yield path


def main():
    builtins = ("zh_CN", "en_US", "zh-PQ")
    for lang in builtins:
        if not (LANG_DIR / f"{lang}.xml").is_file():
            print(f"FAIL i18n keys: built-in language {lang} not found under {LANG_DIR}")
            return 1

    zh = load_lang(LANG_DIR / "zh_CN.xml")
    en = load_lang(LANG_DIR / "en_US.xml")
    pq = load_lang(LANG_DIR / "zh-PQ.xml")
    errors = []

    for name, pairs in (("zh_CN", zh), ("en_US", en), ("zh-PQ", pq)):
        seen = set()
        for key, value in pairs:
            if key in seen:
                errors.append(f"{name}: duplicate key '{key}'")
            seen.add(key)
            if not value.strip():
                errors.append(f"{name}: empty value for '{key}'")

    zh_keys = {key for key, _ in zh}
    en_keys = {key for key, _ in en}
    pq_keys = {key for key, _ in pq}
    for key in sorted(zh_keys - en_keys):
        errors.append(f"en_US missing key '{key}'")
    for key in sorted(en_keys - zh_keys):
        errors.append(f"zh_CN missing key '{key}'")
    for key in sorted(zh_keys - pq_keys):
        errors.append(f"zh-PQ missing key '{key}'")
    for key in sorted(pq_keys - zh_keys):
        errors.append(f"zh_CN missing key from zh-PQ '{key}'")

    direct = set()
    for path in iter_sources():
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for match in DIRECT_CALL.finditer(text):
            direct.add(match.group(1))
    for key in sorted(direct):
        if key not in zh_keys:
            errors.append(f"zh_CN missing (i18n_get) '{key}'")
        if key not in en_keys:
            errors.append(f"en_US missing (i18n_get) '{key}'")
        if key not in pq_keys:
            errors.append(f"zh-PQ missing (i18n_get) '{key}'")

    settings_c = ROOT / "src" / "cn.org.huansoft.ter-music" / "ui" / "settings.c"
    text = settings_c.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"static const char \*settings_options\[\]\s*=\s*\{(.*?)\};",
                      text, re.S)
    if not match:
        errors.append("settings.c: cannot locate settings_options[]")
    else:
        for key in re.findall(r'"([^"]+)"', match.group(1)):
            if key not in zh_keys:
                errors.append(f"zh_CN missing (settings_options) '{key}'")
            if key not in en_keys:
                errors.append(f"en_US missing (settings_options) '{key}'")
            if key not in pq_keys:
                errors.append(f"zh-PQ missing (settings_options) '{key}'")

    editor_c = ROOT / "src" / "cn.org.huansoft.ter-music" / "ui" / "layout_editor.c"
    text = editor_c.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"static const char \*const k_action_keys\[\]\s*=\s*\{(.*?)\};",
                      text, re.S)
    if not match:
        errors.append("layout_editor.c: cannot locate k_action_keys[]")
    else:
        for key in re.findall(r'"([^"]+)"', match.group(1)):
            if key not in zh_keys:
                errors.append(f"zh_CN missing (layout_editor actions) '{key}'")
            if key not in en_keys:
                errors.append(f"en_US missing (layout_editor actions) '{key}'")
            if key not in pq_keys:
                errors.append(f"zh-PQ missing (layout_editor actions) '{key}'")

    actions_c = ROOT / "src" / "cn.org.huansoft.ter-music" / "keymap" / "actions.c"
    text = strip_comments(actions_c.read_text(encoding="utf-8", errors="replace"))
    ids = ACT_ID.findall(text)
    if not ids:
        errors.append("actions.c: no ACT() entries found")
    for action_id in ids:
        key = f"keyaction.{action_id}"
        if key not in zh_keys:
            errors.append(f"zh_CN missing (keyaction) '{key}'")
        if key not in en_keys:
            errors.append(f"en_US missing (keyaction) '{key}'")
        if key not in pq_keys:
            errors.append(f"zh-PQ missing (keyaction) '{key}'")

    if errors:
        print(f"FAIL i18n keys ({len(errors)} problem(s))")
        for line in errors[:60]:
            print(f"  {line}")
        if len(errors) > 60:
            print(f"  ... and {len(errors) - 60} more")
        return 1

    print(f"PASS i18n keys ({len(zh_keys)} keys; zh_CN/en_US/zh-PQ aligned, "
          f"{len(direct)} direct + {len(ids)} actions checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
