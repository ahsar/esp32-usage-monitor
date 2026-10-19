#pragma once
// Numeric limits for the values config.json and /settings.json feed the same
// fields. Dependency-free (no Arduino, no ArduinoJson), so the host tests can
// cover both entry points with the same table.
//
// Why one table: the settings channel has always range-checked what it accepts,
// while config.json took whatever the JSON said. Two ways into the same fields
// with different rules is how `"lat": 999` ends up in the moon calculation and
// `"poll_usage_s": 0` turns the fetch queue into a busy loop — so both paths ask
// these predicates instead.
namespace config_limits {

// Coordinates: the real-world range. A typo (or a swapped lat/lon) is rejected
// rather than silently moving the device's weather and moon.
constexpr double LAT_MIN = -90.0;
constexpr double LAT_MAX = 90.0;
constexpr double LON_MIN = -180.0;
constexpr double LON_MAX = 180.0;

// Minutes east of UTC. ±16 h covers every real zone including the +14 h line.
// clock_sync.cpp clamps to the same value; here it is a validation failure.
constexpr int TZ_MIN = -16 * 60;
constexpr int TZ_MAX = 16 * 60;

// Poll periods (seconds). The floors matter as much as the ceilings: a zero
// period makes Interval::due() true on every iteration, and although only one
// fetch is in flight at a time, that still means "poll back to back forever".
// The usage meter keeps its own, faster floor; the others are allowed to be
// slow but not instantaneous.
constexpr unsigned POLL_USAGE_MIN_S = 10;
constexpr unsigned POLL_USAGE_MAX_S = 86400;      // 1 day
constexpr unsigned POLL_OTHER_MIN_S = 60;
constexpr unsigned POLL_OTHER_MAX_S = 604800;     // 7 days

inline bool latOk(double v) { return v >= LAT_MIN && v <= LAT_MAX; }
inline bool lonOk(double v) { return v >= LON_MIN && v <= LON_MAX; }
inline bool tzOk(double v) { return v >= TZ_MIN && v <= TZ_MAX; }
inline bool usagePollOk(double v) {
  return v >= POLL_USAGE_MIN_S && v <= POLL_USAGE_MAX_S;
}
inline bool otherPollOk(double v) {
  return v >= POLL_OTHER_MIN_S && v <= POLL_OTHER_MAX_S;
}

}  // namespace config_limits
