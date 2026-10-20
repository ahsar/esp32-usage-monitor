#pragma once
// The agent-context request: "?agent=1&state=running&repo=NAME&tool=NAME&note=TEXT&session=ID".
//
// Why this is its own kind rather than more fields on `?state=`: a state ping
// keeps the original *value-only* signature (`status_channel.cpp` finds
// "state=running" anywhere in the query and signs just that word), so anything
// else travelling in the same query would be unsigned — a LAN peer could append
// "&note=whatever" to a captured ping and dictate what the panel says. This
// request is signed over the whole raw query, exactly like the settings channel,
// and that is what makes the text trustworthy enough to draw.
//
// Header-only and hardware-free (ArduinoJson is not even needed): the host tests
// cover every rejection and the length budget.
//
// Auth is required (no `status_key`, no agent requests) for the same reason the
// settings channel requires it: this is a write, even if it only lasts until the
// next one. It is *not* the same as the settings channel in one respect — nothing
// is persisted, so unknown keys are ignored instead of failing the request. A
// newer client can add a field without a firmware update; a typo just does
// nothing, and there is no half-configured state to protect.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util/url_util.h"

// Field caps, as buffer sizes (so N-1 characters). Chosen so a fully
// percent-encoded request — every byte as %XX, i.e. x3 — still fits
// STATUS_QUERY_MAX (384; see status_channel.h), which both transports size their
// frames from. Worst case: 6 + 6 + 3*9 + 6 + 3*27 + 6 + 3*19 + 6 + 3*51 + 9 + 3*7
// = 381.
constexpr size_t AGENT_STATE_CAP = 10;   // "interrupted" would still fit
constexpr size_t AGENT_REPO_CAP  = 28;
constexpr size_t AGENT_TOOL_CAP  = 20;
constexpr size_t AGENT_NOTE_CAP  = 52;
// A short client-chosen session token ("s1", or 7 hex chars), not the host's
// session id: the device only needs to tell sessions apart, and every byte here
// costs three in a percent-encoded query.
constexpr size_t AGENT_SESSION_CAP = 8;

// What the animation screen shows alongside "Thinking..." / "Needs you".
// Every field is optional; an empty string means "not sent".
struct AgentInfo {
  char repo[AGENT_REPO_CAP];
  char tool[AGENT_TOOL_CAP];
  char note[AGENT_NOTE_CAP];   // the permission/question text, clipped
};

struct AgentRequest {
  AgentInfo info;
  char      state[AGENT_STATE_CAP];
  char      session[AGENT_SESSION_CAP];   // empty = the single legacy session
};

inline void agentInfoClear(AgentInfo& a) { memset(&a, 0, sizeof(a)); }

// The states the machine knows (parse/status_machine.h); anything else is a
// client bug and gets a 400 rather than a silent no-op that the user would read
// as "the device is broken".
inline bool agentStateOk(const char* s) {
  return s && (!strcmp(s, "running") || !strcmp(s, "waiting") ||
               !strcmp(s, "done")    || !strcmp(s, "failed"));
}

// Drop anything that is not printable ASCII: these strings go straight to the
// LVGL rasteriser, and a control byte out of a model-generated note must not be
// able to corrupt a frame. Removing a character is preferable to rejecting the
// request — the context is advisory, and losing the whole thing over one stray
// byte would be a worse outcome than losing the byte.
inline void agentSanitise(char* s) {
  char* w = s;
  for (char* p = s; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c >= 0x20 && c != 0x7F) *w++ = *p;
  }
  *w = '\0';
}

// Copy `v` into `dst`, rejecting an over-long value rather than silently
// truncating (the client owns the clip; the device gives a reason instead).
inline bool agentCopy(char* dst, size_t cap, const char* v) {
  size_t n = v ? strlen(v) : 0;
  if (n >= cap) return false;
  memcpy(dst, v ? v : "", n);
  dst[n] = '\0';
  return true;
}

// Parse a raw (undecoded) query string starting with "agent=". All-or-nothing:
// a bad state or an over-long field returns false with a message and the caller
// applies nothing, so a rejected request cannot leave half a context on screen.
inline bool agentParse(const char* query, AgentRequest& out, char* err, size_t errCap) {
  auto fail = [&](const char* m) {
    if (err && errCap) snprintf(err, errCap, "%s", m);
    return false;
  };

  memset(&out, 0, sizeof(out));
  if (!query || !query[0]) return fail("empty agent query");

  bool sawAgent = false;
  for (const char* p = query; p && *p; ) {
    const char* amp = strchr(p, '&');
    size_t pairLen = amp ? (size_t)(amp - p) : strlen(p);

    // The longest legitimate pair is "note=" plus a fully encoded 51-char note
    // (159 bytes); anything past this is malformed, and the whole query cannot
    // exceed 384 anyway.
    char pair[192], key[16], val[176];
    if (pairLen == 0) return fail("malformed agent query");
    if (pairLen >= sizeof(pair)) return fail("agent query too long");
    memcpy(pair, p, pairLen);
    pair[pairLen] = '\0';
    p = amp ? amp + 1 : p + pairLen;

    char* eq = strchr(pair, '=');
    if (!eq) return fail("expected key=value");
    *eq = '\0';
    if (!urlDecode(pair, key, sizeof(key))) return fail("agent key too long");
    if (!urlDecode(eq + 1, val, sizeof(val))) return fail("agent value too long");

    if (!strcmp(key, "agent")) {
      sawAgent = true;
    } else if (!strcmp(key, "state")) {
      agentSanitise(val);
      if (!agentStateOk(val)) return fail("unknown agent state");
      snprintf(out.state, sizeof(out.state), "%s", val);
    } else if (!strcmp(key, "session")) {
      agentSanitise(val);
      if (!agentCopy(out.session, sizeof(out.session), val)) return fail("session too long");
    } else if (!strcmp(key, "repo")) {
      agentSanitise(val);
      if (!agentCopy(out.info.repo, sizeof(out.info.repo), val)) return fail("repo too long");
    } else if (!strcmp(key, "tool")) {
      agentSanitise(val);
      if (!agentCopy(out.info.tool, sizeof(out.info.tool), val)) return fail("tool too long");
    } else if (!strcmp(key, "note")) {
      agentSanitise(val);
      if (!agentCopy(out.info.note, sizeof(out.info.note), val)) return fail("note too long");
    }
    // Unknown keys: ignored on purpose, see the header comment.
  }

  if (!sawAgent)   return fail("not an agent request");
  if (!out.state[0]) return fail("missing state");
  return true;
}
