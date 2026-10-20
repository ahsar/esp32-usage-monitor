#!/usr/bin/env bash
# Push a notice to the device's Notice page — a short line that stays on the
# panel until its TTL runs out (src/parse/notice_parse.h).
#
#   tools/status-notice.sh "Standup in 8 min" --kind meeting --left 480 --ttl 900
#   tools/status-notice.sh "deploy 1.4.2 is live" --ttl 600
#   tools/status-notice.sh ""            # clear it now
#
# `--left SECONDS` is a countdown to the event itself, which the device ticks
# locally: push once when the calendar entry is a day out and the panel counts
# down without another send. `--ttl` is how long the notice may live (cap 24 h),
# and it is mandatory on the wire — an unexpiring notice is a lie waiting to
# happen.
#
# Delivery walks the same ladder as everything else (tools/monitor-send.sh: last
# good HTTP → learned IP → configured URLs → BLE one-shot), and the request is
# signed over the whole query, so the text is bound to the key.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"

text=""
kind="note"
ttl=3600
left=""

while [ $# -gt 0 ]; do
  case "$1" in
    -h | --help)
      sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    --kind)
      kind="${2:-}"
      shift 2 || { echo "--kind needs a value" >&2; exit 2; }
      ;;
    --ttl)
      ttl="${2:-}"
      shift 2 || { echo "--ttl needs a value" >&2; exit 2; }
      ;;
    --left)
      left="${2:-}"
      shift 2 || { echo "--left needs a value" >&2; exit 2; }
      ;;
    --*)
      echo "unknown option: $1" >&2
      exit 2
      ;;
    *)
      text="$1"
      shift
      ;;
  esac
done

# The empty form clears the panel now: `?notice=1&clear=1`, a signed request of
# its own rather than an empty text (which the device rejects).
if [ -z "$text" ]; then
  query="notice=1&clear=1"
else
  enc="$(python3 -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1], safe=""))' "$text")"
  query="notice=1&kind=${kind}&text=${enc}&ttl=${ttl}"
  [ -n "$left" ] && query="$query&left=${left}"
fi

exec "$here/monitor-send.sh" "$query"
