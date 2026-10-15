# Outbound TLS and the console API

**Outbound TLS.** `http_client.cpp` verifies the server chain against the ESP-IDF
CA bundle (`setCACertBundle` + embedded `x509_crt_bundle`), so an active MitM on
the LAN cannot impersonate `opencode.ai` with a self-signed cert and lift the
OAuth tokens. It is the *public* root store, so it also fails closed against a
corporate interception proxy. Costs ~70 KB of flash. There is no `setInsecure()`
left — do not reintroduce it.

Because chain verification also checks the certificate's validity dates the
system clock must be plausible **before the first fetch**. `setup()` calls
`clockSyncRestoreFromRtc()` (the PCF85063 holds the last NTP-synced time, minus
the UTC+8 offset the RTC is written with), and the poll schedule is gated on
`clockSyncValid()` so a blank RTC + blocked NTP shows a clear serial warning
instead of a burst of failed handshakes. If you change the timezone handling in
`clock_sync.cpp`, keep `TZ_OFFSET_SEC` consistent with what `hwRtcWrite()` stores.

**No expiry deadline, but a reflash trigger.** The IDF bundle stores *subject
name + public key* per root (not certificates — `esp_crt_bundle` uses
`mbedtls_pk_parse_public_key` + `mbedtls_pk_verify_ext`), so the anchors have no
validity dates and the device keeps working indefinitely while a host's chain
still terminates at a bundled name with an unchanged key. It breaks only when a
host moves to a CA older than the frozen bundle, or a CA rotates that root's key
— an event, not a date. `tools/check-hosts.sh` detects exactly that by reading
the bundle back out of the built library and checking each host's chain against
it; the bundle's format is `u16 count` then per entry
`u16 name_len, u16 key_len, name(DER), key(DER)`.

## Auth / API notes

- opencode moved the console from the old `/_server` RPC to a REST API under
  `/console/api/*`; the old endpoint now redirects to `/console/login`.
- Requests need `x-org-id: <org id>` (same value as `workspace_id`) plus either an
  `Authorization: Bearer <access token>` or the `__Host-console_session` cookie.
  The firmware uses the Bearer/device-flow path.
- `authorize.py` uses `client_id=opencode-cli`; the device refreshes with the
  `refresh_token` grant and persists rotated tokens to `/tokens.json`.
- Usage meters: `access.meters.{fiveHour,week,month}` with string
  `usedMicroCents` / `limitMicroCents`; percent = used/limit*100.
- Weather: `wttr.in/Beijing?m&format=...` (one-line, ~120 B) — **must** send a
  `curl/...` User-Agent, and HTTP/1.0 avoids chunked encoding.
- Moon: `api.met.no/weatherapi/sunrise/3.0/moon` (~580 B) → moonrise/moonset/phase.
- Air: `api.waqi.info/feed/@3303/?token=<waqi_token>` (@3303 = Beijing US Embassy).
  PM2.5 is sometimes not reported; the card falls back to PM10.

> **⚠️ Throughput note.** Large responses (e.g. wttr.in `format=j1`, ~39 KB) transfer
> at only ~1-3 KB/s on this device, which looks like sender-side pacing. Keep network
> payloads small and prefer the one-line endpoints. `handleHttpStatus()` runs from
> `loop()` every ~16 ms (the fetch task never calls it), and reads drain the TLS
> buffer in an inner loop (calling `available()` per chunk cripples throughput).
