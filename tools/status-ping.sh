#!/usr/bin/env bash
# Ping the ESP32 usage monitor's status server by hand, without OpenCode.
#
#   tools/status-ping.sh running|waiting|done|failed
#
# The device is an HTTP server on port 80; this is exactly the request the
# OpenCode plugin (tools/opencode-plugin) makes on each agent transition.
#
# When the device has a `status_key` in data/config.json, the request is signed
# with HMAC-SHA256 over "<state>|<unixSeconds>" (X-Ts / X-Sig). The key is read
# from data/config.json, or from ESP32_MONITOR_KEY if set.
# The address comes from ESP32_MONITOR_URL (default: the mDNS name).
set -euo pipefail

state="${1:-}"
case "$state" in
  running | waiting | done | failed) ;;
  *)
    echo "usage: $0 running|waiting|done|failed" >&2
    exit 2
    ;;
esac

here="$(cd "$(dirname "$0")" && pwd)"
url="${ESP32_MONITOR_URL:-http://usage-monitor.local}"

key="${ESP32_MONITOR_KEY:-}"
if [ -z "$key" ] && [ -f "$here/../data/config.json" ]; then
  key="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("status_key",""))' \
          "$here/../data/config.json" 2>/dev/null || true)"
fi

args=()
if [ -n "$key" ]; then
  ts="$(date +%s)"
  sig="$(printf '%s' "$state|$ts" | openssl dgst -sha256 -hmac "$key" -r | awk '{print $1}')"
  args+=(-H "X-Ts: $ts" -H "X-Sig: $sig")
  auth="signed"
else
  auth="unsigned (no status_key)"
fi

if curl -fsS --max-time 5 ${args[@]+"${args[@]}"} "$url/?state=$state" >/dev/null; then
  echo "sent state=$state to $url [$auth]"
else
  echo "failed to reach $url (is the device online? try ESP32_MONITOR_URL=http://<ip>)" >&2
  exit 1
fi
