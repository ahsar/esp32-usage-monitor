#pragma once
// The notice request: "?notice=1&kind=meeting&text=Standup&ttl=3600&left=480".
//
// The second *text* kind on the device channel (`?agent=` is the first), and it
// exists for the same reason: the device never talks to the PC, so anything the
// PC wants it to say has to be pushed. What it adds over `?agent=` is that a
// notice outlives the request — it sits on its own page until its TTL runs out,
// which is what makes "the next meeting is in 8 minutes" useful at a glance.
//
// That makes it the one remote string that can *linger*, so two rules come with
// it (both enforced here, both host-tested):
//
//   - it must carry a TTL. An unexpiring notice is a lie waiting to happen: the
//     PC goes to sleep, the meeting starts, and the panel still counts down to
//     it. Missing/zero TTL is a 400, not a default.
//   - the text goes through the same sanitiser and field caps as `?agent=`
//     (`agent_parse.h`), so it is drawn through exactly one path: printable
//     ASCII only, and a fully percent-encoded request still fits
//     STATUS_QUERY_MAX / BLE_FRAME_MAX.
//
// Signed over the whole raw query, like `?agent=` and the settings channel.
// Unknown keys are ignored (the request is ephemeral, so a newer client can add
// a field without a firmware update); a bad value fails the whole request so a
// rejected notice cannot leave half of one on screen.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "agent_parse.h"   // agentSanitise / agentCopy — one text policy
#include "util/url_util.h"

constexpr size_t NOTICE_TEXT_CAP = 64;   // 63 characters
constexpr size_t NOTICE_KIND_CAP = 12;   // "meeting", "ci", "media", …

// A day is the longest a notice may live on the panel. Anything longer belongs
// in a page that fetches, not in a pushed string nobody refreshes.
constexpr uint32_t NOTICE_TTL_MAX  = 24u * 3600u;
// …and a week the furthest ahead the countdown may point (a calendar entry).
constexpr uint32_t NOTICE_LEFT_MAX = 7u * 86400u;

struct NoticeRequest {
  char     kind[NOTICE_KIND_CAP];   // empty = "note"
  char     text[NOTICE_TEXT_CAP];
  uint32_t ttlSec = 0;              // required: how long it stays on the panel
  uint32_t leftSec = 0;             // seconds until the event starts
  bool     hasLeft = false;         // false = no countdown, just the text
  bool     clear = false;           // "?notice=1&clear=1": drop it now
};

// Strict unsigned decimal: "0".."4294967295". Leading/trailing junk is a typo
// ("3600x"), not a number, and must not silently become 3600.
inline bool noticeParseU32(const char* s, uint32_t& out) {
  if (!s || !s[0]) return false;
  uint64_t v = 0;
  for (const char* p = s; *p; p++) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10u + (uint64_t)(*p - '0');
    if (v > 0xFFFFFFFFULL) return false;
  }
  out = (uint32_t)v;
  return true;
}

// Parse a raw (undecoded) query string starting with "notice=". All-or-nothing.
inline bool noticeParse(const char* query, NoticeRequest& out, char* err, size_t errCap) {
  auto fail = [&](const char* m) {
    if (err && errCap) snprintf(err, errCap, "%s", m);
    return false;
  };

  memset(&out, 0, sizeof(out));
  if (!query || !query[0]) return fail("empty notice query");

  bool sawNotice = false, sawText = false, sawTtl = false;

  for (const char* p = query; p && *p; ) {
    const char* amp = strchr(p, '&');
    size_t pairLen = amp ? (size_t)(amp - p) : strlen(p);

    // Worst pair: a fully encoded 63-char text (194 bytes) plus the key.
    char pair[224], key[16], val[200];
    if (pairLen == 0) return fail("malformed notice query");
    if (pairLen >= sizeof(pair)) return fail("notice query too long");
    memcpy(pair, p, pairLen);
    pair[pairLen] = '\0';
    p = amp ? amp + 1 : p + pairLen;

    char* eq = strchr(pair, '=');
    if (!eq) return fail("expected key=value");
    *eq = '\0';
    if (!urlDecode(pair, key, sizeof(key))) return fail("notice key too long");
    if (!urlDecode(eq + 1, val, sizeof(val))) return fail("notice value too long");

    if (!strcmp(key, "notice")) {
      sawNotice = true;
    } else if (!strcmp(key, "text")) {
      agentSanitise(val);
      if (!val[0]) return fail("empty notice text");
      if (!agentCopy(out.text, sizeof(out.text), val)) return fail("notice text too long");
      sawText = true;
    } else if (!strcmp(key, "kind")) {
      agentSanitise(val);
      if (!agentCopy(out.kind, sizeof(out.kind), val)) return fail("notice kind too long");
    } else if (!strcmp(key, "ttl")) {
      if (!noticeParseU32(val, out.ttlSec) || out.ttlSec == 0 || out.ttlSec > NOTICE_TTL_MAX)
        return fail("ttl must be 1..86400 seconds");
      sawTtl = true;
    } else if (!strcmp(key, "left")) {
      if (!noticeParseU32(val, out.leftSec) || out.leftSec > NOTICE_LEFT_MAX)
        return fail("left must be 0..604800 seconds");
      out.hasLeft = true;
    } else if (!strcmp(key, "clear")) {
      if (!strcmp(val, "1") || !strcmp(val, "true") || !strcmp(val, "on")) {
        out.clear = true;
      } else if (!strcmp(val, "0") || !strcmp(val, "false") || !strcmp(val, "off")) {
        out.clear = false;
      } else {
        return fail("clear must be 0/1");
      }
    }
    // Unknown keys: ignored on purpose (see the header comment).
  }

  if (!sawNotice) return fail("not a notice request");
  // Clearing is its own intent: it needs no text and no TTL, because there will
  // be nothing left to expire.
  if (out.clear) return true;
  if (!sawText)   return fail("missing notice text");
  if (!sawTtl)    return fail("missing ttl");
  if (!out.kind[0]) snprintf(out.kind, sizeof(out.kind), "%s", "note");
  return true;
}
