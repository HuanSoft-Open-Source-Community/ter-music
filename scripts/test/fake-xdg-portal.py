#!/usr/bin/env python3
"""最小 XDG desktop portal Settings 服务（仅供 appearance pty e2e）。

在 dbus-run-session 的 session bus 上拥有 org.freedesktop.portal.Desktop，
在 /org/freedesktop/portal/desktop 实现 org.freedesktop.portal.Settings 的
ReadOne/Read/ReadAll 与 SettingChanged。color-scheme 初始值由 --initial 指定
（0/1/2），运行期监视 --control-file：文件内容变化时发 SettingChanged。

用法（在 dbus-run-session 内）：
  python3 fake-xdg-portal.py --initial 2 --control-file /tmp/scheme
"""

import argparse
import os
import sys
import time

try:
    import dbus
    import dbus.mainloop.glib
    import dbus.service
    from gi.repository import GLib
except ImportError as exc:  # pragma: no cover - 环境缺依赖时明确失败
    print(f"fake-xdg-portal: missing dependency: {exc}", file=sys.stderr)
    sys.exit(2)

BUS_NAME = "org.freedesktop.portal.Desktop"
OBJECT_PATH = "/org/freedesktop/portal/desktop"
IFACE = "org.freedesktop.portal.Settings"
NS = "org.freedesktop.appearance"
KEY = "color-scheme"


class Portal(dbus.service.Object):
    def __init__(self, bus, control_file, initial, delay_ms=0):
        super().__init__(bus, OBJECT_PATH)
        self.control_file = control_file
        self.scheme = int(initial)
        self.delay_ms = delay_ms
        self.last_raw = self._read_control()

    def _read_control(self):
        try:
            with open(self.control_file, "r", encoding="utf-8") as f:
                return f.read().strip()
        except OSError:
            return ""

    @dbus.service.method(IFACE, in_signature="ss", out_signature="v")
    def ReadOne(self, namespace, key):
        if self.delay_ms > 0:
            time.sleep(self.delay_ms / 1000.0)   # 模拟慢 portal（e2e 延迟用）
        if namespace == NS and key == KEY:
            return dbus.UInt32(self.scheme)
        return dbus.UInt32(0)

    @dbus.service.method(IFACE, in_signature="ss", out_signature="v")
    def Read(self, namespace, key):
        if self.delay_ms > 0:
            time.sleep(self.delay_ms / 1000.0)
        # 老 portal 实现可能返回“variant 里再包一层 variant”；
        # dbus-python 的 out_signature 'v' 只包一层，这里保持单层即可
        # （客户端的递归解析在真实环境已验证两层）。
        if namespace == NS and key == KEY:
            return dbus.UInt32(self.scheme)
        return dbus.UInt32(0)

    @dbus.service.method(IFACE, in_signature="as", out_signature="a{sa{sv}}")
    def ReadAll(self, namespaces):
        return {
            NS: {KEY: dbus.UInt32(self.scheme)},
        }

    @dbus.service.signal(IFACE, signature="ssv")
    def SettingChanged(self, namespace, key, value):
        pass

    def _poll_control(self):
        raw = self._read_control()
        if raw != self.last_raw:
            self.last_raw = raw
            try:
                value = int(raw)
            except (TypeError, ValueError):
                value = self.scheme
            if value != self.scheme:
                self.scheme = value
                self.SettingChanged(NS, KEY, dbus.UInt32(value))
        return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--initial", type=int, default=1)
    parser.add_argument("--control-file", required=True)
    parser.add_argument("--delay-ms", type=int, default=0)
    args = parser.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName(BUS_NAME, bus)
    portal = Portal(bus, args.control_file, args.initial, args.delay_ms)
    # 立即写回初始值，避免 client 看到空控制文件。
    with open(args.control_file, "w", encoding="utf-8") as f:
        f.write(str(args.initial))
    portal.last_raw = str(args.initial)

    GLib.timeout_add(100, portal._poll_control)
    print("fake-xdg-portal: ready", flush=True)
    GLib.MainLoop().run()
    del name


if __name__ == "__main__":
    main()
