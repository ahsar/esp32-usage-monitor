#!/usr/bin/env bash
# Read or change the ESP32 usage monitor's settings, over the status endpoint.
#
#   tools/status-settings.sh                                   # show the overrides
#   tools/status-settings.sh chime=0 brightness=4              # set two of them
#   tools/status-settings.sh place=Shanghai lat=31.2304 lon=121.4737
#   tools/status-settings.sh place=New+York                    # '+' is a space
#   tools/status-settings.sh place= lat= lon=                   # clear, back to config.json
#
# These are the knobs that would otherwise mean editing data/config.json (and
# running `uploadfs`, which erases the device's rotated OAuth token — see
# tools/uploadfs_guard.py). The device stores them in /settings.json, layered on
# top of config.json, so clearing one restores the configured value.
#
# Field reference is in the README ("Tuning it over HTTP"); the wire protocol is
# in docs/pc-bridge.md. Values are URL-decoded by the device, so write a space as
# `+` or `%20` — whatever you type here is exactly what the signature covers, so
# do not paste a pre-encoded query and expect different bytes.
#
# The shared key comes from data/config.json (`status_key`) or ESP32_MONITOR_KEY;
# the address from ESP32_MONITOR_URL (default: the mDNS name). Settings are
# refused by the device when it has no `status_key` at all.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: tools/status-settings.sh [key=value ...]

  (no arguments)   read the current overrides
  key=value ...    set them; an empty value clears one

  keys: chime=0|1            brightness=1..4
        place=CITY           lat=LAT lon=LON
        tz_offset_min=MIN    waqi_station=@ID
        air_label=LABEL
EOF
  exit 2
}

case "${1:-}" in
  -h | --help) usage ;;
esac

here="$(cd "$(dirname "$0")" && pwd)"
url="${ESP32_MONITOR_URL:-http://usage-monitor.local}"

key="${ESP32_MONITOR_KEY:-}"
if [ -z "$key" ] && [ -f "$here/../data/config.json" ]; then
  key="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("status_key",""))' \
          "$here/../data/config.json" 2>/dev/null || true)"
fi
if [ -z "$key" ]; then
  echo "no status_key (data/config.json or ESP32_MONITOR_KEY) — settings require auth" >&2
  exit 2
fi

if [ "$#" -eq 0 ]; then
  query="settings"
else
  query="$(printf '%s&' "$@")"
  query="${query%&}"
fi

# The signature covers the raw query string, exactly as it goes on the wire, so
# every value in the request is bound to the key (a captured request cannot be
# edited into a different one inside the timestamp window).
ts="$(date +%s)"
sig="$(printf '%s' "$query|$ts" | openssl dgst -sha256 -hmac "$key" -r | awk '{print $1}')"

# No -f: an HTTP error still exits 0, so the status code can be reported as a
# device answer rather than as "unreachable".
reply="$(curl -sS --max-time 5 -w $'\n%{http_code}' \
              -H "X-Ts: $ts" -H "X-Sig: $sig" "$url/?$query")" || {
  echo "failed to reach $url (is the device online? try ESP32_MONITOR_URL=http://<ip>)" >&2
  exit 1
}
code="${reply##*$'\n'}"
body="${reply%$'\n'*}"
if [ "$code" != "200" ]; then
  echo "device returned $code: $body" >&2
  exit 1
fi
printf '%s\n' "$body"
