#!/usr/bin/env bash
# Push firmware to the ESP32 usage monitor over WiFi (ArduinoOTA / espota).
#
#   tools/ota.sh [ip-or-host]        # default host: usage-monitor.local
#   tools/ota.sh --gen-password      # write a fresh strong ota_password
#
# The password is read from data/config.json (`ota_password`) and handed to
# espota through OTA_AUTH; it is never printed and never committed. The device
# refuses to serve OTA at all without one (see src/ota.h), so an empty or short
# password here is an error, not a request for "no auth".
#
# The transfer is slow and the link is the limiting factor (espota acks every
# 1 KB chunk), so this retries: a failed attempt leaves the running firmware
# untouched and simply needs another go.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cfg="$here/../data/config.json"
min_chars=12
attempts=3

gen_password() {
  [ -f "$cfg" ] || { echo "missing $cfg" >&2; exit 1; }
  python3 - "$cfg" <<'PY'
import json, pathlib, secrets, sys
p = pathlib.Path(sys.argv[1])
cfg = json.loads(p.read_text())
cfg["ota_password"] = secrets.token_urlsafe(24)   # 32 chars, ~144 bits
p.write_text(json.dumps(cfg, indent=2, ensure_ascii=False) + "\n")
print(f"wrote a new {len(cfg['ota_password'])}-character ota_password to {p}")
PY
  cat <<EOF

Now get it onto the device (LittleFS — this ERASES the device's rotated tokens,
so authorise first):

  python3 tools/authorize.py
  pio run -e esp32-usage-monitor -t upload -t uploadfs
EOF
}

case "${1:-}" in
  --gen-password)
    gen_password
    exit 0
    ;;
  -h | --help)
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
esac

host="${1:-usage-monitor.local}"

password="${OTA_AUTH:-}"
if [ -z "$password" ] && [ -f "$cfg" ]; then
  password="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("ota_password",""))' \
              "$cfg" 2>/dev/null || true)"
fi

if [ "${#password}" -lt "$min_chars" ]; then
  echo "ota_password is missing or shorter than $min_chars characters — the" >&2
  echo "device would refuse the update. Run: $0 --gen-password" >&2
  exit 2
fi

echo "pushing to $host (password: ${#password} chars)"
for i in $(seq 1 "$attempts"); do
  echo "--- attempt $i/$attempts"
  # espota takes the password as a command-line argument (PlatformIO's espota
  # integration has no other channel), so it is briefly visible in `ps`.
  if OTA_AUTH="$password" pio run -e esp32-ota -t upload --upload-port "$host"; then
    echo "ok — the device reboots into the new slot by itself"
    exit 0
  fi
  echo "attempt $i failed (link loss); the running firmware is unchanged" >&2
done

echo "all $attempts attempts failed — check the link (ping $host), or use USB" >&2
exit 1
