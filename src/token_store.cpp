#include "token_store.h"
#include "storage.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string.h>

static const char* TOKENS_PATH = "/tokens.json";
static const char* TOKENS_TMP  = "/tokens.json.tmp";

bool tokenStoreLoad(TokenSet& t) {
  memset(&t, 0, sizeof(t));

  if (!storageMount()) return false;

  File f = LittleFS.open(TOKENS_PATH, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  strlcpy(t.refreshToken, doc["refresh_token"] | "", sizeof(t.refreshToken));
  strlcpy(t.accessToken,  doc["access_token"]  | "", sizeof(t.accessToken));
  strlcpy(t.orgId,        doc["org_id"]        | "", sizeof(t.orgId));
  t.accessExpiresAt = doc["expires_at"] | (uint32_t)0;
  t.valid = t.refreshToken[0] != 0;
  t.dirty = false;
  return true;
}

bool tokenStoreSave(const TokenSet& t) {
  if (!storageMount()) return false;

  JsonDocument doc;
  doc["refresh_token"] = t.refreshToken;
  doc["access_token"]  = t.accessToken;
  doc["org_id"]        = t.orgId;
  doc["expires_at"]    = t.accessExpiresAt;

  // Write to a temp file, verify the byte count, then rename over the live one.
  // A LittleFS short write or a failed serialization must never truncate
  // /tokens.json: after a reboot the device would fall back to config.json's
  // (long-rotated) refresh token, and reusing that revokes the whole token
  // family — the one failure mode this store exists to prevent. The byte-count
  // check is what stands between an unchecked serializeJson() and the
  // filesystem.
  File f = LittleFS.open(TOKENS_TMP, "w");
  if (!f) return false;

  const size_t want = (size_t)measureJson(doc);
  const size_t got  = serializeJson(doc, f);
  bool ok = want > 0 && got == want;
  if (ok) f.flush();                      // flush the underlying stream too
  ok = ok && f.size() == want;
  f.close();
  if (!ok) {
    Serial.printf("tokens: short write (%u/%u bytes); keeping the old file\n",
                  (unsigned)got, (unsigned)want);
    LittleFS.remove(TOKENS_TMP);
    return false;
  }

  // LittleFS rename is itself atomic (a crash mid-rename leaves one intact
  // file); most stacks overwrite the target, and the fallback below covers the
  // ones that refuse rename-over-existing.
  if (!LittleFS.rename(TOKENS_TMP, TOKENS_PATH)) {
    LittleFS.remove(TOKENS_PATH);
    if (!LittleFS.rename(TOKENS_TMP, TOKENS_PATH)) {
      LittleFS.remove(TOKENS_TMP);
      return false;
    }
  }
  return true;
}

bool tokenStoreInit(TokenSet& t, const char* fallbackRefresh, const char* fallbackOrg) {
  tokenStoreLoad(t);

  if (!t.refreshToken[0] && fallbackRefresh && fallbackRefresh[0]) {
    strlcpy(t.refreshToken, fallbackRefresh, sizeof(t.refreshToken));
    t.valid = true;
    t.dirty = true;
  }
  if (!t.orgId[0] && fallbackOrg && fallbackOrg[0]) {
    strlcpy(t.orgId, fallbackOrg, sizeof(t.orgId));
  }
  return t.valid;
}
