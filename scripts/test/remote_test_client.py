#!/usr/bin/env python3
"""远程音乐源 E2E 的夹具与断言客户端（不依赖 python-gi / ffmpeg）。

子命令：
  serve --kind http|webdav|ftp --root DIR [--auth user:pass] [--trickle]
        [--cert FILE --key FILE] [--port-file FILE]
      本地协议夹具服务器；端口写入 --port-file（内容 "PORT=<n>"）。
  pty-session --bin BIN --url URL --home DIR [--cache DIR] [--wait SEC]
        [--action quit|cancel-on-part] [--frontend local|remote]
        [--query-method queue|info|config] [--result FILE] [--log-dir DIR]
        [--env K=V ...]
      用 pty 启动 TUI，等待下载/进入会话，可选在退出前用 gdbus 取核心状态，
      然后按动作退出；结果 JSON 写到 --result（并打印）。
  wait-core [--timeout SEC]
  info | queue OFFSET COUNT | config      （gdbus 调用，输出 JSON 文本）

这些工具只用于 scripts/test/remote-frontend-e2e.sh，不参与产品构建。
"""

import argparse
import ast
import base64
import glob
import html
import json
import os
import pty
import re
import select
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BUS_NAME = "org.mpris.MediaPlayer2.ter_music"
OBJECT_PATH = "/org/mpris/MediaPlayer2"
INFO_IFACE = "cn.org.huansoft.ter_music.Info"
QUEUE_IFACE = "cn.org.huansoft.ter_music.Queue"
CONFIG_IFACE = "cn.org.huansoft.ter_music.Config"


# ── D-Bus（gdbus，无 python-gi） ─────────────────────────────────────


def gdbus_call(method, *args, timeout=15):
    command = [
        "gdbus", "call", "--session", "--dest", BUS_NAME,
        "--object-path", OBJECT_PATH, "--method", method,
    ] + [str(a) for a in args]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr.strip() or "gdbus call failed")
    value = ast.literal_eval(completed.stdout.strip())
    if isinstance(value, tuple):
        value = value[0] if value else ""
    if not isinstance(value, str):
        raise RuntimeError("unexpected gdbus reply type: %r" % (value,))
    return value


def core_json(method, *args):
    return json.loads(gdbus_call(method, *args))


def cmd_wait_core(args):
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        completed = subprocess.run(
            ["gdbus", "introspect", "--session", "--dest", BUS_NAME,
             "--object-path", OBJECT_PATH],
            capture_output=True, text=True)
        if completed.returncode == 0:
            print("CORE_READY")
            return 0
        time.sleep(0.1)
    print("CORE_TIMEOUT", file=sys.stderr)
    return 1


def cmd_info(_args):
    print(json.dumps(core_json(INFO_IFACE + ".GetInfo"), ensure_ascii=False))
    return 0


def cmd_queue(args):
    print(json.dumps(core_json(QUEUE_IFACE + ".Get", args.offset, args.count),
                     ensure_ascii=False))
    return 0


def cmd_config(_args):
    print(json.dumps(core_json(CONFIG_IFACE + ".GetAll"), ensure_ascii=False))
    return 0


# ── 音频夹具（python wave，替代 ffmpeg） ────────────────────────────


def write_wave(path, frequency=440, seconds=0.15, rate=8000):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    frames = bytearray()
    samples = int(rate * seconds)
    for i in range(samples):
        value = int(9000 * (1 if (i * frequency // rate) % 2 else -1))
        frames += struct.pack("<h", value)
    with wave.open(path, "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(bytes(frames))


# ── HTTP autoindex / 文件服务 ───────────────────────────────────────


def make_handler(root, auth=None, trickle=False):
    root = os.path.realpath(root)

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):  # noqa: D401 - 静默
            pass

        def _check_auth(self):
            if not auth:
                return True
            expected = "Basic " + base64.b64encode(auth.encode()).decode()
            if self.headers.get("Authorization", "") == expected:
                return True
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="remote-e2e"')
            self.send_header("Content-Length", "0")
            self.end_headers()
            return False

        def _resolve(self):
            path = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
            candidate = os.path.realpath(os.path.join(root, path.lstrip("/")))
            if candidate != root and not candidate.startswith(root + os.sep):
                return None, path
            return candidate, path

        def _send_dir_listing(self, filesystem_path, url_path):
            if not url_path.endswith("/"):
                self.send_response(301)
                self.send_header("Location", url_path + "/")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            rows = ['<html><body><h1>Index of %s</h1><ul>' % html.escape(url_path)]
            rows.append('<li><a href="../">../</a></li>')
            for name in sorted(os.listdir(filesystem_path)):
                full = os.path.join(filesystem_path, name)
                href = urllib.parse.quote(name + ("/" if os.path.isdir(full) else ""))
                display = html.escape(name + ("/" if os.path.isdir(full) else ""))
                rows.append('<li><a href="%s">%s</a></li>' % (href, display))
            rows.append("</ul></body></html>")
            data = "".join(rows).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):  # noqa: N802
            if not self._check_auth():
                return
            filesystem_path, url_path = self._resolve()
            if filesystem_path is None:
                self.send_error(403)
                return
            if os.path.isdir(filesystem_path):
                self._send_dir_listing(filesystem_path, url_path)
                return
            if not os.path.isfile(filesystem_path):
                self.send_error(404)
                return
            data = open(filesystem_path, "rb").read()
            self.send_response(200)
            content_type = "audio/wav" if filesystem_path.endswith(".wav") else "application/octet-stream"
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            if trickle and filesystem_path.endswith(".wav"):
                offset = 0
                while offset < len(data):
                    try:
                        self.wfile.write(data[offset:offset + 512])
                        self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError):
                        return
                    offset += 512
                    time.sleep(0.5)
            else:
                self.wfile.write(data)

    return Handler


def make_webdav_handler(root, auth=None):
    base = make_handler(root, auth=auth)

    class WebdavHandler(base):
        def do_PROPFIND(self):  # noqa: N802
            if not self._check_auth():
                return
            filesystem_path, url_path = self._resolve()
            if filesystem_path is None or not os.path.isdir(filesystem_path):
                self.send_error(404)
                return
            if not url_path.endswith("/"):
                url_path += "/"
            entries = []
            # 目录自身（客户端应按“当前目录”跳过）
            entries.append((url_path, True, 0))
            for name in sorted(os.listdir(filesystem_path)):
                full = os.path.join(filesystem_path, name)
                href = urllib.parse.quote(url_path + name + ("/" if os.path.isdir(full) else ""))
                size = 0 if os.path.isdir(full) else os.path.getsize(full)
                entries.append((href, os.path.isdir(full), size))
            parts = ['<?xml version="1.0" encoding="utf-8"?>',
                     '<D:multistatus xmlns:D="DAV:">']
            for href, is_dir, size in entries:
                collection = "<D:collection/>" if is_dir else ""
                parts.append(
                    "<D:response><D:href>%s</D:href>"
                    "<D:propstat><D:prop><D:resourcetype>%s</D:resourcetype>"
                    "<D:getcontentlength>%d</D:getcontentlength>"
                    "</D:prop></D:propstat></D:response>"
                    % (html.escape(href), collection, size))
            parts.append("</D:multistatus>")
            data = "".join(parts).encode("utf-8")
            self.send_response(207)
            self.send_header("Content-Type", "application/xml; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    return WebdavHandler


def serve_http(args):
    handler = make_handler(args.root, auth=args.auth, trickle=args.trickle)
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    if args.cert and args.key:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    announce_port(server.server_address[1], args.port_file)
    server.serve_forever()


def serve_webdav(args):
    handler = make_webdav_handler(args.root, auth=args.auth)
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    if args.cert and args.key:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    announce_port(server.server_address[1], args.port_file)
    server.serve_forever()


def serve_ftp(args):
    try:
        from pyftpdlib.authorizers import DummyAuthorizer
        from pyftpdlib.handlers import FTPHandler
        from pyftpdlib.servers import FTPServer
    except ImportError:
        print("ERROR: pyftpdlib 不可用（FTP 夹具需要它）", file=sys.stderr)
        return 3
    authorizer = DummyAuthorizer()
    authorizer.add_user(args.user, args.password, args.root, perm="elradfmwMT")
    handler = FTPHandler
    handler.authorizer = authorizer
    handler.passive_ports = range(41000, 42000)
    server = FTPServer(("127.0.0.1", 0), handler)
    announce_port(server.socket.getsockname()[1], args.port_file)
    server.serve_forever()


def announce_port(port, port_file):
    if port_file:
        with open(port_file, "w", encoding="utf-8") as handle:
            handle.write("PORT=%d\n" % port)
    print("PORT=%d" % port, flush=True)


def cmd_serve(args):
    if args.kind == "http":
        return serve_http(args)
    if args.kind == "webdav":
        return serve_webdav(args)
    if args.kind == "ftp":
        return serve_ftp(args)
    print("unknown --kind %s" % args.kind, file=sys.stderr)
    return 2


# ── pty 驱动 TUI ───────────────────────────────────────────────────


class PtySession:
    def __init__(self, argv, env, cwd=None):
        self.output = bytearray()
        self.pid, self.fd = pty.fork()
        if self.pid == 0:  # pragma: no cover - 子进程
            try:
                if cwd:
                    os.chdir(cwd)
                os.execvpe(argv[0], argv, env)
            finally:
                os._exit(127)
        os.set_blocking(self.fd, False)

    def drain(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                return
            if not chunk:
                return
            self.output.extend(chunk)
            if len(self.output) > 262144:
                del self.output[:131072]

    def alive(self):
        pid, _status = os.waitpid(self.pid, os.WNOHANG)
        return pid == 0

    def send(self, data):
        try:
            os.write(self.fd, data)
        except OSError:
            pass

    def wait_exit(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid != 0:
                if os.WIFEXITED(status):
                    return os.WEXITSTATUS(status)
                if os.WIFSIGNALED(status):
                    return -os.WTERMSIG(status)
                return -1
            self.drain(0.05)
        return None

    def kill(self):
        try:
            os.kill(self.pid, 9)
        except OSError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except OSError:
            pass

    def tail(self, limit=2000):
        return bytes(self.output[-limit:]).decode("utf-8", "replace")


def find_part_files(cache_root):
    return glob.glob(os.path.join(cache_root, "**", "*.part"), recursive=True)


def cmd_pty_session(args):
    home = os.path.realpath(args.home)
    cache = os.path.realpath(args.cache or os.path.join(home, ".cache"))
    log_dir = args.log_dir or os.path.join(home, "logs")
    os.makedirs(home, exist_ok=True)
    os.makedirs(cache, exist_ok=True)
    os.makedirs(log_dir, exist_ok=True)

    env = os.environ.copy()
    env.update({
        "HOME": home,
        "XDG_CONFIG_HOME": os.path.join(home, ".config"),
        "XDG_CACHE_HOME": cache,
        "TERM": "xterm-256color",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TER_MUSIC_LOG_DIR": log_dir,
    })
    for pair in args.env or []:
        if "=" not in pair:
            print("bad --env %s" % pair, file=sys.stderr)
            return 2
        key, value = pair.split("=", 1)
        env[key] = value

    argv = [args.bin]
    if args.debug:
        argv.append("--debug")
    if args.frontend:
        argv.append("--frontend=" + args.frontend)
    argv.append(args.url)

    session = PtySession(argv, env, cwd=args.cwd)
    result = {
        "argv": argv,
        "alive_before": False,
        "exit_code": None,
        "exit_ms": None,
        "part_seen_ms": None,
        "parts_after": None,
        "query": None,
    }

    session.drain(args.wait)
    result["alive_before"] = session.alive()

    action = args.action
    start = time.time()

    if action == "cancel-on-part":
        cache_root = os.path.join(cache, "ter-music", "remote")
        deadline = start + args.part_timeout
        seen = False
        while time.time() < deadline:
            if find_part_files(cache_root):
                seen = True
                result["part_seen_ms"] = int((time.time() - start) * 1000)
                break
            if not session.alive():
                break
            session.drain(0.05)
        if not seen:
            result["part_seen_ms"] = None

    if args.query_method:
        try:
            if args.query_method == "queue":
                result["query"] = core_json(QUEUE_IFACE + ".Get", 0, 100)
            elif args.query_method == "info":
                result["query"] = core_json(INFO_IFACE + ".GetInfo")
            elif args.query_method == "config":
                result["query"] = core_json(CONFIG_IFACE + ".GetAll")
        except Exception as exc:                      # noqa: BLE001
            result["query_error"] = str(exc)

    quit_start = time.time()
    session.send(b"q")
    code = session.wait_exit(8.0)
    result["exit_ms"] = int((time.time() - quit_start) * 1000)
    if code is None:
        session.kill()
        code = None
    result["exit_code"] = code
    session.drain(0.2)

    cache_root = os.path.join(cache, "ter-music", "remote")
    result["parts_after"] = len(find_part_files(cache_root))
    result["output_tail"] = session.tail()
    result["ok"] = bool(result["alive_before"] and code == 0)

    if args.result:
        with open(args.result, "w", encoding="utf-8") as handle:
            json.dump(result, handle, ensure_ascii=False, indent=1)
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result["ok"] else 1


# ── 入口 ───────────────────────────────────────────────────────────


def main(argv):
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    serve = sub.add_parser("serve")
    serve.add_argument("--kind", choices=["http", "webdav", "ftp"], required=True)
    serve.add_argument("--root", required=True)
    serve.add_argument("--auth", default="")
    serve.add_argument("--user", default="e2e")
    serve.add_argument("--password", default="e2e-pass")
    serve.add_argument("--trickle", action="store_true")
    serve.add_argument("--cert", default="")
    serve.add_argument("--key", default="")
    serve.add_argument("--port-file", default="")

    pty_parser = sub.add_parser("pty-session")
    pty_parser.add_argument("--bin", required=True)
    pty_parser.add_argument("--url", required=True)
    pty_parser.add_argument("--home", required=True)
    pty_parser.add_argument("--cache", default="")
    pty_parser.add_argument("--cwd", default="")
    pty_parser.add_argument("--wait", type=float, default=5.0)
    pty_parser.add_argument("--action", choices=["quit", "cancel-on-part"], default="quit")
    pty_parser.add_argument("--frontend", choices=["local", "remote"], default="")
    pty_parser.add_argument("--query-method", choices=["queue", "info", "config"], default="")
    pty_parser.add_argument("--result", default="")
    pty_parser.add_argument("--log-dir", default="")
    pty_parser.add_argument("--debug", action="store_true", default=True)
    pty_parser.add_argument("--part-timeout", type=float, default=20.0)
    pty_parser.add_argument("--env", action="append", default=[])

    wait_core = sub.add_parser("wait-core")
    wait_core.add_argument("--timeout", type=float, default=15.0)

    sub.add_parser("info")
    queue = sub.add_parser("queue")
    queue.add_argument("offset", type=int, nargs="?", default=0)
    queue.add_argument("count", type=int, nargs="?", default=100)
    sub.add_parser("config")

    args = parser.parse_args(argv)
    if args.command == "serve":
        return cmd_serve(args)
    if args.command == "pty-session":
        return cmd_pty_session(args)
    if args.command == "wait-core":
        return cmd_wait_core(args)
    if args.command == "info":
        return cmd_info(args)
    if args.command == "queue":
        return cmd_queue(args)
    if args.command == "config":
        return cmd_config(args)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
