#pragma once
#include <stdint.h>
#include "api_client.h"
#include "weather_client.h"
#include "ui_model.h"

void uiBeginFrame();
void uiSetEnv(const UiEnv& env);

// Cards are drawn at (ox, oy) so two of them can be composed for the swipe
// transition. Each card paints its own background rect.
void uiDrawUsage(const UsageData& u, int ox, int oy);
void uiDrawTrend(const UsageData& u, int ox, int oy);
void uiDrawWeatherCard(int card, const WeatherData& w, const MoonData& m,
                       const AirData& a, bool hasAirToken, int ox, int oy);
void uiDrawClock(int ox, int oy);

void uiDrawPageDots(int activePage);
void uiDrawCardDots(int activeCard, int count);

void usageDisplayDrawThinking(uint32_t elapsedMs);
void usageDisplayDrawWaiting(uint32_t elapsedMs);
void usageDisplayDrawEaster555(uint32_t elapsedMs);
