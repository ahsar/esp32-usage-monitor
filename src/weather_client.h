#pragma once
#include <stdint.h>
#include "data_types.h"   // WeatherData / MoonData / AirData

bool apiFetchWeather(WeatherData& out);
bool apiFetchMoon(MoonData& out);
bool apiFetchAir(AirData& out, const char* token);

// Set the location every weather/moon/air request is built from. Call once after
// loadConfig(); a place or station id of "" keeps the previous (default) value,
// so a config without a location block behaves as before. A place that cannot be
// encoded (or does not fit the path buffer) is reported as a "bad place" log line
// at fetch time rather than being sent as a wrong URL.
void weatherSetLocation(const char* place, float lat, float lon, int tzOffsetMin,
                        const char* waqiStation);
