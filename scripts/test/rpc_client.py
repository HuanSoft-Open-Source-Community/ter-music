#!/usr/bin/env python3
"""ter-music D-Bus 测试客户端。

gdbus/busctl 都无法把负整数当作位置参数（会被当成选项），也无法在多次
调用间保持同一连接；本脚本补上这两点，供 scripts/test/*.sh 使用。

子命令：
  call IFACE.METHOD [ARG ...]   调用方法并把结果转成 JSON 打印
                                （int/bool/float/str 自动推断类型；字符串以
                                 json: 前缀则按 JSON 文本传递）
  attach ROLE [SECONDS]         连接后 Attach，持续 Ping 若干秒，再 Detach
                                （用于验证前端注册表与心跳）
  monitor SECONDS               收集若干秒内的信号（打印 "接口.成员"）

退出码：0 成功；1 调用失败；2 用法错误。
"""

import json
import sys
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

BUS_NAME = "org.mpris.MediaPlayer2.ter_music"
OBJECT_PATH = "/org/mpris/MediaPlayer2"


def _connection():
    return Gio.bus_get_sync(Gio.BusType.SESSION, None)


def _variant_to_python(value):
    """把 GVariant 递归转成 Python 对象。

    unpack() 会把容器逐层展开成 Python 对象（tuple/list/基本类型），
    因此递归时先判断是否还是 GVariant。
    """
    if isinstance(value, (bool, int, float, str)) or value is None:
        return value
    if isinstance(value, (list, tuple)):
        return [_variant_to_python(item) for item in value]
    kind = value.get_type_string()
    if kind.startswith(("a{", "a(", "a")) or kind.startswith("("):
        return _variant_to_python(value.unpack())
    if kind == "v":
        return _variant_to_python(value.get_variant())
    if kind in ("s", "o", "g"):
        return value.get_string()
    return value.unpack()


def _coerce(arg):
    """返回 (D-Bus 类型字符, 原始 Python 值)。"""
    if arg.startswith("json:"):
        return "s", arg[5:]
    try:
        return "i", int(arg)
    except ValueError:
        pass
    if arg in ("true", "false"):
        return "b", arg == "true"
    try:
        return "d", float(arg)
    except ValueError:
        pass
    return "s", arg


def cmd_call(argv):
    if not argv or "." not in argv[0]:
        print("用法：rpc_client.py call IFACE.METHOD [ARG ...]", file=sys.stderr)
        return 2

    iface, method = argv[0].rsplit(".", 1)
    coerced = [_coerce(a) for a in argv[1:]]
    parameters = None
    if coerced:
        signature = "(" + "".join(kind for kind, _ in coerced) + ")"
        parameters = GLib.Variant(signature, tuple(value for _, value in coerced))

    connection = _connection()
    try:
        result = connection.call_sync(
            BUS_NAME, OBJECT_PATH, iface, method, parameters,
            None, Gio.DBusCallFlags.NONE, 10000, None)
    except GLib.Error as exc:
        print("ERROR %s" % exc.message)
        return 1

    payload = _variant_to_python(result)
    if isinstance(payload, list) and len(payload) == 1:
        payload = payload[0]
    # 本 API 的复杂载荷都是 JSON 字符串：原样输出，便于 shell 直接管道进 jq
    if isinstance(payload, str):
        print(payload)
    else:
        print(json.dumps(payload, ensure_ascii=False, sort_keys=True))
    return 0


def cmd_attach(argv):
    role = argv[0] if argv else "cli"
    seconds = float(argv[1]) if len(argv) > 1 else 0
    connection = _connection()

    result = connection.call_sync(BUS_NAME, OBJECT_PATH, "org.yxzl.ter_music.Control",
                                  "Attach", GLib.Variant("(s)", (role,)), None,
                                  Gio.DBusCallFlags.NONE, 10000, None)
    token = json.loads(_variant_to_python(result)[0])["token"]
    print("token=%s" % token)

    deadline = time.time() + seconds
    while time.time() < deadline:
        connection.call_sync(BUS_NAME, OBJECT_PATH, "org.yxzl.ter_music.Control",
                             "Ping", GLib.Variant("(s)", (token,)), None,
                             Gio.DBusCallFlags.NONE, 10000, None)
        time.sleep(2)

    connection.call_sync(BUS_NAME, OBJECT_PATH, "org.yxzl.ter_music.Control",
                         "Detach", GLib.Variant("(s)", (token,)), None,
                         Gio.DBusCallFlags.NONE, 10000, None)
    print("detached")
    return 0


def cmd_monitor(argv):
    seconds = float(argv[0]) if argv else 3
    connection = _connection()
    seen = []

    def on_signal(_conn, _sender, path, iface, member, _params):
        if path == OBJECT_PATH:
            seen.append("%s.%s" % (iface, member))

    subscription = connection.signal_subscribe(
        None, None, None, OBJECT_PATH, None, Gio.DBusSignalFlags.NONE, on_signal)
    deadline = time.time() + seconds
    context = GLib.MainContext.default()
    while time.time() < deadline:
        while context.pending():
            context.iteration(False)
        time.sleep(0.02)
    connection.signal_unsubscribe(subscription)

    for name in sorted(set(seen)):
        print(name)
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    command = sys.argv[1]
    if command == "call":
        return cmd_call(sys.argv[2:])
    if command == "attach":
        return cmd_attach(sys.argv[2:])
    if command == "monitor":
        return cmd_monitor(sys.argv[2:])
    print("未知子命令 %s" % command, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
