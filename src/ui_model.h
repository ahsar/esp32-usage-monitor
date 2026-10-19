#pragma once
// Neutral UI model shared by the pager and the display layer.
//
// Kept free of any drawing / hardware dependency so modules like pager.cpp can
// reason about pages and cards without pulling in the whole display stack.
#include <stdint.h>

enum UiPage { UI_PAGE_WEATHER = 0, UI_PAGE_USAGE = 1, UI_PAGE_TREND = 2, UI_PAGE_CLOCK = 3, UI_PAGE_COUNT = 4 };

enum UiWeatherCard {
  UI_WC_NOW = 0,
  UI_WC_SUN,
  UI_WC_AIR,
  UI_WC_MOON,
  UI_WC_MOONSET,
  UI_WC_COUNT
};

// Both axes are endless carousels: stepping past either end wraps to the far
// end instead of clamping, so the gesture machine, the commit animation and the
// two-tile composition all agree on which neighbour sits next to the current
// page/card. Shared here (rather than duplicated in pager/ui_compose) so the
// three call sites cannot drift.
inline int uiPageNext(int page) { return (page + 1) % UI_PAGE_COUNT; }
inline int uiPagePrev(int page) { return (page + UI_PAGE_COUNT - 1) % UI_PAGE_COUNT; }
inline int uiCardNext(int card) { return (card + 1) % UI_WC_COUNT; }
inline int uiCardPrev(int card) { return (card + UI_WC_COUNT - 1) % UI_WC_COUNT; }

// Shared, per-frame UI context (status bar + clock page).
struct UiEnv {
  bool online;
  bool timeValid;
  int  hh, mm, ss;
  int  year, month, day, weekday;   // weekday 0=Sun
  int  batteryPct;
  bool charging;
  bool usbPresent;
  const char* city;        // status-bar title on the Weather page
  const char* airLabel;    // Air card subtitle (the monitoring station)

  // Device identity, drawn on the clock page — the one page that describes the
  // device itself (time, battery), so it is where the LAN address belongs: mDNS
  // does not cross a VLAN boundary, and reaching for a serial console to read an
  // IP off the boot log is exactly what this avoids.
  uint32_t ipv4;           // 0 = no address yet (WiFi down or still associating)
  bool     bleUp;          // the BLE fallback came up; NOT proof it is advertising

  // The usage meter has no data and no poll will ever bring any: the worker has
  // no usable refresh token. This is terminal rather than transient — only a
  // refusal from the token endpoint (400, in practice `invalid_grant`) or a
  // device that was never authorised clears the token, while a request that
  // merely failed on the network leaves it alone — which is why the Usage page
  // can say "re-authorise" instead of drawing three grey bars that look like a
  // fetch still in flight.
  bool     authNeeded;
};
