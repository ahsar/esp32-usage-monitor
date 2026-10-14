#!/usr/bin/env python3
"""Minimal stand-in for the device's status endpoint, used to verify the
OpenCode plugin end to end (including HMAC auth) without the hardware.

Checks the same signature the firmware does: HMAC-SHA256 over "<state>|<X-Ts>".

    python3 tools/status_stub.py <key> [port]
"""
import hashlib
import hmac
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

KEY = (sys.argv[1] if len(sys.argv) > 1 else "").encode()
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8099


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        q = parse_qs(urlparse(self.path).query)
        state = (q.get("state") or [""])[0]
        ts = self.headers.get("X-Ts", "")
        sig = self.headers.get("X-Sig", "")

        if KEY:
            if not ts or not sig:
                self._reply(401, "no signature")
                return
            want = hmac.new(KEY, f"{state}|{ts}".encode(), hashlib.sha256).hexdigest()
            if not hmac.compare_digest(want, sig):
                self._reply(401, "bad signature")
                return
        self._reply(200, state or "ok")

    def _reply(self, code, msg):
        # Print the accepted state so the test can assert on it.
        print(f"{code} {msg}", flush=True)
        self.send_response(code)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"OK")

    def log_message(self, *a):
        pass


HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
