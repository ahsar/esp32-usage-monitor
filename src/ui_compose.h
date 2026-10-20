#pragma once
// UI composition: decides which page/card(s) to draw for the current pager
// state (including the two-tile swipe transition) and drives the display
// layer. Kept out of main.cpp so the entrypoint only wires data + input.
#include "data_types.h"

// Snapshot of everything the UI reads. Pointers stay valid for the duration of
// a render call; main.cpp owns the underlying storage.
struct UiData {
  const UsageData*   usage;
  const WeatherData* weather;
  const MoonData*    moon;
  const AirData*     air;
  const HealthData*  health;
  bool               hasAir;   // a waqi token is configured
};

void uiRender(const UiData& d);
