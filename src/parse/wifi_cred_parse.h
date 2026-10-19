#pragma once
// WiFi credentials from the device channel: the one request a device that is not
// on the network yet can still be given, and the reason it has to work over BLE
// (until these land there is no IP path to the device at all — see
// ble_status.cpp).
//
// Deliberately *not* a settings_parse.h field. Settings are overrides persisted
// to /settings.json, which is a readable record of what was changed over the
// channel; a WiFi passphrase does not belong in one. This request writes NVS
// through hw/net.h and is an instruction to *act* (associate) rather than an
// override to store, so it gets its own kind, its own parser and no persistence
// of its own.
//
// Header-only and dependency-free apart from the URL decoder, so the whole "what
// does this request mean, is it usable, what are we being asked to do" decision
// is host-tested (test/test_util) and the firmware only has to act on the result.
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "util/url_util.h"

// 802.11 SSID / WPA2-PSK passphrase maxima (bytes, no NUL).
constexpr size_t WIFI_SSID_CAP = 32;
constexpr size_t WIFI_PASS_CAP = 63;

// Enough for one pair with every byte percent-encoded ("wifi_pass=" + 63*3).
constexpr size_t WIFI_PAIR_MAX = 224;

// What the request asked for.
enum WifiCredAction : uint8_t {
  WIFI_CRED_NONE = 0,   // nothing parsed
  WIFI_CRED_SET,        // associate with ssid/pass
  WIFI_CRED_FORGET,     // drop what is stored, back to the setup screen
};

struct WifiCred {
  WifiCredAction action = WIFI_CRED_NONE;
  char ssid[WIFI_SSID_CAP + 1] = {0};
  char pass[WIFI_PASS_CAP + 1] = {0};
};

// Apply a raw query (no leading '?', not yet decoded), e.g.
// "wifi_ssid=Home&wifi_pass=hunter2", on top of `out`.
//
// - both keys must be present in the same request: a name without its passphrase
//   (or the reverse) would silently keep half of the old network's credentials;
// - an empty `wifi_ssid` *with* an empty `wifi_pass` means **forget** — the
//   device drops what it stored and comes back up in the setup UI;
// - an empty passphrase with a name is a valid request (an open network);
// - all-or-nothing: an unknown key, a value that does not fit or a missing half
//   returns false with a message and leaves `out` untouched, so a typo cannot
//   half-configure the device. The passphrase length is deliberately *not*
//   checked beyond its maximum: a WEP key and a WPA2 passphrase have different
//   valid ranges, and the association result is the real authority.
inline bool wifiCredFromQuery(const char* query, WifiCred& out, char* err, size_t errCap) {
  auto fail = [&](const char* msg) {
    if (err && errCap) snprintf(err, errCap, "%s", msg);
    return false;
  };

  if (!query || !query[0]) return fail("empty wifi query");

  bool haveSsid = false, havePass = false;
  char ssid[WIFI_SSID_CAP + 1] = {0}, pass[WIFI_PASS_CAP + 1] = {0};

  for (const char* p = query; p && *p; ) {
    const char* amp = strchr(p, '&');
    size_t pairLen = amp ? (size_t)(amp - p) : strlen(p);

    char pair[WIFI_PAIR_MAX], key[16], val[WIFI_PAIR_MAX];
    if (pairLen == 0) return fail("malformed wifi query");
    if (pairLen >= sizeof(pair)) return fail("wifi query too long");
    memcpy(pair, p, pairLen);
    pair[pairLen] = '\0';
    p = amp ? amp + 1 : p + pairLen;

    char* eq = strchr(pair, '=');
    if (!eq) return fail("expected key=value");
    *eq = '\0';
    if (!urlDecode(pair, key, sizeof(key))) return fail("wifi key too long");
    if (!urlDecode(eq + 1, val, sizeof(val))) return fail("wifi value too long");

    if (!strcmp(key, "wifi_ssid")) {
      size_t n = strlen(val);
      if (n > WIFI_SSID_CAP) return fail("wifi_ssid too long (max 32 characters)");
      memcpy(ssid, val, n + 1);
      haveSsid = true;
    } else if (!strcmp(key, "wifi_pass")) {
      size_t n = strlen(val);
      if (n > WIFI_PASS_CAP) return fail("wifi_pass too long (max 63 characters)");
      memcpy(pass, val, n + 1);
      havePass = true;
    } else {
      // This is also what rejects a settings key mixed into a wifi request: the
      // two kinds are never applied together.
      return fail("unknown wifi key");
    }
  }

  if (!haveSsid || !havePass)
    return fail("wifi_ssid and wifi_pass must be given together");
  if (!ssid[0] && pass[0])
    return fail("wifi_ssid is empty but wifi_pass is not");

  out.action = ssid[0] ? WIFI_CRED_SET : WIFI_CRED_FORGET;
  memcpy(out.ssid, ssid, sizeof(out.ssid));
  memcpy(out.pass, pass, sizeof(out.pass));
  return true;
}

// The query as it is safe to log. A WiFi credentials request names the network
// and hides everything else: the passphrase travels in the same bytes, and the
// BLE transport otherwise prints whole frames. `buf` is only used in that case,
// so callers can pass a small stack buffer and ignore it for other queries.
inline const char* wifiQueryForLog(const char* query, char* buf, size_t cap) {
  if (!query || strncmp(query, "wifi_", 5) != 0) return query;
  snprintf(buf, cap, "wifi_ssid=<credentials, passphrase redacted>");
  return buf;
}

// Human-readable summary, for the reply body and the serial log. **Never echoes
// the passphrase**: the reply travels back over a link anyone in radio range can
// hear (BLE is unauthenticated at the link layer — the HMAC is what makes the
// *write* trustworthy, not the transport), and the serial log has no business
// holding a secret either.
inline void wifiCredDescribe(const WifiCred& c, char* out, size_t cap) {
  if (!out || !cap) return;
  out[0] = '\0';
  if (c.action == WIFI_CRED_FORGET) {
    snprintf(out, cap, "wifi: credentials cleared; back to the setup screen\n");
    return;
  }
  if (c.action == WIFI_CRED_SET) {
    if (c.pass[0]) {
      snprintf(out, cap, "wifi: connecting to \"%s\" (passphrase: %u chars); "
                         "kept if it connects\n",
               c.ssid, (unsigned)strlen(c.pass));
    } else {
      snprintf(out, cap, "wifi: connecting to \"%s\" (open network); "
                         "kept if it connects\n", c.ssid);
    }
    return;
  }
  snprintf(out, cap, "wifi: nothing to do\n");
}
