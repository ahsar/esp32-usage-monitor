#!/usr/bin/env python3
"""Minimal stand-in for the device's status endpoint, used to verify the OpenCode
plugin *and* tools/status-settings.sh end to end (including HMAC auth) without the
hardware.

It checks the same signatures the firmware does:

    ?state=...   HMAC-SHA256 over "<state>|<X-Ts>"
    ?<settings>  HMAC-SHA256 over "<raw query>|<X-Ts>"   (e.g. "chime=0&brightness=4")

    python3 tools/status_stub.py <key> [port]

Then, in another shell:

    ESP32_MONITOR_KEY=<key> ESP32_MONITOR_URL=http://127.0.0.1:8099 \\
        tools/status-settings.sh chime=0 brightness=4
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
        query = urlparse(self.path).query
        q = parse_qs(query)
        # The device keeps `?state=` on the original value-only signature (so the
        # existing plugin is untouched) and signs the whole raw query for settings.
        if query.startswith("state="):
            payload = (q.get("state") or [""])[0]
        else:
            payload = query
        ts = self.headers.get("X-Ts", "")
        sig = self.headers.get("X-Sig", "")

        if not payload:
            self._reply(200, "ok")
            return
        if KEY:
            if not ts or not sig:
                self._reply(401, "no signature")
                return
            want = hmac.new(KEY, f"{payload}|{ts}".encode(), hashlib.sha256).hexdigest()
            if not hmac.compare_digest(want, sig):
                self._reply(401, "bad signature")
                return
        self._reply(200, payload)

    def _reply(self, code, msg):
        # Print the accepted payload so the test can assert on it.
        print(f"{code} {msg}", flush=True)
        self.send_response(code)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"OK")

    def log_message(self, *a):
        pass


HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
