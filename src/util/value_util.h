#pragma once
// Dependency-free classification helpers shared by the UI and the parsers.
// No Arduino headers, so this can be unit-tested on the host.
#include <string.h>

// Map a free-text condition (from wttr.in) to an icon kind:
// 0 clear, 1 partly, 2 cloudy, 3 overcast, 4 fog, 5 thunder,
// 6 rain, 7 snow, 8 sleet
inline int kindFromText(const char* c) {
  if (!c) return 2;
  if (strstr(c, "Thunder")) return 5;
  if (strstr(c, "Blizzard")) return 7;
  if (strstr(c, "Snow") || strstr(c, "snow")) return 7;
  if (strstr(c, "Sleet") || strstr(c, "Ice") || strstr(c, "Freezing")) return 8;
  if (strstr(c, "Rain") || strstr(c, "Drizzle") || strstr(c, "rain")) return 6;
  if (strstr(c, "Fog") || strstr(c, "Mist")) return 4;
  if (strstr(c, "Haze") || strstr(c, "Smoky")) return 4;
  if (strstr(c, "Overcast")) return 3;
  if (strstr(c, "Partly")) return 1;   // must precede "Cloudy": "Partly Cloudy"
  if (strstr(c, "Cloudy")) return 2;
  if (strstr(c, "Sunny") || strstr(c, "Clear")) return 0;
  return 2;
}

// Moon phase [0,1) -> human-readable name.
inline const char* phaseNameFor(float p) {
  if (p < 0.03f || p >= 0.97f) return "New Moon";
  if (p < 0.22f) return "Waxing Crescent";
  if (p < 0.28f) return "First Quarter";
  if (p < 0.47f) return "Waxing Gibbous";
  if (p < 0.53f) return "Full Moon";
  if (p < 0.72f) return "Waning Gibbous";
  if (p < 0.78f) return "Last Quarter";
  return "Waning Crescent";
}

// US EPA AQI -> category label ("--" when unknown).
inline const char* aqiCategory(int aqi) {
  if (aqi < 0) return "--";
  if (aqi <= 50) return "Good";
  if (aqi <= 100) return "Moderate";
  if (aqi <= 150) return "Sensitive";
  if (aqi <= 200) return "Unhealthy";
  if (aqi <= 300) return "Very Unhealthy";
  return "Hazardous";
}
