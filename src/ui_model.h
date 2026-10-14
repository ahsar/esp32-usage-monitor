#pragma once
// Neutral UI model shared by the gesture layer and the display layer.
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

// Shared, per-frame UI context (status bar + clock page).
struct UiEnv {
  bool online;
  bool timeValid;
  int  hh, mm, ss;
  int  year, month, day, weekday;   // weekday 0=Sun
  int  batteryPct;
  bool charging;
  bool usbPresent;
  const char* city;
};
