#pragma once
#include <stdint.h>
#include "data_types.h"
#include "ui_model.h"

void uiBeginFrame();
void uiSetEnv(const UiEnv& env);

// Animation clock for the cards, set once per render. 0 means "draw a static
// frame" — the dimmed panel, a build with FRAME_DUMP, and any page that is not
// animated all get that, so a card never has to know why motion is not wanted.
void uiSetAnimMs(uint32_t ms);

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

// The agent card stack's dots, in the page-dot row (the agent screens draw no
// page dots, so the slot is free): how many sessions are up and which one is
// showing. Drawn only when there is more than one, so a single session still
// looks exactly like the original screen.
void uiDrawAgentDots(int activeCard, int count);

// Agent-activity screens. `repo`/`tool`/`note` are what that session's last
// `?agent=` ping carried (status_channel.h) and may each be empty — the card
// draws whatever is there. `pos`/`total` place it in the stack of live sessions
// (statusChannelAgentCards): one screen per session, steppable with the
// horizontal axis while it is up. Strings rather than the AgentInfo struct so
// the UI layer keeps its distance from the parser headers.
void usageDisplayDrawThinking(uint32_t elapsedMs, const char* repo, const char* tool,
                              int pos, int total);
void usageDisplayDrawWaiting(uint32_t elapsedMs, const char* note, const char* repo,
                             int pos, int total);

// Firmware update in flight (ArduinoOTA). The device reboots on success, so
// this screen is never dismissed by the UI: it is painted from the OTA progress
// callback because loop() is blocked inside the receive loop for the whole
// transfer. Keep it cheap — it redraws between flash writes.
void usageDisplayDrawOta(uint32_t pct);
