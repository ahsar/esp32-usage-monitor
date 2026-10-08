#!/usr/bin/env python3
"""Authorise the ESP32 usage monitor against the opencode console.

Runs the OAuth device flow (same client id as the official opencode CLI) and
writes the resulting refresh token into data/config.json. After this, run
`pio run -e esp32-usage-monitor -t uploadfs` so the device picks it up.

Usage:
    python3 tools/authorize.py          # authorise (or re-authorise)
    python3 tools/authorize.py --check  # refresh + print current usage
"""
import argparse
import json
import pathlib
import sys
import time
import urllib.error
import urllib.request

BASE = "https://opencode.ai/console"
CLIENT_ID = "opencode-cli"
ROOT = pathlib.Path(__file__).resolve().parent.parent
CONFIG = ROOT / "data" / "config.json"

DEVICE_GRANT = "urn:ietf:params:oauth:grant-type:device_code"
REFRESH_GRANT = "refresh_token"


def request(method, path, *, payload=None, bearer=None, org_id=None):
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Accept": "application/json",
               "User-Agent": "esp32-usage-monitor-tools"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    if bearer:
        headers["Authorization"] = f"Bearer {bearer}"
    if org_id:
        headers["x-org-id"] = org_id
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        raw = e.read().decode()
        try:
            return e.code, json.loads(raw)
        except ValueError:
            return e.code, {"raw": raw}


def load_config():
    if not CONFIG.exists():
        sys.exit(f"missing {CONFIG}")
    return json.loads(CONFIG.read_text())


def save_config(cfg):
    CONFIG.write_text(json.dumps(cfg, indent=2, ensure_ascii=False) + "\n")
    print(f"updated {CONFIG}")


def device_flow():
    status, dev = request("POST", "/auth/device/code", payload={"client_id": CLIENT_ID})
    if status != 200:
        sys.exit(f"device/code failed: HTTP {status} {dev}")

    url = dev.get("verification_uri_complete") or dev.get("verification_uri")
    if url and url.startswith("/"):
        url = "https://opencode.ai" + url
    code = dev["user_code"]
    print("\n  1. 打开这个网址并登录：")
    print(f"     {url}")
    print(f"  2. 确认码：{code}\n")
    print("  等待授权中...")

    interval = max(1, int(dev.get("interval", 5)))
    deadline = time.time() + int(dev.get("expires_in", 600))
    while time.time() < deadline:
        time.sleep(interval)
        status, tok = request("POST", "/auth/device/token", payload={
            "grant_type": DEVICE_GRANT,
            "device_code": dev["device_code"],
            "client_id": CLIENT_ID,
        })
        if status == 200 and tok.get("access_token"):
            return tok
        err = tok.get("error")
        if err == "authorization_pending":
            continue
        if err == "slow_down":
            interval += 5
            continue
        if err in ("expired_token", "access_denied"):
            sys.exit(f"授权失败: {err}")
        # Anything else (including a transient 5xx) -> keep polling until deadline.
    sys.exit("授权超时，请重试")


def print_usage(access_token, org_id):
    status, data = request("GET", "/api/go/status", bearer=access_token, org_id=org_id)
    if status != 200:
        sys.exit(f"go/status failed: HTTP {status} {data}")
    meters = data.get("access", {}).get("meters", {})
    print("当前用量：")
    for key, label in (("fiveHour", "5 小时"), ("week", "本周"), ("month", "本月")):
        m = meters.get(key)
        if not m:
            continue
        used = int(m.get("usedMicroCents", 0))
        limit = int(m.get("limitMicroCents", 0))
        pct = (used * 100 // limit) if limit else 0
        print(f"  {label:<6} {pct:>3}%   (重置 {m.get('resetsAt', '?')})")


def main():
    ap = argparse.ArgumentParser(description="Authorise the ESP32 usage monitor")
    ap.add_argument("--check", action="store_true",
                    help="refresh the token and print current usage instead of authorising "
                         "(WARNING: rotates the refresh token - do not run while the device is live)")
    args = ap.parse_args()

    cfg = load_config()

    if args.check:
        if not cfg.get("refresh_token"):
            sys.exit("config.json has no refresh_token; run without --check first")
        org = cfg.get("org_id") or cfg.get("workspace_id")
        status, tok = request("POST", "/auth/device/token", payload={
            "grant_type": REFRESH_GRANT,
            "refresh_token": cfg["refresh_token"],
            "client_id": CLIENT_ID,
        })
        if status != 200 or not tok.get("access_token"):
            if isinstance(tok, dict) and tok.get("error") == "invalid_grant":
                sys.exit("refresh_token was rotated/revoked - re-run without --check to authorise again")
            sys.exit(f"refresh failed: HTTP {status} {tok}")
        cfg["refresh_token"] = tok.get("refresh_token", cfg["refresh_token"])
        if tok.get("org_id"):
            cfg["org_id"] = tok["org_id"]
        save_config(cfg)
        print_usage(tok["access_token"], cfg.get("org_id") or org)
        return

    tok = device_flow()
    cfg["refresh_token"] = tok["refresh_token"]
    if tok.get("org_id"):
        cfg["org_id"] = tok["org_id"]
    save_config(cfg)
    print("\n授权成功。现在运行：")
    print("  pio run -e esp32-usage-monitor -t uploadfs")
    print("（设备已在运行时，重传文件系统后重启一次即可。）")
    print("\n注意：之后不要再运行 --check，否则会轮换 token 让设备失效。")


if __name__ == "__main__":
    main()
