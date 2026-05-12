#!/usr/bin/env python3
"""
Minimal HTTP test backend for nginx-flowlens.
Only provides endpoints actually needed by test.sh.

Usage:
    python3 backend.py [PORT]
"""

import http.server
import json
import socketserver
import sys
import time
import urllib.parse

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18100


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass  # suppress default logging

    def _send_json(self, data, status=200, extra_headers=None):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self):
        content_len = int(self.headers.get("Content-Length", 0))
        if content_len > 0:
            return self.rfile.read(content_len)
        return b""

    # ===================== GET =====================

    def do_GET(self):
        path = self.path

        if path == "/status":
            self._send_json({"ok": True})

        elif path == "/json":
            self._send_json({"message": "hello", "test": True})

        elif path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            for i in range(5):
                chunk = f"chunk-{i}\n".encode()
                self.wfile.write(f"{len(chunk):x}\r\n".encode())
                self.wfile.write(chunk)
                self.wfile.write(b"\r\n")
            self.wfile.write(b"0\r\n\r\n")

        elif path == "/sse":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            for i in range(3):
                self.wfile.write(f"data: event-{i}\n\n".encode())
                self.wfile.flush()
                time.sleep(0.03)

        elif path == "/gzip-target":
            # Return plain JSON; nginx gzip filter compresses for the client
            body = json.dumps({"message": "hello", "gzip_test": True}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        elif path.startswith("/large"):
            query = urllib.parse.urlparse(path).query
            params = urllib.parse.parse_qs(query)
            size_str = params.get("size", ["5m"])[0]
            size = self._parse_size(size_str)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            chunk = b"X" * (64 * 1024)
            remaining = size
            while remaining > 0:
                to_write = min(len(chunk), remaining)
                self.wfile.write(chunk[:to_write])
                remaining -= to_write

        elif path == "/slow":
            time.sleep(0.3)
            self._send_json({"delayed": True})

        else:
            self.send_error(404)

    def _parse_size(self, s):
        s = s.lower().strip()
        multipliers = {"k": 1024, "m": 1024 * 1024, "g": 1024 * 1024 * 1024}
        for suffix, mult in multipliers.items():
            if s.endswith(suffix):
                return int(s[:-1]) * mult
        return int(s)

    # ===================== POST / PUT / PATCH / DELETE =====================

    def do_POST(self):
        self._handle_echo("POST")

    def do_PUT(self):
        self._handle_echo("PUT")

    def do_PATCH(self):
        self._handle_echo("PATCH")

    def do_DELETE(self):
        self._handle_echo("DELETE")

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Allow", "GET, POST, PUT, PATCH, DELETE, OPTIONS")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

    def _handle_echo(self, method):
        body = self._read_body()
        self._send_json({
            "method": method,
            "path": self.path,
            "received_bytes": len(body),
        })


class ReuseAddrTCPServer(socketserver.TCPServer):
    allow_reuse_address = True


if __name__ == "__main__":
    with ReuseAddrTCPServer(("127.0.0.1", PORT), Handler) as httpd:
        print(f"Test backend listening on port {PORT}", flush=True)
        httpd.serve_forever()
