#pragma once
#include <stdint.h>
#include "data_types.h"
#include "ui_model.h"

void uiBeginFrame();
void uiSetEnv(const UiEnv& env);

// Cards are drawn at (ox, oy) so two of them can be composed for the swipe
// transition. Each card paints its own background rect.
void uiDrawUsage(const UsageData& u, int ox, int oy);
void uiDrawTrend(const UsageData& u, int ox, int oy);
void uiDrawWeatherCard(int card, const WeatherData& w, const MoonData& m,
                       const AirData& a, const ForecastData& f, bool hasAirToken,
                       int ox, int oy);
void uiDrawClock(int ox, int oy);

// The Notice page: what the PC last pushed over the device channel
// (parse/notice_parse.h), already resolved against the clock by main.cpp.
void uiDrawNotice(const NoticeData& n, int ox, int oy);

// The device's own state: link, uptime, memory, the last fetch, firmware. Read
// only — this page never causes a fetch, a flash write or a radio change.
void uiDrawHealth(const HealthData& h, int ox, int oy);

void uiDrawPageDots(int activePage);
void uiDrawCardDots(int activeCard, int count);

// Agent-activity screens. `repo`/`tool`/`note` are what the last `?agent=` ping
// carried (status_channel.h) and may each be empty — the card draws whatever is
// there. `running` is how many sessions are active, so two OpenCode windows read
// as "2 running" instead of looking like one. Strings rather than the AgentInfo
// struct so the UI layer keeps its distance from the parser headers.
void usageDisplayDrawThinking(uint32_t elapsedMs, const char* repo, const char* tool,
                              int running);
void usageDisplayDrawWaiting(uint32_t elapsedMs, const char* note, const char* repo);

// Firmware update in flight (ArduinoOTA). The device reboots on success, so
// this screen is never dismissed by the UI: it is painted from the OTA progress
// callback because loop() is blocked inside the receive loop for the whole
// transfer. Keep it cheap — it redraws between flash writes.
void usageDisplayDrawOta(uint32_t pct);
