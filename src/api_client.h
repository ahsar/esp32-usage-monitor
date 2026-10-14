#pragma once
#include <stdint.h>
#include <stddef.h>
#include "token_store.h"

// Per-meter spend, added so the trend page can show cost and its rate rather
// than just a percentage. Microcents are 1e-6 cents, so $1.00 = 1e8.
struct UsageData {
  bool valid;
  int rollingPercent;
  uint32_t rollingResetSec;
  int weeklyPercent;
  uint32_t weeklyResetSec;
  int monthlyPercent;
  uint32_t monthlyResetSec;

  uint64_t rollingUsedMicroCents;
  uint64_t rollingLimitMicroCents;
  uint64_t weeklyUsedMicroCents;
  uint64_t weeklyLimitMicroCents;
  uint64_t monthlyUsedMicroCents;
  uint64_t monthlyLimitMicroCents;
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
