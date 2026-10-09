#include "api_client.h"
#include "http_client.h"
#include "util/time_util.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char* HOST      = "opencode.ai";
static const char* CLIENT_ID = "opencode-cli";

// Sanity floor for a synced clock (2020-09-13). Below this we treat time as unset.
static const uint32_t CLOCK_MIN = 1600000000u;

// ---------------------------------------------------------------------------
// Usage parsing
// ---------------------------------------------------------------------------

static unsigned long long jsonU64(JsonVariantConst v) {
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    return s ? strtoull(s, nullptr, 10) : 0;
  }
  if (v.is<long long>())          return (unsigned long long)v.as<long long>();
  if (v.is<unsigned long long>()) return v.as<unsigned long long>();
  if (v.is<double>())             return (unsigned long long)v.as<double>();
  return 0;
}

static int meterPercent(JsonVariantConst meter) {
  unsigned long long used  = jsonU64(meter["usedMicroCents"]);
  unsigned long long limit = jsonU64(meter["limitMicroCents"]);
  if (limit == 0) return 0;
  unsigned long long pct = (used * 100ULL) / limit;
  if (pct > 100) pct = 100;
  return (int)pct;
}

static bool parseUsage(const String& body, const String& dateHeader, UsageData& out) {
  out.valid = false;

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

  JsonObjectConst meters = doc["access"]["meters"].as<JsonObjectConst>();
  if (meters.isNull()) return false;

  uint32_t nowSec = (uint32_t)time(nullptr);
  if (nowSec < CLOCK_MIN) nowSec = httpDateToEpoch(dateHeader.c_str());

  struct MeterMap { const char* key; int* pct; uint32_t* reset; };
  MeterMap map[3] = {
    { "fiveHour", &out.rollingPercent, &out.rollingResetSec },
    { "week",     &out.weeklyPercent,  &out.weeklyResetSec  },
    { "month",    &out.monthlyPercent, &out.monthlyResetSec },
  };

  bool any = false;
  for (auto& m : map) {
    JsonObjectConst meter = meters[m.key].as<JsonObjectConst>();
    if (meter.isNull()) continue;

    *m.pct = meterPercent(meter);

    uint32_t resetsAt = isoToEpoch(meter["resetsAt"] | "");
    *m.reset = (resetsAt && nowSec >= CLOCK_MIN && resetsAt > nowSec) ? (resetsAt - nowSec) : 0;
    any = true;
  }

  out.valid = any;
  return any;
}

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------

static bool parseTokenResponse(const String& body, TokenSet& tokens) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

  const char* at = doc["access_token"] | "";
  if (!at[0]) return false;

  strlcpy(tokens.accessToken, at, sizeof(tokens.accessToken));

  const char* rt = doc["refresh_token"] | "";
  if (rt[0]) strlcpy(tokens.refreshToken, rt, sizeof(tokens.refreshToken));

  const char* org = doc["org_id"] | "";
  if (org[0]) strlcpy(tokens.orgId, org, sizeof(tokens.orgId));

  uint32_t now = (uint32_t)time(nullptr);
  unsigned long long ttl = jsonU64(doc["expires_in"]);
  tokens.accessExpiresAt = (now >= CLOCK_MIN) ? (uint32_t)(now + ttl) : 0;

  tokens.valid = tokens.refreshToken[0] != 0;
  tokens.dirty = true;
  return true;
}

bool apiRefreshToken(TokenSet& tokens) {
  if (!tokens.refreshToken[0]) return false;

  // refresh_token values are opaque ASCII tokens (no escaping needed).
  char body[768];
  snprintf(body, sizeof(body),
           "{\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\",\"client_id\":\"%s\"}",
           tokens.refreshToken, CLIENT_ID);

  HttpResponse res;
  if (!httpRequest(HOST, "POST", "/console/auth/device/token", nullptr, body, res)) return false;
  if (res.status == 400) {
    // invalid_grant: the refresh token was rotated/revoked (or its family was
    // invalidated because an older token got reused). Stop using it.
    Serial.println("refresh rejected (invalid_grant); re-authorise with tools/authorize.py");
    tokens.accessToken[0]  = 0;
    tokens.refreshToken[0] = 0;
    tokens.accessExpiresAt = 0;
    tokens.valid = false;
    tokens.dirty = true;
    return false;
  }
  if (res.status != 200) {
    Serial.printf("refresh failed: HTTP %d\n", res.status);
    return false;
  }
  return parseTokenResponse(res.body, tokens);
}

bool apiEnsureToken(TokenSet& tokens) {
  if (!tokens.refreshToken[0]) return false;

  uint32_t now = (uint32_t)time(nullptr);
  if (tokens.accessToken[0]) {
    if (now < CLOCK_MIN) return true;               // no clock yet: trust the token
    if (tokens.accessExpiresAt == 0 ||
        now + 60 < tokens.accessExpiresAt) return true;
  }
  return apiRefreshToken(tokens);
}

bool apiFetchUsage(UsageData& out, TokenSet& tokens) {
  out.valid = false;
  if (!apiEnsureToken(tokens)) return false;

  String headers;
  if (tokens.orgId[0]) { headers += "x-org-id: "; headers += tokens.orgId; headers += "\r\n"; }
  if (tokens.accessToken[0]) { headers += "Authorization: Bearer "; headers += tokens.accessToken; headers += "\r\n"; }

  HttpResponse res;
  if (!httpRequest(HOST, "GET", "/console/api/go/status", headers.c_str(), nullptr, res)) return false;

  if (res.status == 401) {
    if (!apiRefreshToken(tokens)) return false;
    headers = "";
    if (tokens.orgId[0]) { headers += "x-org-id: "; headers += tokens.orgId; headers += "\r\n"; }
    if (tokens.accessToken[0]) { headers += "Authorization: Bearer "; headers += tokens.accessToken; headers += "\r\n"; }
    if (!httpRequest(HOST, "GET", "/console/api/go/status", headers.c_str(), nullptr, res)) return false;
  }
  if (res.status != 200) {
    Serial.printf("usage fetch failed: HTTP %d\n", res.status);
    return false;
  }
  if (!parseUsage(res.body, res.date, out)) {
    Serial.println("usage parse failed");
    return false;
  }
  Serial.printf("usage: rolling=%d%% (%lus) weekly=%d%% (%lus) monthly=%d%% (%lus)\n",
                out.rollingPercent, (unsigned long)out.rollingResetSec,
                out.weeklyPercent,  (unsigned long)out.weeklyResetSec,
                out.monthlyPercent, (unsigned long)out.monthlyResetSec);
  return true;
}
