#!/usr/bin/env python3
"""
perf-driver.py — A6 的时延探针（保持一条连接的 D-Bus 客户端）

用法：
    perf-driver.py latency <bus_name> [samples]

在被测实例上反复执行「发命令 → 轮询快照直到值可见」，打印每次的毫秒数与中位数。
之所以轮询 `Info.GetInfo` 而不是等信号：这测的是"命令在核心侧生效并被下一次读数
看见"的时延，与界面按 40 ms 事件循环取快照的路径一致（排除按键本身的终端延迟）。

为什么不用 gdbus/busctl：两者每次调用都要新起进程，进程启动就会淹没毫秒级差异。
本脚本用 Gio 保持一条连接，轮询开销在进程内。
"""

import statistics
import sys
import time

try:
    import gi  # noqa: F401
    gi.require_version("Gio", "2.0")
    from gi.repository import Gio, GLib  # noqa: E402
except Exception as exc:                                        # noqa: BLE001
    print("SKIP 缺少 PyGObject/Gio：%s" % exc, file=sys.stderr)
    raise SystemExit(3)

OBJECT_PATH = "/org/mpris/MediaPlayer2"
IFACE_INFO = "org.yxzl.ter_music.Info"
IFACE_CONTROL = "org.yxzl.ter_music.Control"


def connect(bus_name):
    return Gio.bus_get_sync(Gio.BusType.SESSION, None)


def call(conn, bus_name, iface, method, args=None, timeout_ms=5000):
    return conn.call_sync(bus_name, OBJECT_PATH, iface, method,
                          args, None, Gio.DBusCallFlags.NONE, timeout_ms, None)


def info_json(conn, bus_name):
    reply = call(conn, bus_name, IFACE_INFO, "GetInfo")
    text = reply.unpack()[0]
    import json
    return json.loads(text)


def time_until(conn, bus_name, send, read, want, deadline_ms=2000):
    """send() 发命令；read(doc) 从快照取值；等它满足 want() 为止。返回毫秒。"""
    start = time.perf_counter()
    send()
    while True:
        doc = info_json(conn, bus_name)
        if want(read(doc)):
            return (time.perf_counter() - start) * 1000.0
        if (time.perf_counter() - start) * 1000.0 > deadline_ms:
            return None
        time.sleep(0.001)


def cmd_latency(conn, bus_name, samples):
    docs = info_json(conn, bus_name)
    start_volume = docs["playback"]["volume_percent"]
    start_mode = docs["playback"]["play_mode_index"]

    volume_lat = []
    mode_lat = []
    for i in range(samples):
        want_volume = 40 if start_volume != 40 else 60
        ms = time_until(conn, bus_name,
                        lambda: call(conn, bus_name, IFACE_CONTROL, "SetVolume",
                                     GLib.Variant("(i)", (want_volume,))),
                        lambda d: d["playback"]["volume_percent"],
                        lambda v: v == want_volume)
        if ms is not None:
            volume_lat.append(ms)
        start_volume = want_volume

        want_mode = 2 if start_mode != 2 else 0
        ms = time_until(conn, bus_name,
                        lambda: call(conn, bus_name, IFACE_CONTROL, "SetPlayMode",
                                     GLib.Variant("(i)", (want_mode,))),
                        lambda d: d["playback"]["play_mode_index"],
                        lambda v: v == want_mode)
        if ms is not None:
            mode_lat.append(ms)
        start_mode = want_mode

    for label, values in (("SetVolume", volume_lat), ("SetPlayMode", mode_lat)):
        if not values:
            print("%s 无有效样本" % label)
            continue
        print("%s 中位数 %.1f ms（%s）" % (label, statistics.median(values),
                                          " ".join("%.1f" % v for v in values)))
    if not volume_lat or not mode_lat:
        return 1
    return 0


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, bus_name = argv[1], argv[2]
    conn = connect(bus_name)
    if cmd == "latency":
        samples = int(argv[3]) if len(argv) > 3 else 5
        return cmd_latency(conn, bus_name, samples)
    print("未知子命令: %s" % cmd, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
