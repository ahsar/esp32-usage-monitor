#pragma once
#include <stdint.h>

// OAuth device-flow credentials for the opencode console API.
// Kept in a separate file (/tokens.json) so runtime refresh never clobbers
// the user-edited /config.json.
struct TokenSet {
  char     refreshToken[512];
  char     accessToken[2048];
  char     orgId[128];
  uint32_t accessExpiresAt;   // epoch seconds (UTC), 0 when unknown
  bool     valid;             // a refresh token is present
  bool     dirty;             // updated in RAM but not yet written to flash
};

bool tokenStoreLoad(TokenSet& t);
bool tokenStoreSave(const TokenSet& t);

// Load persisted tokens and apply config.json fallbacks: seed the refresh token
// from `fallbackRefresh` when none is stored, and the org id from `fallbackOrg`
// when none is known. Returns true when a usable refresh token is present.
//
// Kept here (rather than in main) so the single-use / rotating-refresh-token
// migration lives in one place.
bool tokenStoreInit(TokenSet& t, const char* fallbackRefresh, const char* fallbackOrg);
