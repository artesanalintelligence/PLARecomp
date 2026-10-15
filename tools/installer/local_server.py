#!/usr/bin/env python3
"""Local HTTP server with Range support, to test the setup's download fallback (light builds).

python -m http.server does not support Range requests, so resumed downloads could not be tested with it.
Serves one folder on 127.0.0.1 only and logs every request (method, path, Range, status, bytes).

    python tools/installer/local_server.py D:\\PLARecomp\\paquetes [--port 8765] [--log server.log] [--rate-kbps 0]
"""
from __future__ import annotations

import argparse
import http.server
import os
import re
import sys
import time
from pathlib import Path


def make_handler(root: Path, log_path: Path | None, rate: int):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):  # quiet default logging; we write our own lines
            pass

        def _log(self, status: int, sent: int) -> None:
            line = f"{time.strftime('%H:%M:%S')} {self.command} {self.path} range={self.headers.get('Range', '-')} -> {status} {sent} bytes"
            print(line, flush=True)
            if log_path:
                with open(log_path, "a", encoding="utf-8") as f:
                    f.write(line + "\n")

        def do_GET(self):
            rel = self.path.split("?", 1)[0].lstrip("/")
            p = (root / rel).resolve()
            if not str(p).startswith(str(root.resolve())) or not p.is_file():
                self.send_error(404)
                self._log(404, 0)
                return
            size = p.stat().st_size
            start, end, status = 0, size - 1, 200
            m = re.match(r"bytes=(\d+)-(\d*)$", self.headers.get("Range", ""))
            if m:
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else size - 1
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.end_headers()
                    self._log(416, 0)
                    return
                status = 206
            self.send_response(status)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Accept-Ranges", "bytes")
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            sent = 0
            with open(p, "rb") as f:
                f.seek(start)
                left = end - start + 1
                while left > 0:
                    chunk = f.read(min(1 << 16, left))
                    if not chunk:
                        break
                    try:
                        self.wfile.write(chunk)
                    except (ConnectionError, OSError):
                        break
                    sent += len(chunk)
                    left -= len(chunk)
                    if rate:
                        time.sleep(len(chunk) / (rate * 1024))
            self._log(status, sent)

    return Handler


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--log", type=Path)
    ap.add_argument("--rate-kbps", type=int, default=0, help="throttle (KB/s per request), 0 = unlimited")
    a = ap.parse_args()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(a.root, a.log, a.rate_kbps))
    print(f"serving {a.root} on http://127.0.0.1:{a.port}/", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
