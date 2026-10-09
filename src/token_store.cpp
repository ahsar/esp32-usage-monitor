#include "token_store.h"
#include "storage.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string.h>

static const char* TOKENS_PATH = "/tokens.json";

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

  File f = LittleFS.open(TOKENS_PATH, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}
