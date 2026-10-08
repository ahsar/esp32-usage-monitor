#pragma once
#include <stdint.h>
#include <stddef.h>
#include "token_store.h"

struct UsageData {
  bool valid;
  int rollingPercent;
  uint32_t rollingResetSec;
  int weeklyPercent;
  uint32_t weeklyResetSec;
  int monthlyPercent;
  uint32_t monthlyResetSec;
};

// Exchange the stored refresh token for a fresh access token.
// On success updates `tokens` (and sets tokens.dirty).
bool apiRefreshToken(TokenSet& tokens);

// Make sure `tokens` holds a usable access token, refreshing when missing/expired.
bool apiEnsureToken(TokenSet& tokens);

// Fetch current usage from the opencode console REST API.
// `tokens.orgId` must be set (falls back to the workspace id by the caller).
// Refreshes once on 401. Returns true when new usage data was parsed.
bool apiFetchUsage(UsageData& out, TokenSet& tokens);
