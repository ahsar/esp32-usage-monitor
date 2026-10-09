#include "api_client.h"
#include "http_client.h"
#include "parse/usage_parse.h"
#include "util/time_util.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char* HOST      = "opencode.ai";
static const char* CLIENT_ID = "opencode-cli";

// ---------------------------------------------------------------------------
// Usage parsing
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
  unsigned long long ttl = usage_parse_detail::jsonU64(doc["expires_in"]);
  tokens.accessExpiresAt = (now >= CLOCK_VALID_MIN) ? (uint32_t)(now + ttl) : 0;

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
  if (!httpRequest(HOST, "POST", "/console/auth/device/token", nullptr, body, res,
                   false, 4096, 12000)) return false;
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
    if (now < CLOCK_VALID_MIN) return true;               // no clock yet: trust the token
    if (tokens.accessExpiresAt == 0 ||
        now + 60 < tokens.accessExpiresAt) return true;
  }
  return apiRefreshToken(tokens);
}

bool apiFetchUsage(UsageData& out, TokenSet& tokens) {
  if (!apiEnsureToken(tokens)) return false;

  String headers;
  if (tokens.orgId[0]) { headers += "x-org-id: "; headers += tokens.orgId; headers += "\r\n"; }
  if (tokens.accessToken[0]) { headers += "Authorization: Bearer "; headers += tokens.accessToken; headers += "\r\n"; }

  HttpResponse res;
  if (!httpRequest(HOST, "GET", "/console/api/go/status", headers.c_str(), nullptr, res,
                   false, 8192, 12000)) return false;

  if (res.status == 401) {
    if (!apiRefreshToken(tokens)) return false;
    headers = "";
    if (tokens.orgId[0]) { headers += "x-org-id: "; headers += tokens.orgId; headers += "\r\n"; }
    if (tokens.accessToken[0]) { headers += "Authorization: Bearer "; headers += tokens.accessToken; headers += "\r\n"; }
    if (!httpRequest(HOST, "GET", "/console/api/go/status", headers.c_str(), nullptr, res,
                     false, 8192, 12000)) return false;
  }
  if (res.status != 200) {
    Serial.printf("usage fetch failed: HTTP %d\n", res.status);
    return false;
  }
  uint32_t nowSec = (uint32_t)time(nullptr);
  if (nowSec < CLOCK_VALID_MIN) nowSec = httpDateToEpoch(res.date.c_str());
  // Parse into a scratch copy and only publish on success, so the UI (which is
  // pumped during this blocking fetch) never renders a half-cleared meter.
  UsageData tmp;
  if (!usageParse(res.body.c_str(), nowSec, tmp)) {
    Serial.println("usage parse failed");
    return false;
  }
  out = tmp;
  Serial.printf("usage: rolling=%d%% (%lus) weekly=%d%% (%lus) monthly=%d%% (%lus)\n",
                out.rollingPercent, (unsigned long)out.rollingResetSec,
                out.weeklyPercent,  (unsigned long)out.weeklyResetSec,
                out.monthlyPercent, (unsigned long)out.monthlyResetSec);
  return true;
}
