#!/usr/bin/env bash
# Deliver one status/settings request to the ESP32 usage monitor, over the
# transports that can carry it — in the order that matters.
#
# This is the client half of docs/pc-bridge.md, and the whole reason it exists
# as one command is that the device serves the *same* channel over HTTP
# (src/status_server.cpp) and BLE (src/ble_status.cpp) while the two do not
# always agree about being reachable: a VLAN boundary, a blocked mDNS, or an AP
# that associates and then carries nothing all break the LAN path without the
# device noticing — and none of them mean anything to BLE. So the ladder is
# tried in order and the first transport that reaches the device wins:
#
#   1. the URL that worked last        (cache: a dead mDNS is not paid twice)
#   2. the device IP learned over BLE  (cache: a DHCP move heals itself)
#   3. ESP32_MONITOR_URLS              (default: http://usage-monitor.local)
#   4. BLE, one shot                   tools/ble-bridge.py --once --info
#
# No daemon: every invocation brings the ladder up, uses it, and exits. The BLE
# rung is a full cold connect (~8-15 s on macOS) and is only ever paid when HTTP
# has already failed; a successful BLE send also remembers where the device was,
# so the next one can go back to HTTP.
#
#   tools/monitor-send.sh done                    # state=done (…|running|waiting|failed)
#   tools/monitor-send.sh 'chime=0&brightness=3'  # any settings query
#
# A bare `state=…` keeps the original value-only signature (the state name is
# what gets signed); *any other* query — settings, and the plugin's
# `agent=1&state=…&repo=…&tool=…&note=…` — is signed verbatim as a whole, which is
# what lets the device draw that text. See status_channel.cpp.
#
# Environment:
#   ESP32_MONITOR_KEY        HMAC secret (default: data/config.json `status_key`;
#                            empty on either side = auth disabled, unsigned send)
#   ESP32_MONITOR_URLS       comma-separated HTTP rungs, in order
#   ESP32_MONITOR_URL        single-URL back-compat for the above
#   ESP32_MONITOR_BLE        0 to drop the BLE rung
#   ESP32_MONITOR_DEBUG      1 to log each rung to stderr
#   ESP32_MONITOR_HTTP_TIMEOUT_S   per-HTTP-rung curl --max-time (default 2)
#
# Exit: 0 = the device was reached (any HTTP status: 401/403 is an auth answer,
# not a transport failure), 1 = nothing reached it, 2 = usage error.
#
# Every invocation also appends one line to
# ${XDG_CACHE_HOME:-~/.cache}/esp32-usage-monitor/send.log — the winning transport
# and what was tried before it, or "FAILED no transport". That file is always on:
# a fire-and-forget sender that fails quietly is indistinguishable from one that
# never ran, and the plugin's stderr is invisible from a TUI. ESP32_MONITOR_DEBUG
# only adds the per-rung narration on top of it. WiFi credentials are redacted in
# the log exactly as they are on the device's console.

set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/esp32-usage-monitor"
cache="$cache_dir/last-url"
ble_log="$cache_dir/ble-last.log"

debug() { [ -n "${ESP32_MONITOR_DEBUG:-}" ] && printf '[monitor-send] %s\n' "$*" >&2 || true; }

# A persistent one-liner per invocation, always on — not gated on
# ESP32_MONITOR_DEBUG. The whole point of the ladder is to fail quietly, and the
# plugin's stderr is invisible from a TUI, so without this a send that never
# arrived is indistinguishable from one that was never made. Rotated by
# truncation rather than appended forever: this is a diagnostic, not an archive.
log_file="$cache_dir/send.log"
LOG_MAX_BYTES=131072
log() {
  local size
  mkdir -p "$cache_dir" 2>/dev/null || true
  if [ -f "$log_file" ]; then
    size="$(wc -c <"$log_file" | tr -d ' ')"
    if [ "${size:-0}" -gt "$LOG_MAX_BYTES" ]; then : >"$log_file"; fi
  fi
  printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$*" >>"$log_file" 2>/dev/null || true
}

# The one query that carries a secret. Mirrors wifiQueryForLog() in
# src/parse/wifi_cred_parse.h: neither the serial console nor this file has any
# business holding a passphrase.
log_query() {
  case "$1" in
    wifi_*) printf 'wifi_ssid=<credentials, passphrase redacted>' ;;
    *) printf '%s' "$1" ;;
  esac
}

# ---- the query ------------------------------------------------------------

case "${1:-}" in
  -h | --help)
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  running | waiting | done | failed)
    query="state=$1"
    shift
    ;;
  '')
    echo "usage: $0 <state>|<query> …   (state: running|waiting|done|failed)" >&2
    exit 2
    ;;
  *)
    query="$1"
    shift
    ;;
esac

# Extra arguments are joined with '&', so `monitor-send.sh chime=0 brightness=3`
# reads like the settings tool. Everything is signed verbatim (see below).
for extra in "$@"; do query="$query&$extra"; done

# ---- signing --------------------------------------------------------------
# Mirrors status_server.cpp (via tools/status-ping.sh): a `state=` request is
# signed over the *value*; anything else (the settings channel) over the whole
# raw query string, so every field is bound to the key. The key itself never
# goes on the wire — only the timestamp and the signature do.

key="${ESP32_MONITOR_KEY:-}"
if [ -z "$key" ] && [ -f "$here/../data/config.json" ]; then
  key="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("status_key",""))' \
          "$here/../data/config.json" 2>/dev/null || true)"
fi

hdr=()
if [ -n "$key" ]; then
  case "$query" in
    state=*) payload="${query#state=}" ;;
    *) payload="$query" ;;
  esac
  ts="$(date +%s)"
  sig="$(printf '%s' "$payload|$ts" | openssl dgst -sha256 -hmac "$key" -r | awk '{print $1}')"
  hdr=(-H "X-Ts: $ts" -H "X-Sig: $sig")
  debug "auth: signed"
else
  debug "auth: unsigned (no status_key)"
fi

# ---- the ladder -----------------------------------------------------------

mkdir -p "$cache_dir" 2>/dev/null || true

# Deduplicated, ordered rungs. `seen` is newline-delimited because this must run
# on bash 3.2 (macOS /bin/bash), where an empty array under `set -u` is an error.
seen=$'\n'
urls=()
add() {
  [ -n "$1" ] || return 0
  case "$seen" in *$'\n'"$1"$'\n'*) return 0 ;; esac
  seen="$seen$1"$'\n'
  urls+=("$1")
}

# 1. Last URL that worked, then the IP learned over BLE (see the last rung): on a
#    network where mDNS is dead, this is what keeps every send from paying the
#    mDNS timeout again — and what lets a BLE fallback restore the HTTP path.
last_good="$(sed -n '1p' "$cache" 2>/dev/null || true)"
learned_ip="$(sed -n '2p' "$cache" 2>/dev/null || true)"
save_cache() { printf '%s\n%s\n' "$last_good" "$learned_ip" > "$cache" 2>/dev/null || true; }
add "$last_good"
add "$learned_ip"
# 2. Configured rungs (the mDNS name by default).
if [ -n "${ESP32_MONITOR_URLS:-}" ]; then
  old_ifs="$IFS"; IFS=','
  for u in $ESP32_MONITOR_URLS; do add "$u"; done
  IFS="$old_ifs"
else
  add "${ESP32_MONITOR_URL:-http://usage-monitor.local}"
fi

http_timeout="${ESP32_MONITOR_HTTP_TIMEOUT_S:-2}"
# What the log line shows for this request: the query, minus a passphrase.
shown="$(log_query "$query")"
notes=""
for u in ${urls[@]+"${urls[@]}"}; do
  # --max-time bounds the whole operation, name resolution included, which is
  # what keeps a blocked mDNS from stalling the ladder.
  code="$(curl -sS -m "$http_timeout" -o /dev/null -w '%{http_code}' \
            ${hdr[@]+"${hdr[@]}"} "$u/?$query" 2>/dev/null)" || code=""
  if [ -n "$code" ] && [ "$code" != "000" ]; then
    debug "http $u → $code"
    log "sent http $u → $code${notes:+ (tried ${notes})} :: $shown"
    last_good="$u"; save_cache
    exit 0
  fi
  notes="${notes:+$notes, }$u → no answer"
  debug "http $u → no answer"
done

# 3. BLE, one shot. ble-bridge signs for itself, from the same key source, and
#    `--info` makes it read back the device's own IP on the same link.
if [ "${ESP32_MONITOR_BLE:-1}" != "0" ]; then
  debug "ble → one-shot connect…"
  if ESP32_MONITOR_KEY="$key" python3 "$here/ble-bridge.py" --once "$query" --info \
       >"$ble_log" 2>&1; then
    # Learn where the device is, so the next send can go back to HTTP.
    ip="$(sed -n 's/.*[^0-9]ip=\([0-9][0-9.]*\).*/\1/p' "$ble_log" | head -n 1)"
    if [ -n "$ip" ] && [ "http://$ip" != "$learned_ip" ]; then
      learned_ip="http://$ip"; save_cache
      debug "ble → ok (learned $learned_ip)"
    else
      debug "ble → ok"
    fi
    log "sent ble${notes:+ (tried ${notes})} :: $shown"
    exit 0
  fi
  debug "ble → $(tail -n 1 "$ble_log" 2>/dev/null || echo failed)"
  notes="${notes:+$notes, }ble → $(tail -n 1 "$ble_log" 2>/dev/null || echo failed)"
fi

debug "no transport reached the device"
log "FAILED no transport${notes:+ ($notes)} :: $shown"
exit 1
