#pragma once
#include <stdint.h>
#include "api_client.h"
#include "weather_client.h"

enum UiPage { UI_PAGE_WEATHER = 0, UI_PAGE_USAGE = 1, UI_PAGE_CLOCK = 2, UI_PAGE_COUNT = 3 };

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

void uiBeginFrame();
void uiSetEnv(const UiEnv& env);
const char* uiPageTitle(int page);

// Cards are drawn at (ox, oy) so two of them can be composed for the swipe
// transition. Each card paints its own background rect.
void uiDrawUsage(const UsageData& u, int ox, int oy);
void uiDrawWeatherCard(int card, const WeatherData& w, const MoonData& m,
                       const AirData& a, bool hasAirToken, int ox, int oy);
void uiDrawClock(int ox, int oy);
void uiDrawAuthNeeded(int ox, int oy);

void uiDrawPageDots(int activePage);
void uiDrawCardDots(int activeCard, int count);

void usageDisplayDrawThinking(uint32_t elapsedMs);
void usageDisplayDrawEaster555(uint32_t elapsedMs);
