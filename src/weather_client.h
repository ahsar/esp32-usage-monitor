#pragma once
#include <stdint.h>
#include "data_types.h"   // WeatherData / MoonData / AirData

bool apiFetchWeather(WeatherData& out);
bool apiFetchMoon(MoonData& out);
bool apiFetchAir(AirData& out, const char* token);
