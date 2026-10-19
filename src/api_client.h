#pragma once
#include <stdint.h>
#include <stddef.h>
#include "data_types.h"   // UsageData
#include "token_store.h"  // TokenSet

// Exchange the stored refresh token for a fresh access token.
// On success updates `tokens` (and sets tokens.dirty).
bool apiRefreshToken(TokenSet& tokens);

// Hook the refresh path calls the moment a rotation has been parsed, before the
// caller continues. The new refresh token exists only in RAM at that point, so a
// device that loses power (or reboots for an update) before the token store has
// written it comes back holding the token it already spent — the reuse that
// revokes the whole OAuth family. The fetch task registers its hand-off here so
// the unpersisted window is the round trip, not the rest of the job.
void apiSetTokenRotatedHook(void (*cb)());

// Make sure `tokens` holds a usable access token, refreshing when missing/expired.
bool apiEnsureToken(TokenSet& tokens);

// Fetch current usage from the opencode console REST API.
// `tokens.orgId` must be set (falls back to the workspace id by the caller).
// Refreshes once on 401. Returns true when new usage data was parsed.
bool apiFetchUsage(UsageData& out, TokenSet& tokens);
