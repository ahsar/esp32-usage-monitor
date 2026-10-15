#include "status_server.h"
#include "clock_sync.h"
#include <Arduino.h>
#include <WiFi.h>
#include <mbedtls/md.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

static WiFiServer s_server(80);

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------

static uint8_t s_key[80];
static size_t  s_keyLen = 0;

void statusServerSetKey(const char* key) {
  s_keyLen = 0;
  if (!key) return;
  size_t n = strlen(key);
  if (n > sizeof(s_key)) n = sizeof(s_key);
  memcpy(s_key, key, n);
  s_keyLen = n;
}

bool statusServerAuthEnabled() { return s_keyLen > 0; }

static void hmacSha256(const uint8_t* key, size_t keyLen,
                       const char* msg, size_t msgLen, uint8_t out[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (!info || mbedtls_md_setup(&ctx, info, 1) != 0) {
    mbedtls_md_free(&ctx);
    memset(out, 0, 32);
    return;
  }
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg, msgLen);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool hexToBytes(const char* hex, uint8_t* out, size_t n) {
  if (strlen(hex) != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    int hi = hexVal(hex[2 * i]), lo = hexVal(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// Constant-time compare so a wrong signature can't be brute-forced by timing.
static bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0;
}

// Case-insensitive header lookup over the raw request head. Headers are
// newline-separated in `req` (each line keeps its trailing '\r').
static bool headerValue(const String& req, const char* name, char* out, size_t outLen) {
  char needle[40];
  snprintf(needle, sizeof(needle), "\n%s:", name);
  const size_t nlen = strlen(needle);
  const size_t rlen = req.length();
  if (rlen < nlen) return false;

  for (size_t i = 0; i + nlen <= rlen; i++) {
    size_t j = 0;
    for (; j < nlen; j++) {
      if (tolower((unsigned char)req[i + j]) != tolower((unsigned char)needle[j])) break;
    }
    if (j != nlen) continue;

    size_t v = i + nlen;
    while (v < rlen && (req[v] == ' ' || req[v] == '\t')) v++;
    size_t e = v;
    while (e < rlen && req[e] != '\r' && req[e] != '\n') e++;
    size_t len = e - v;
    if (len >= outLen) len = outLen - 1;
    memcpy(out, req.c_str() + v, len);
    out[len] = 0;
    return true;
  }
  return false;
}

// True when the request is allowed to change state. `payload` is what gets
// signed: the state name for `?state=`, the raw query string for settings.
static bool authorize(const String& req, const char* payload) {
  if (!statusServerAuthEnabled()) return true;   // dev mode, warned at boot

  char ts[24], sig[80];
  if (!headerValue(req, "X-Ts", ts, sizeof(ts))) return false;
  if (!headerValue(req, "X-Sig", sig, sizeof(sig))) return false;
  if (ts[0] == 0) return false;

  // Sized for a full settings query: the longest place/station/air_label plus the
  // keys and separators, which is what the client hashes too.
  char msg[256];
  int n = snprintf(msg, sizeof(msg), "%s|%s", payload, ts);
  if (n <= 0 || (size_t)n >= sizeof(msg)) return false;   // too long to sign

  uint8_t mac[32], got[32];
  hmacSha256(s_key, s_keyLen, msg, strlen(msg), mac);
  if (!hexToBytes(sig, got, 32)) return false;
  if (!ctEqual(mac, got, 32)) return false;

  // Freshness is best-effort: enforced once NTP has set the clock. Before that
  // a captured request could in principle be replayed, but it still needs the
  // key to have been forged in the first place.
  if (clockSyncValid()) {
    long long t   = atoll(ts);
    long long now = (long long)time(nullptr);
    if (t < now - 60 || t > now + 60) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum St : uint8_t { ST_IDLE, ST_RUNNING, ST_WAITING };
static St s_state = ST_IDLE;
static uint32_t s_since = 0;
static StatusEvent s_pending = STATUS_EV_NONE;
static void (*s_wakeCb)() = nullptr;
static DeviceSettings s_settings;
static bool (*s_settingsCb)(const DeviceSettings&, uint16_t) = nullptr;
static uint32_t s_lastRejectLog = 0;

void statusServerBegin() { s_server.begin(); }
void statusServerSetWakeCallback(void (*cb)()) { s_wakeCb = cb; }
void statusServerSetSettings(const DeviceSettings& s) { s_settings = s; }
void statusServerSetSettingsCallback(bool (*cb)(const DeviceSettings&, uint16_t)) {
  s_settingsCb = cb;
}

bool statusServerThinking() {
  return s_state == ST_RUNNING || s_state == ST_WAITING;
}
bool     statusServerWaiting()      { return s_state == ST_WAITING; }
uint32_t statusServerThinkingSince() { return s_since; }

void statusServerClearThinking() {
  s_state = ST_IDLE;
}

StatusEvent statusServerTakeEvent() {
  StatusEvent e = s_pending;
  s_pending = STATUS_EV_NONE;
  return e;
}

static void enterActive(St next) {
  if (s_state == next) return;
  s_state = next;
  s_since = millis();
  if (s_wakeCb) s_wakeCb();
}

static void respond(WiFiClient& client, const char* status, const char* body = "OK") {
  client.print("HTTP/1.1 ");
  client.print(status);
  client.print("\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: ");
  client.print((unsigned)strlen(body));
  client.print("\r\n\r\n");
  client.print(body);
  delay(10);
  client.stop();
}

// The query of the request line, verbatim and undecoded (no leading '?'), e.g.
// "chime=0&place=Shanghai". This exact string is what the settings signature
// covers, so it must be taken as bytes off the wire.
static bool requestQuery(const String& req, char* out, size_t cap) {
  int g = req.indexOf("GET ");
  if (g < 0) return false;
  int sp = req.indexOf(' ', g + 4);          // end of the request target
  if (sp < 0) return false;
  int q = req.indexOf('?', g + 4);
  if (q < 0 || q >= sp) return false;
  size_t n = (size_t)(sp - q - 1);
  if (n == 0 || n + 1 > cap) return false;
  memcpy(out, req.c_str() + q + 1, n);
  out[n] = '\0';
  return true;
}

// A settings request: everything except a `?state=` (which keeps the original
// value-only signature so the OpenCode plugin is untouched). `?settings` on its
// own reads the current overrides back.
static void handleSettings(WiFiClient& client, const String& req, const char* query) {
  if (!statusServerAuthEnabled()) {
    Serial.println("settings: REFUSED (no status_key configured)");
    respond(client, "403 Forbidden", "settings require status_key\n");
    return;
  }

  // Auth before parsing: an unauthenticated request gets no feedback about the
  // parser at all, and the signature covers the raw query either way.
  if (!authorize(req, query)) {
    if (millis() - s_lastRejectLog > 5000) {
      s_lastRejectLog = millis();
      Serial.printf("settings: REJECTED from %s (bad/missing signature)\n",
                    client.remoteIP().toString().c_str());
    }
    respond(client, "401 Unauthorized");
    return;
  }

  bool readOnly = !strcmp(query, "settings");
  DeviceSettings next = s_settings;
  uint16_t changed = SET_NONE;
  char err[96] = {0};

  if (!readOnly && !settingsFromQuery(query, next, changed, err, sizeof(err)))
    return respond(client, "400 Bad Request", err[0] ? err : "bad settings query");

  if (readOnly || !s_settingsCb) {
    char body[512];
    settingsDescribe(s_settings, body, sizeof(body));
    respond(client, "200 OK", body);
    return;
  }

  if (!s_settingsCb(next, changed)) {
    Serial.printf("settings: apply failed for query '%s'\n", query);
    respond(client, "500 Internal Server Error", "could not store settings\n");
    return;
  }

  s_settings = next;
  Serial.printf("settings: %s\n", query);
  char body[512];
  settingsDescribe(s_settings, body, sizeof(body));
  respond(client, "200 OK", body);
}

extern "C" void handleHttpStatus() {
  WiFiClient client = s_server.accept();
  if (!client) return;

  client.setTimeout(50);

  // Read the request head (request line + headers). Each line is appended with
  // a '\n' so header lookup can scan for "\nName:" regardless of line endings.
  String req;
  for (int i = 0; i < 24; i++) {
    uint32_t t0 = millis();
    while (!client.available() && (millis() - t0) < 60) delay(1);
    if (!client.available()) break;
    String line = client.readStringUntil('\n');
    req += line;
    req += '\n';
    if (line == "\r" || line.length() == 0) break;
  }

  if (req.indexOf("GET ") < 0) {
    respond(client, "405 Method Not Allowed");
    return;
  }

  // Settings first: a `?state=` request keeps the original protocol, anything
  // else that is a query is the settings channel.
  char query[256];
  bool hasQuery = requestQuery(req, query, sizeof(query));
  if (hasQuery && strncmp(query, "state=", 6) != 0) {
    handleSettings(client, req, query);
    return;
  }

  const char* st = nullptr;
  if (req.indexOf("state=running") >= 0)       st = "running";
  else if (req.indexOf("state=waiting") >= 0)  st = "waiting";
  else if (req.indexOf("state=done") >= 0)     st = "done";
  else if (req.indexOf("state=failed") >= 0)   st = "failed";

  if (!st) {
    respond(client, "200 OK");   // health probe / unknown: nothing to do
    return;
  }

  if (!authorize(req, st)) {
    if (millis() - s_lastRejectLog > 5000) {
      s_lastRejectLog = millis();
      Serial.printf("status: REJECTED state=%s from %s (bad/missing signature)\n",
                    st, client.remoteIP().toString().c_str());
    }
    respond(client, "401 Unauthorized");
    return;
  }

  if (strcmp(st, "running") == 0) {
    enterActive(ST_RUNNING);
  } else if (strcmp(st, "waiting") == 0) {
    if (s_state != ST_WAITING) s_pending = STATUS_EV_WAITING;
    enterActive(ST_WAITING);
  } else {
    bool failed = strcmp(st, "failed") == 0;
    // Only chime for a real transition; a duplicate/stale report stays silent.
    if (s_state != ST_IDLE) s_pending = failed ? STATUS_EV_FAILED : STATUS_EV_DONE;
    s_state = ST_IDLE;
  }
  Serial.printf("state=%s\n", st);

  respond(client, "200 OK");
}
