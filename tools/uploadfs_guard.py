#!/usr/bin/env python3
"""Refuse to `uploadfs` a config token the device has already rotated away.

`pio run -t uploadfs` erases LittleFS, `/tokens.json` included, so the rebooted
device falls back to the `refresh_token` in `data/config.json`. That copy is only
still usable if the device has *not* booted since it was written: once it has,
the token was rotated away, reusing the stale copy gets `invalid_grant`, and the
whole OAuth family is revoked (see `apiRefreshToken()` in `src/api_client.cpp`).
The only way back is a full `tools/authorize.py` round trip — so this refuses the
flash instead of letting it happen.

Wired in from `platformio.ini`, next to the fonts hook:

    pre   block unless the digest of the token in `data/config.json` differs from
          `pushed_token_sha` in `.uploadfs-state.json`
    post  remember the token that was just pushed — the device claims it on the
          next boot, which is exactly why a *second* `uploadfs` is the fatal one

Run it bare to ask the same question by hand (exit code 1 = it would block):

    python3 tools/uploadfs_guard.py

Deliberate re-flash of an already-pushed config (restored a backup, re-authorised
on another machine, …):

    ESP32_ALLOW_STALE_TOKEN=1 pio run -e esp32-usage-monitor -t uploadfs
"""
import argparse
import hashlib
import json
import os
import pathlib
import sys
import time

CONFIG_NAME = pathlib.Path("data/config.json")
# Host-side only, and deliberately *not* under data/: everything in data/ is
# packed into the LittleFS image and shipped to the device.
STATE_NAME = pathlib.Path(".uploadfs-state.json")
BYPASS_ENV = "ESP32_ALLOW_STALE_TOKEN"
EXAMPLE = "data/config.json.example"


def default_root():
    """Project root. SCons `exec`s this file, where `__file__` does not exist."""
    file = globals().get("__file__")
    return pathlib.Path(file).resolve().parent.parent if file else pathlib.Path.cwd()


def fingerprint(token):
    """Digest of a refresh token, so the state file never holds the token itself."""
    return hashlib.sha256(token.encode()).hexdigest()[:16]


def read_json(path):
    """Parsed JSON object at `path`, or None when unreadable or not an object."""
    try:
        data = json.loads(pathlib.Path(path).read_text())
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def verdict(config=None, state=None, env=os.environ):
    """Decide whether flashing `config` is safe. Pure: reads two files, writes none.

    Returns `(ok, lines)`; `lines` is what the hook prints either way.
    """
    root = default_root()
    path = pathlib.Path(config) if config else root / CONFIG_NAME
    state = pathlib.Path(state) if state else root / STATE_NAME

    if env.get(BYPASS_ENV):
        return True, [f"uploadfs guard: bypassed ({BYPASS_ENV} is set)"]

    cfg = read_json(path)
    if cfg is None:
        return False, [
            f"uploadfs guard: {path} is missing or not valid JSON",
            f"  cp {EXAMPLE} {path} && python3 tools/authorize.py",
        ]

    token = cfg.get("refresh_token") or ""
    if not token:
        return False, [
            f"uploadfs guard: {path} has no refresh_token",
            "  python3 tools/authorize.py   # then re-run this command",
        ]

    pushed = (read_json(state) or {}).get("pushed_token_sha")
    if pushed and pushed == fingerprint(token):
        return False, [
            "uploadfs guard: refusing to flash — the device has already seen this token.",
            "",
            "  This config was pushed before, so the device has since rotated its",
            "  refresh token into /tokens.json. uploadfs erases that file, the device",
            "  falls back to this stale copy, hits invalid_grant, and the whole token",
            "  family is revoked — the exact state that needs a fresh authorisation.",
            "",
            "  Fix it first, then re-run this command:",
            "    python3 tools/authorize.py",
            "",
            "  To flash anyway (token re-authorised elsewhere, backup restored, …):",
            f"    {BYPASS_ENV}=1 pio run -e esp32-usage-monitor -t uploadfs",
        ]

    return True, [
        "uploadfs guard: this token has not been pushed yet, flashing is safe.",
        "  (the device owns it from the next boot on: re-run tools/authorize.py",
        "   before the next uploadfs — this guard will ask for it)",
    ]


def record_pushed(config=None, state=None):
    """Remember the token just pushed, so the next uploadfs can spot the stale copy."""
    root = default_root()
    config = pathlib.Path(config) if config else root / CONFIG_NAME
    state = pathlib.Path(state) if state else root / STATE_NAME

    token = (read_json(config) or {}).get("refresh_token") or ""
    if not token:
        return
    try:
        state.write_text(json.dumps(
            {"pushed_token_sha": fingerprint(token), "pushed_at": int(time.time())},
            indent=2) + "\n")
    except OSError as err:  # never fail the upload over bookkeeping
        print(f"uploadfs guard: could not write {state}: {err}")
        return
    print(f"uploadfs guard: recorded {state.name} — re-authorise before the next uploadfs")


def _register(env):
    project = pathlib.Path(env.subst("$PROJECT_DIR"))
    config, state = project / CONFIG_NAME, project / STATE_NAME

    def before_uploadfs(source, target, env):
        ok, lines = verdict(config, state)
        print("\n".join(lines))
        if not ok:
            env.Exit(1)

    def after_uploadfs(source, target, env):
        record_pushed(config, state)

    env.AddPreAction("uploadfs", before_uploadfs)
    env.AddPostAction("uploadfs", after_uploadfs)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", default=None, help="config to check (default: data/config.json)")
    ap.add_argument("--state", default=None, help="guard state file (default: .uploadfs-state.json)")
    args = ap.parse_args(argv)

    ok, lines = verdict(args.config, args.state)
    print("\n".join(lines))
    return 0 if ok else 1


try:
    # Injected by SCons; absent when this file is run as a plain script.
    Import("env")  # noqa: F821
except NameError:
    env = None

if env is not None:
    _register(env)
elif __name__ == "__main__":
    sys.exit(main())
