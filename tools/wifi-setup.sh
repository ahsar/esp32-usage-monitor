#!/usr/bin/env bash
# Give the ESP32 usage monitor its WiFi credentials from a real keyboard.
#
#   tools/wifi-setup.sh Home 'hunter2'      # join that network (and keep it)
#   tools/wifi-setup.sh Home -              # …prompt for the passphrase instead
#   tools/wifi-setup.sh --forget            # drop them, back to the setup screen
#
# Why this exists: the device's own setup screen is a 4×32 character grid on a
# 184×224 panel, so a WPA2 passphrase is 60+ taps with a group switch every few
# characters. This sends the same credentials over the device channel instead
# (docs/pc-bridge.md), where they are typed on a PC keyboard.
#
# Transport: HTTP when the device is already reachable, BLE when it is not — and
# "not" is the normal case, because a device with no credentials has no IP
# address. BLE needs no pairing and no network: the request carries the same
# HMAC the HTTP path verifies, and the device stores the passphrase only once the
# association actually succeeds (so a typo cannot replace a working network).
#
# The passphrase is a secret and it *does* travel in the clear over BLE: the HMAC
# makes the write unforgeable, not private (see docs/pc-bridge.md). A `-`
# passphrase argument keeps it out of the local process list, but nothing can
# hide it from the radio.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: tools/wifi-setup.sh <ssid> <passphrase|->
       tools/wifi-setup.sh --forget

  <passphrase>   '-' prompts for it (keeps it out of the process list)
  --forget       clear the stored credentials; the device comes back up in the
                 setup screen (the only way back from a network that is gone)

The shared key comes from data/config.json (`status_key`) or ESP32_MONITOR_KEY;
the HTTP address from ESP32_MONITOR_URL (default: the mDNS name).
EOF
  exit 2
}

case "${1:-}" in
  -h | --help) usage ;;
  --forget)    [ "$#" -eq 1 ] || usage ;;
  "")          usage ;;
esac
if [ "${1:-}" != "--forget" ] && [ "$#" -ne 2 ]; then usage; fi

here="$(cd "$(dirname "$0")" && pwd)"

key="${ESP32_MONITOR_KEY:-}"
if [ -z "$key" ] && [ -f "$here/../data/config.json" ]; then
  key="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("status_key",""))' \
          "$here/../data/config.json" 2>/dev/null || true)"
fi
if [ -z "$key" ]; then
  echo "no status_key (data/config.json or ESP32_MONITOR_KEY) — the device refuses" >&2
  echo "unauthenticated credentials: anyone in radio range could point it at their own AP" >&2
  exit 2
fi

if [ "$1" = "--forget" ]; then
  ssid=""
  pass=""
  done_msg="the credentials are gone: the panel is back on the setup screen"
else
  ssid="$1"
  pass="$2"
  done_msg="the panel shows Connecting… then Connected (or the network list again)"
  if [ "$pass" = "-" ]; then
    printf 'passphrase for "%s" (not echoed): ' "$ssid" >&2
    IFS= read -rs pass
    printf '\n' >&2
  fi
fi

# Percent-encoded exactly as src/util/url_util.h encodes (unreserved = ALPHA /
# DIGIT / -._~): the signature covers the bytes that go on the wire, so the
# device must decode back the same string we hashed.
enc() {
  python3 -c 'import sys,urllib.parse;sys.stdout.write(urllib.parse.quote(sys.argv[1],safe="-._~"))' "$1"
}
query="wifi_ssid=$(enc "$ssid")&wifi_pass=$(enc "$pass")"

# HTTP first: it is the cheaper path and the one that works whenever the device
# already has an address (a re-provision, or a move to another AP).
url="${ESP32_MONITOR_URL:-http://usage-monitor.local}"
ts="$(date +%s)"
sig="$(printf '%s' "$query|$ts" | openssl dgst -sha256 -hmac "$key" -r | awk '{print $1}')"
reply="$(curl -sS --connect-timeout 2 --max-time 4 -w $'\n%{http_code}' \
              -H "X-Ts: $ts" -H "X-Sig: $sig" "$url/?$query" 2>/dev/null)" || reply=""

if [ -n "$reply" ]; then
  code="${reply##*$'\n'}"
  body="${reply%$'\n'*}"
  printf '%s %s\n' "$code" "$body"
  [ "$code" = "200" ] || exit 1
  echo "sent over HTTP to $url — $done_msg"
  exit 0
fi

# No IP path (the normal case: the device has no credentials yet). BLE is the
# transport that exists before association, and the reason this is not just
# another settings key.
if ! python3 -c 'import bleak' 2>/dev/null; then
  echo "device not reachable over HTTP, and the BLE fallback needs bleak:" >&2
  echo '  python3 -m pip install "bleak>=0.22"' >&2
  exit 1
fi

echo "no HTTP path to $url; sending over BLE (works with no network at all)…" >&2
python3 "$here/ble-bridge.py" --once "$query" --key "$key"
echo
echo "$done_msg."
if [ "$1" != "--forget" ]; then
  echo "nothing is stored if the passphrase is wrong."
  echo "re-check the address once it has associated:"
  echo "  tools/ble-bridge.py --scan --info"
fi
