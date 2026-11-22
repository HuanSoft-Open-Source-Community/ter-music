#!/usr/bin/env python3
"""XDG desktop portal 的 pty e2e（t16 A）。

必须在 dbus-run-session 内运行；脚本自己启动 scripts/test/fake-xdg-portal.py
提供 org.freedesktop.portal.Settings。复用 appearance-pty-e2e.py 的 run_case
（临时 HOME/XDG_CONFIG_HOME + pty）。

用法：
  dbus-run-session -- script -qefc \
    "python3 scripts/test/appearance-xdg-pty-e2e.py --bin build-ld/ter-music" /dev/null
"""

import argparse
import importlib.util
import os
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BASE_PATH = os.path.join(HERE, "appearance-pty-e2e.py")
FAKE_PORTAL = os.path.join(HERE, "fake-xdg-portal.py")

_spec = importlib.util.spec_from_file_location("appearance_pty_e2e", BASE_PATH)
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)

CASES = []
FAILURES = 0


def case(fn):
    CASES.append(fn)
    return fn


CONFIG_FILE = os.path.join(HERE, "xdg-session.conf")


class PrivateBus:
    """自管 dbus-daemon（无 service activation），避免宿主 portal 干扰。"""

    def __init__(self):
        self.proc = subprocess.Popen(
            ["dbus-daemon", "--config-file", CONFIG_FILE,
             "--print-address=1", "--nofork"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        address = self.proc.stdout.readline().strip()
        if not address:
            raise RuntimeError("dbus-daemon did not print an address")
        self.address = address
        os.environ["DBUS_SESSION_BUS_ADDRESS"] = address

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()


class FakePortal:
    def __init__(self, initial, delay_ms=0):
        self.control = tempfile.mktemp(prefix="tm-xdg-scheme-", dir="/tmp")
        args = [sys.executable, FAKE_PORTAL, "--initial", str(initial),
                "--control-file", self.control]
        if delay_ms:
            args += ["--delay-ms", str(delay_ms)]
        self.proc = subprocess.Popen(
            args,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        deadline = time.time() + 4.0
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if "ready" in line:
                return
            if self.proc.poll() is not None:
                raise RuntimeError("fake portal exited: " +
                                   (self.proc.stderr.read() or ""))
        raise RuntimeError("fake portal did not become ready")

    def set_scheme(self, value):
        with open(self.control, "w", encoding="utf-8") as f:
            f.write(str(value))

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        try:
            os.unlink(self.control)
        except OSError:
            pass


def quiesce():
    """终止 fake portal 后等 session bus 释放名字，避免下个场景读到旧服务。"""
    time.sleep(0.25)


@case
def xdg_dark(binary):
    portal = FakePortal(1)
    try:
        r = base.run_case(binary, "xdg-dark", mode=0, duration=3.2)
    finally:
        portal.stop(); quiesce()
    checks = [
        (bool(r.colours & {37, 198, 208}), "XDG prefer-dark 使用暗色方案"),
        (not (r.colours & {236, 94, 28, 90}), "暗色下不出现亮色变体"),
        (r.status == 0, "进程正常退出"),
    ]
    return r, checks


@case
def xdg_light(binary):
    portal = FakePortal(2)
    try:
        r = base.run_case(binary, "xdg-light", mode=0, duration=4.6,
                          snapshots=[(4.0, "settings")],
                          key_steps=[(2.7, b"\x1bOQ"), (3.1, b"\x1bOC")])
    finally:
        portal.stop(); quiesce()
    settings = base.screen_text(r, "settings")
    checks = [
        (bool(r.colours & {236, 94, 28, 24, 90}), "XDG prefer-light 使用亮色方案"),
        (not (r.colours & {37, 198, 208}), "亮色下不出现暗色预设专属色"),
        ("系统外观" in settings and "亮色" in settings,
         "Settings 检测行显示来源“系统外观”与亮色结论"),
        (r.status == 0, "进程正常退出"),
    ]
    return r, checks


@case
def xdg_beats_colorfgbg(binary):
    portal = FakePortal(1)
    try:
        r = base.run_case(binary, "xdg-priority", mode=0,
                          colorfgbg="0;15", duration=3.2)
    finally:
        portal.stop(); quiesce()
    checks = [
        (bool(r.colours & {37, 198, 208}),
         "XDG（系统外观）优先于 COLORFGBG"),
        (not (r.colours & {236, 94, 28, 90}), "不采用 COLORFGBG 的亮色"),
        (r.status == 0, "进程正常退出"),
    ]
    return r, checks


@case
def osc_beats_xdg(binary):
    portal = FakePortal(2)
    try:
        r = base.run_case(binary, "osc-priority", mode=0,
                          replies=[b"\x1b]11;rgb:0000/0000/0000\x1b\\"],
                          duration=3.4)
    finally:
        portal.stop(); quiesce()
    checks = [
        (r.queries >= 1, "已尝试 OSC11"),
        (bool(r.colours & {37, 198, 208}),
         "OSC11 实际底色（暗）优先于 XDG 亮色偏好"),
        (not (r.colours & {236, 94, 28, 90}), "不采用 XDG 的亮色"),
        (r.status == 0, "进程正常退出"),
    ]
    return r, checks


@case
def xdg_setting_changed(binary):
    portal = FakePortal(1)
    timer = threading.Timer(3.0, portal.set_scheme, [2])
    timer.daemon = True
    timer.start()
    try:
        r = base.run_case(binary, "xdg-changed", mode=0, duration=5.4,
                          snapshots=[(4.8, "after_change")])
    finally:
        portal.stop(); quiesce()
    checks = [
        (bool(r.colours & {236, 94, 28, 90}),
         "SettingChanged 后无需重启即切换亮色"),
        (236 in r.colours and 94 in r.colours,
         "亮色专属色仅在信号后出现"),
        (r.status == 0, "进程正常退出"),
    ]
    return r, checks


@case
def xdg_osc_authority(binary):
    # OSC11 已应答后，XDG 变化不直接改观感，只触发一次非阻塞 OSC11 重探。
    portal = FakePortal(1)
    timer = threading.Timer(3.0, portal.set_scheme, [2])
    timer.daemon = True
    timer.start()
    black = b"\x1b]11;rgb:0000/0000/0000\x1b\\"
    try:
        r = base.run_case(binary, "xdg-osc-authority", mode=0,
                          replies=[black, black], duration=6.8,
                          snapshots=[(6.0, "settings")],
                          key_steps=[(5.0, b"\x1bOQ"), (5.4, b"\x1bOC")])
    finally:
        portal.stop(); quiesce()
    checks = [
        (r.queries >= 2, "XDG 变化触发了一次 OSC11 重探（queries=%d）" % r.queries),
        (bool(r.colours & {37, 198, 208}),
         "OSC11 实际底色（暗）保持权威，不被 XDG 亮色覆盖"),
        (not (r.colours & {236, 94, 28, 90}), "未切换到 XDG 亮色"),
        ("终端应答" in base.screen_text(r, "settings"),
         "Settings 检测行显示来源“终端应答”"),
        (r.status == 0, "进程正常退出"),
    ]
    return r, checks


@case
def xdg_mode_cycle(binary):
    # F4：以显式 Dark 启动 → Settings 切回 Auto 时必须重新完整采集，
    # 跟随当前 XDG（fake portal 在此前已改成 light），不能沿用旧信号。
    portal = FakePortal(2)
    try:
        r = base.run_case(binary, "xdg-mode-cycle", mode=2, duration=6.6,
                          snapshots=[(5.6, "settings")],
                          key_steps=[(2.6, b"\x1bOQ"), (3.0, b"\x1bOC"),
                                     (3.4, b"\x1bOA"), (3.7, b"\x1bOA"),
                                     (4.1, b"\x1bOC"),   # Dark -> Auto
                                     (5.8, b"\x1b"), (6.2, b"q")])
    finally:
        portal.stop(); quiesce()
    settings = base.screen_text(r, "settings")
    checks = [
        (bool(r.colours & {236, 94, 28, 90}),
         "切回 Auto 后重新采集并采用 XDG 亮色"),
        ("系统外观" in settings and "亮色" in settings,
         "Settings 显示重新采集后的来源/结论"),
        (base.saved_value(r.saved, "appearance_mode") == 0,
         "全局模式持久化为 auto"),
        (r.status == 0, "进程正常退出"),
    ]
    return r, checks


@case
def xdg_two_changes_timeout(binary):
    # F5：OSC11 有过应答、其后两次 XDG 变化都无人应答 OSC11 时，
    # 3s 窗口必须过期清零，第二次变化仍能再触发一次 OSC11 重探。
    portal = FakePortal(1)
    t1 = threading.Timer(3.0, portal.set_scheme, [2])
    t2 = threading.Timer(8.0, portal.set_scheme, [1])
    for timer in (t1, t2):
        timer.daemon = True
        timer.start()
    black = b"\x1b]11;rgb:0000/0000/0000\x1b\\"
    try:
        r = base.run_case(binary, "xdg-two-changes", mode=0,
                          replies=[black], duration=11.0)
    finally:
        portal.stop(); quiesce()
    checks = [
        (r.queries >= 3,
         "两次 XDG 变化各触发一次 OSC11 重探（queries=%d，期望≥3）" % r.queries),
        (bool(r.colours & {37, 198, 208}),
         "OSC11 权威保持暗色（未被 XDG 两次变化改写）"),
        (r.status == 0, "进程正常退出"),
    ]
    return r, checks


@case
def xdg_theme_unknown_async_fallback(binary):
    # t24 F2：主题 known→unknown 且 portal 慢响应时，回退读取不得阻塞输入。
    portal = FakePortal(2, delay_ms=800)   # 系统偏好亮色，但慢 800ms

    def prep(binary_, env, cfg_dir, root):
        env["TERM_PROGRAM"] = "deepin-terminal"
        # 用自定义命名主题，避免 /usr/share/terminalwidget6 的内置 Dark
        # 在文件删除后继续命中（真实系统确实有这两套内置方案）。
        base.deepin_theme_setup(root, theme="Dark", expand="MyDark",
                                named_name="MyDark", named_bg=37)
        scheme = os.path.join(root, "home", ".local", "share",
                              "terminalwidget6", "color-schemes",
                              "MyDark.colorscheme")

        def remove_theme():
            try:
                os.unlink(scheme)
            except OSError:
                pass

        timer = threading.Timer(2.5, remove_theme)
        timer.daemon = True
        timer.start()

    try:
        r = base.run_case(binary, "xdg-theme-async", mode=0, prep=prep,
                          duration=7.4, snapshots=[(6.6, "settings")],
                          key_steps=[(5.7, b"\x1bOQ")],   # fallback 生效后再开 Settings
                          latency_keys=[(4.0, b"1", "mode-1"),
                                        (4.4, b"2", "mode-2"),
                                        (4.8, b"3", "mode-3")])
    finally:
        portal.stop(); quiesce()
    settings = base.screen_text(r, "settings")
    latencies = r.latencies or []
    max_latency = max(latencies) if latencies else 99.0
    checks = [
        (len(latencies) >= 3, "采集到 3 次按键→输出延迟（%d）" % len(latencies)),
        (max_latency < 0.10,
         "适配器失效回退不再冻结输入（max=%.3fs < 0.10s）" % max_latency),
        (bool(r.colours & base.LIGHT_ONLY),
         "慢 portal 的异步结果最终回退为亮色"),
        ("系统外观" in settings and "亮色" in settings,
         "Settings 显示最终来源“系统外观”与亮色"),
        (r.status == 0, "进程正常退出"),
    ]
    return r, checks


@case
def xdg_no_portal(binary):
    # 有 session bus 但没有 portal 服务：回退 COLORFGBG，不挂起。
    r = base.run_case(binary, "xdg-no-portal", mode=0,
                      colorfgbg="0;15", duration=3.2)
    checks = [
        (bool(r.colours & {236, 94, 28, 90}), "无 portal 时回退 COLORFGBG 亮色"),
        (r.status == 0, "无 portal 不挂起"),
    ]
    return r, checks


@case
def xdg_no_bus(binary):
    # 无 session bus/无 XDG_RUNTIME_DIR：不得调用 libdbus/autolaunch，直接回退。
    def prep(binary_, env, cfg_dir, root):
        env.pop("DBUS_SESSION_BUS_ADDRESS", None)
        env.pop("XDG_RUNTIME_DIR", None)

    r = base.run_case(binary, "xdg-no-bus", mode=0, prep=prep, duration=3.0)
    checks = [
        (bool(r.colours & {37, 198, 208}), "无总线时保持暗色回退"),
        (r.status == 0, "无总线不崩溃/不挂起"),
    ]
    return r, checks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    parser.add_argument("--only", default=None,
                        help="只跑一个场景（函数名）")
    args = parser.parse_args()
    binary = os.path.abspath(args.bin)
    private_bus = PrivateBus()

    global FAILURES
    passed = 0
    for fn in CASES:
        if args.only and fn.__name__ != args.only:
            continue
        name = fn.__name__
        try:
            result, checks = fn(binary)
            bad = [text for ok, text in checks if not ok]
            if bad:
                FAILURES += 1
                print(f"FAIL {name}")
                for text in bad:
                    print(f"      - {text}")
                print(f"      queries={result.queries} replies={result.replies} "
                      f"status={result.status} colours={sorted(result.colours)}")
            else:
                passed += 1
                print(f"PASS {name}")
        except Exception as exc:  # noqa: BLE001 - e2e 需要给出具体异常
            FAILURES += 1
            print(f"FAIL {name}: {exc}")
    private_bus.stop()
    print(f"{passed} passed, {FAILURES} failed")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
