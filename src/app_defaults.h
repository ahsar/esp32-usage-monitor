#pragma once
// The built-in defaults, in one place.
//
// config.json's loader (config.h::configDefaults) seeds the packed config with
// these; the poller's pre-init fallback timers and the weather client's
// pre-push statics read the same numbers instead of restating them; and
// clock_sync.cpp keys its pre-config timezone the same way. poller.cpp is
// host-tested (test/test_poller builds it without Arduino), which is why this
// is a dependency-free header of its own rather than more facade on config.h.
//
// Change a default here or nowhere — a default that disagrees with itself is
// exactly the kind of drift that shows up as "weather resets its location after
// an unrelated refactor".

#define DEFAULT_PLACE           "Beijing"
#define DEFAULT_LAT             39.9042f
#define DEFAULT_LON             116.4074f
#define DEFAULT_TZ_OFFSET_MIN   480          // UTC+8
#define DEFAULT_WAQI_STATION    "@3303"      // Beijing US Embassy, aqicn.org

#define DEFAULT_POLL_USAGE_S     60          // active usage poll
#define DEFAULT_POLL_USAGE_IDLE_S 180        // usage poll while idle
#define DEFAULT_POLL_WEATHER_S   1800
#define DEFAULT_POLL_AIR_S       3600
#define DEFAULT_POLL_MOON_S      43200
#define DEFAULT_CHIME_VOLUME     3           // index into chime.cpp's amplitude table
