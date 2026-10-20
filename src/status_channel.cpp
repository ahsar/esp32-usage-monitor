// The device channel's one implementation: HMAC auth, the agent state machine,
// settings validation/dispatch and WiFi credentials. status_server.cpp (HTTP)
// and ble_status.cpp (BLE) are transports; everything about *what a request
// means* is here, so the two cannot drift apart.
//
// Threading: the channel is configured in setup() and pumped from loop(), which
// is also where the settings/wifi callbacks may write flash and where the state
// machine's wake callback touches UI state. The BT stack's own task only copies
// a frame (ble_status.cpp); it never calls into here.
#include "status_channel.h"
#include "clock_sync.h"
#include <Arduino.h>
#include <mbedtls/md.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------

static uint8_t s_key[80];
static size_t  s_keyLen = 0;

void statusChannelSetKey(const char* key) {
  s_keyLen = 0;
  if (!key) return;
  size_t n = strlen(key);
  if (n > sizeof(s_key)) n = sizeof(s_key);
  memcpy(s_key, key, n);
  s_keyLen = n;
}

bool statusChannelAuthEnabled() { return s_keyLen > 0; }

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

// True when `sig` is a valid HMAC over "<payload>|<ts>". `payload` is what gets
// signed: the state name for `?state=`, the raw query string for settings and
// WiFi credentials. The HMAC pair itself comes from the transport (the X-Ts/X-Sig
// headers over HTTP, the first two space-separated fields of a BLE frame) — that
// is the only part of auth that is not shared, so it is the only part that lives
// in a transport.
static bool authorizePayload(const char* payload, const char* ts, const char* sig) {
  if (!statusChannelAuthEnabled()) return true;   // dev mode, warned at boot
  if (!payload || !ts || !ts[0] || !sig || !sig[0]) return false;

  // Sized for the largest query either transport accepts plus '|' + a ≤20-digit
  // timestamp + NUL. The client hashes the same "<query>|<ts>" string, so both
  // sides build it the same way — and a *validly signed* request must never be
  // rejected over buffer sizing. (A WiFi request with a fully percent-encoded
  // 63-byte passphrase reaches ~317 bytes, which the old fixed 288 could not
  // hold: it failed auth with no explanation.)
  char msg[STATUS_QUERY_MAX + 1 + 20 + 1];
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
// State + configuration
// ---------------------------------------------------------------------------

static StatusMachine s_machine;
// Every live agent session (one per OpenCode window, keyed by the client's short
// session token). The machine above is the *aggregate* the screen shows; the
// table is what makes two windows honest (parse/agent_sessions.h).
static AgentTable    s_agents;
// Context of the session that owns the screen, published for the animation.
static AgentInfo     s_agent;
// Key1 dismisses the screen without ending the sessions. A heartbeat or a
// context refresh must not bring it back, so remember which generation was
// dismissed; the next real state change (a new question) re-opens it.
static bool          s_dismissed = false;
static uint32_t      s_dismissedGen = 0;
static NoticeState   s_notice;
static void (*s_wakeCb)() = nullptr;
static DeviceSettings s_settings;
static bool (*s_settingsCb)(const DeviceSettings&, uint16_t) = nullptr;
static bool (*s_wifiCb)(const WifiCred&) = nullptr;
static uint32_t s_lastRejectLog = 0;

void statusChannelSetWakeCallback(void (*cb)()) { s_wakeCb = cb; }
void statusChannelSetSettings(const DeviceSettings& s) { s_settings = s; }
void statusChannelSetSettingsCallback(bool (*cb)(const DeviceSettings&, uint16_t)) {
  s_settingsCb = cb;
}
void statusChannelSetWifiCallback(bool (*cb)(const WifiCred&)) { s_wifiCb = cb; }

const AgentInfo& statusChannelAgentInfo() { return s_agent; }

// Publish the table's aggregate into the machine (which owns the chimes and the
// wake) and the owning context into s_agent. `allowCue` is false for the expiry
// path: a session that went silent is not a completion, and it must not wake the
// panel or chime.
static void syncAgent(uint32_t now, bool allowCue) {
  const AgentInfo* info = agentSessionsInfo(s_agents);
  if (info) s_agent = *info;
  else      agentInfoClear(s_agent);

  if (s_dismissed) {
    if (s_agents.generation != s_dismissedGen) s_dismissed = false;
    else return;   // still the screen the user dismissed
  }

  const char* st = agentSessionsStateName(s_agents);
  const char* ev = st ? st : (s_agents.lastEndedFailed ? "failed" : "done");

  bool changed = statusMachineApply(s_machine, ev, now);
  if (changed && allowCue && s_wakeCb) s_wakeCb();
  if (!allowCue) statusMachineTakeEvent(s_machine);   // drop any cue it queued
}

bool     statusChannelThinking()      { return statusMachineThinking(s_machine); }
bool     statusChannelWaiting()       { return statusMachineWaiting(s_machine); }
uint32_t statusChannelThinkingSince() { return agentSessionsSince(s_agents, millis()); }
int      statusChannelAgentCount()    { return agentSessionsActive(s_agents); }

void statusChannelClearThinking() {
  s_dismissed    = true;
  s_dismissedGen = s_agents.generation;
  statusMachineClear(s_machine);
}
StatusEvent statusChannelTakeEvent()  { return statusMachineTakeEvent(s_machine); }

const NoticeState& statusChannelNotice() { return s_notice; }

void statusChannelTick(uint32_t now) {
  // Expiry is silent on purpose: the host went away, which is neither a success
  // nor a failure, and waking the panel to say so would be wrong.
  if (agentSessionsExpire(s_agents, now, AGENT_SESSION_TTL_MS)) {
    // Keep a cue that a request handled earlier in this same loop iteration
    // already queued (a real `done` from HTTP, say): an expiry is silent, but it
    // must not swallow someone else's completion.
    StatusEvent keep = statusMachineTakeEvent(s_machine);
    statusMachineClear(s_machine);
    syncAgent(now, /*allowCue=*/false);
    statusMachineTakeEvent(s_machine);   // drop the cue our own transition queued
    if (keep != STATUS_EV_NONE) s_machine.pending = keep;
  }

  if (s_notice.valid && (int32_t)(now - s_notice.expiresMs) >= 0) {
    s_notice.valid = false;
    s_notice.rev++;
    Serial.println("notice: expired");
  }
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

static void setBody(char* body, size_t cap, const char* text) {
  if (!body || !cap) return;
  size_t n = strlen(text);
  if (n > cap - 1) n = cap - 1;
  memcpy(body, text, n);
  body[n] = '\0';
}

// A client with the wrong key would otherwise fill the console. One line every
// few seconds is enough to notice, and a BLE peer retrying on every write is no
// different from a host retrying on every request.
static void logReject(const char* what, const char* peer) {
  if (millis() - s_lastRejectLog < 5000) return;
  s_lastRejectLog = millis();
  Serial.printf("%s: REJECTED from %s (bad/missing signature)\n", what, peer);
}

// A settings request: everything except a `?state=` (which keeps the original
// value-only signature so the OpenCode plugin is untouched). `?settings` on its
// own reads the current overrides back.
static const char* channelSettings(const StatusReq& r, char* body, size_t cap) {
  if (!statusChannelAuthEnabled()) {
    Serial.println("settings: REFUSED (no status_key configured)");
    setBody(body, cap, "settings require status_key\n");
    return "403 Forbidden";
  }

  // Auth before parsing: an unauthenticated request gets no feedback about the
  // parser at all, and the signature covers the raw query either way.
  if (!authorizePayload(r.query, r.ts, r.sig)) {
    logReject("settings", r.peer);
    setBody(body, cap, "OK");   // the body a rejected request has always had
    return "401 Unauthorized";
  }

  bool readOnly = !strcmp(r.query, "settings");
  DeviceSettings next = s_settings;
  uint16_t changed = SET_NONE;
  char err[96] = {0};

  if (!readOnly && !settingsFromQuery(r.query, next, changed, err, sizeof(err))) {
    setBody(body, cap, err[0] ? err : "bad settings query");
    return "400 Bad Request";
  }

  if (readOnly || !s_settingsCb) {
    settingsDescribe(s_settings, body, cap);
    return "200 OK";
  }

  if (!s_settingsCb(next, changed)) {
    Serial.printf("settings: apply failed for query '%s'\n", r.query);
    setBody(body, cap, "could not store settings\n");
    return "500 Internal Server Error";
  }

  s_settings = next;
  Serial.printf("settings: %s\n", r.query);
  settingsDescribe(s_settings, body, cap);
  return "200 OK";
}

// An agent-context request: the state ping plus what the agent is doing. Same
// policy as settings — auth is required, and the signature covers the whole raw
// query, so every field is bound to the key (agent_parse.h explains why the
// state ping's value-only signature is not good enough for this one).
static const char* channelAgent(const StatusReq& r, char* body, size_t cap) {
  // Auth follows `?state=`, not the settings channel: with no `status_key` the
  // channel is in bench mode and the context is as unprotected as the animation
  // it decorates. It is *not* a persisted write, so it does not inherit the
  // "no key, no settings" rule — and refusing it would break a keyless setup that
  // worked before this kind existed.
  if (!authorizePayload(r.query, r.ts, r.sig)) {
    logReject("agent", r.peer);
    setBody(body, cap, "OK");   // the body a rejected request has always had
    return "401 Unauthorized";
  }

  AgentRequest req;
  char err[96] = {0};
  if (!agentParse(r.query, req, err, sizeof(err))) {
    setBody(body, cap, err[0] ? err : "bad agent query");
    return "400 Bad Request";
  }

  // Replace the context wholesale *before* the state machine runs: the two are
  // published together, so the first frame that can see the new state already
  // has the text that goes with it. A repeat of the same state is a no-op there
  // (no chime, no wake), which is what makes the plugin's heartbeat free.
  agentSessionsApply(s_agents, req.session, req.state, req.info, millis());
  syncAgent(millis(), true);

  // Not a secret (unlike the WiFi query), but bounded: repo/tool are short by
  // construction and the note is the interesting part when diagnosing a plugin
  // that sends the wrong field.
  const AgentInfo& a = statusChannelAgentInfo();
  Serial.printf("agent=%s session=%s active=%d repo=%s tool=%s note=%s\n", req.state,
                req.session[0] ? req.session : "-", s_agents.n,
                a.repo, a.tool, a.note);
  setBody(body, cap, "OK");
  return "200 OK";
}

// A WiFi credentials request: associate with what it carries, or drop what is
// stored (`?wifi_ssid=&wifi_pass=`). Everything else about it is the same policy
// as settings — auth required, signature over the whole query, no unauthenticated
// write path — because pointing a device at an attacker's AP is worse than
// changing its brightness.
static const char* channelWifi(const StatusReq& r, char* body, size_t cap) {
  if (!statusChannelAuthEnabled()) {
    Serial.println("wifi: REFUSED (no status_key configured)");
    setBody(body, cap, "wifi credentials require status_key\n");
    return "403 Forbidden";
  }

  if (!authorizePayload(r.query, r.ts, r.sig)) {
    logReject("wifi", r.peer);
    setBody(body, cap, "OK");   // the body a rejected request has always had
    return "401 Unauthorized";
  }

  WifiCred cred;
  char err[96] = {0};
  if (!wifiCredFromQuery(r.query, cred, err, sizeof(err))) {
    setBody(body, cap, err[0] ? err : "bad wifi query");
    return "400 Bad Request";
  }

  if (!s_wifiCb) {
    setBody(body, cap, "wifi channel is not wired up\n");
    return "503 Service Unavailable";
  }

  if (!s_wifiCb(cred)) {
    setBody(body, cap, "could not store credentials\n");
    return "500 Internal Server Error";
  }

  // Never the raw query here, unlike the settings line above: it holds the
  // passphrase. wifiCredDescribe() redacts it for the same reason.
  char line[160];
  wifiCredDescribe(cred, line, sizeof(line));
  Serial.printf("%s", line);
  wifiCredDescribe(cred, body, cap);
  return "200 OK";
}

// A notice request: one short line for the Notice page, with a mandatory TTL
// (parse/notice_parse.h). Same auth policy as `?agent=` — it is ephemeral, so a
// keyless bench setup keeps working — but it *lingers* until the TTL runs out,
// which is exactly why the expiry is enforced here rather than trusted from the
// client, and why the page draws a countdown: a stale notice that merely sits
// there looks current.
static const char* channelNotice(const StatusReq& r, char* body, size_t cap) {
  if (!authorizePayload(r.query, r.ts, r.sig)) {
    logReject("notice", r.peer);
    setBody(body, cap, "OK");
    return "401 Unauthorized";
  }

  NoticeRequest req;
  char err[96] = {0};
  if (!noticeParse(r.query, req, err, sizeof(err))) {
    setBody(body, cap, err[0] ? err : "bad notice query");
    return "400 Bad Request";
  }

  // Clearing is a signed write like any other: an unauthenticated peer cannot
  // take a reminder off the panel (that check is above).
  if (req.clear) {
    bool had = s_notice.valid;
    s_notice.valid = false;
    s_notice.rev++;
    Serial.printf("notice: cleared (%s)\n", had ? "had one" : "none");
    setBody(body, cap, "OK");
    return "200 OK";
  }

  const uint32_t now = millis();
  s_notice.valid      = true;
  s_notice.ttlSec     = req.ttlSec;
  s_notice.leftSec    = req.leftSec;
  s_notice.hasLeft    = req.hasLeft;
  s_notice.receivedMs = now;
  s_notice.expiresMs  = now + req.ttlSec * 1000u;
  s_notice.rev++;
  snprintf(s_notice.kind, sizeof(s_notice.kind), "%s", req.kind);
  snprintf(s_notice.text, sizeof(s_notice.text), "%s", req.text);

  Serial.printf("notice: kind=%s ttl=%lus left=%s%lus text=%s\n", req.kind,
                (unsigned long)req.ttlSec, req.hasLeft ? "" : "-",
                (unsigned long)req.leftSec, req.text);
  setBody(body, cap, "OK");
  return "200 OK";
}

const char* statusChannelHandle(const StatusReq& r, char* body, size_t cap) {
  const char* q = r.query ? r.query : "";
  if (!q[0]) {                        // no query at all: health probe
    setBody(body, cap, "OK");
    return "200 OK";
  }

  // WiFi credentials: their own kind, not a settings field (wifi_cred_parse.h).
  // Checked before the settings branch because this is the request a device with
  // no IP address has to be able to take.
  if (strncmp(q, "wifi_", 5) == 0) return channelWifi(r, body, cap);

  // Agent context: also its own kind, and also before the settings branch —
  // "everything else is settings" would otherwise parse it as a set of unknown
  // keys and reject it.
  if (strncmp(q, "agent=", 6) == 0) return channelAgent(r, body, cap);

  // A pushed notice: its own kind for the same reason, and the one that outlives
  // its request (notice_parse.h).
  if (strncmp(q, "notice=", 7) == 0) return channelNotice(r, body, cap);

  // Settings next: a `?state=` request keeps the original protocol, anything
  // else that is a query is the settings channel.
  if (strncmp(q, "state=", 6) != 0) return channelSettings(r, body, cap);

  // Here — and only here — the signature covers the *value*, not the query, so
  // the original leniency is kept: the state name is matched anywhere in the
  // query and signed on its own. A settings request signs the whole query
  // instead, which is why that rule belongs to the request rather than to a
  // transport.
  const char* st = nullptr;
  if      (strstr(q, "state=running") != nullptr) st = "running";
  else if (strstr(q, "state=waiting") != nullptr) st = "waiting";
  else if (strstr(q, "state=done")    != nullptr) st = "done";
  else if (strstr(q, "state=failed")  != nullptr) st = "failed";

  if (!st) {                          // health probe / unknown: nothing to do
    setBody(body, cap, "OK");
    return "200 OK";
  }

  if (!authorizePayload(st, r.ts, r.sig)) {
    logReject(st, r.peer);
    setBody(body, cap, "OK");
    return "401 Unauthorized";
  }

  // The transitions (and which of them are real) live in parse/status_machine.h,
  // where the host tests can drive them; the wake callback is the UI actor.
  // A bare state ping is the single empty-id session, so an older client keeps
  // working while session-aware ones run alongside it.
  AgentInfo info;
  int si = agentSessionFind(s_agents, "");
  if (si >= 0) info = s_agents.s[si].info;
  else         agentInfoClear(info);
  agentSessionsApply(s_agents, "", st, info, millis());
  syncAgent(millis(), true);
  Serial.printf("state=%s\n", st);

  setBody(body, cap, "OK");
  return "200 OK";
}
