// Host-side unit tests for the dependency-free helpers in src/util.
//
//   pio test -e native
//
// These cover the pure parsing/classification logic that the firmware relies on
// (clock fallback, weather icon selection, AQI labels) without needing hardware.
#include <unity.h>
#include <cstring>

#include "time_util.h"
#include "value_util.h"
#include "interval.h"
#include "dirty_rect.h"
#include "parse/usage_parse.h"
#include "parse/weather_parse.h"

void setUp() {}
void tearDown() {}

// ---- civilToEpoch ---------------------------------------------------------

void test_civil_epoch_unix_origin(void) {
  TEST_ASSERT_EQUAL_UINT32(0u, civilToEpoch(1970, 1, 1, 0, 0, 0));
}

void test_civil_epoch_y2k(void) {
  TEST_ASSERT_EQUAL_UINT32(946684800u, civilToEpoch(2000, 1, 1, 0, 0, 0));
}

void test_civil_epoch_known(void) {
  TEST_ASSERT_EQUAL_UINT32(1791436863u, civilToEpoch(2026, 10, 8, 5, 21, 3));
}

// ---- isoToEpoch -----------------------------------------------------------

void test_iso_to_epoch(void) {
  TEST_ASSERT_EQUAL_UINT32(1791436863u, isoToEpoch("2026-10-08T05:21:03Z"));
}

void test_iso_to_epoch_bad_input(void) {
  TEST_ASSERT_EQUAL_UINT32(0u, isoToEpoch("not-a-date"));
  TEST_ASSERT_EQUAL_UINT32(0u, isoToEpoch(nullptr));
}

// ---- httpDateToEpoch ------------------------------------------------------

void test_http_date_to_epoch(void) {
  TEST_ASSERT_EQUAL_UINT32(1791436863u, httpDateToEpoch("Thu, 08 Oct 2026 05:21:03 GMT"));
}

void test_http_date_to_epoch_bad_input(void) {
  TEST_ASSERT_EQUAL_UINT32(0u, httpDateToEpoch("garbage"));
  TEST_ASSERT_EQUAL_UINT32(0u, httpDateToEpoch(nullptr));
}

// ---- parseHms / parseIsoLocalMinutes --------------------------------------

void test_parse_hms(void) {
  TEST_ASSERT_EQUAL_INT(376, parseHms("06:16:52"));   // 6*60 + 16
  TEST_ASSERT_EQUAL_INT(376, parseHms("06:16"));
  TEST_ASSERT_EQUAL_INT(0,   parseHms("00:00:00"));
}

void test_parse_hms_bad_input(void) {
  TEST_ASSERT_EQUAL_INT(-1, parseHms(""));
  TEST_ASSERT_EQUAL_INT(-1, parseHms(nullptr));
}

void test_parse_iso_local_minutes(void) {
  TEST_ASSERT_EQUAL_INT(207, parseIsoLocalMinutes("2026-10-08T03:27+08:00")); // 3*60 + 27
}

void test_parse_iso_local_minutes_no_time(void) {
  TEST_ASSERT_EQUAL_INT(-1, parseIsoLocalMinutes("2026-10-08"));
}

// ---- kindFromText ---------------------------------------------------------

void test_kind_from_text(void) {
  TEST_ASSERT_EQUAL_INT(0, kindFromText("Sunny"));
  TEST_ASSERT_EQUAL_INT(1, kindFromText("Partly Cloudy"));
  TEST_ASSERT_EQUAL_INT(5, kindFromText("Thunderstorm"));
  TEST_ASSERT_EQUAL_INT(6, kindFromText("Light Drizzle"));
  TEST_ASSERT_EQUAL_INT(7, kindFromText("Heavy Snow"));
}

void test_kind_from_text_null_is_cloudy(void) {
  TEST_ASSERT_EQUAL_INT(2, kindFromText(nullptr));
}

// ---- phaseNameFor ---------------------------------------------------------

void test_phase_name(void) {
  TEST_ASSERT_EQUAL_STRING("New Moon", phaseNameFor(0.0f));
  TEST_ASSERT_EQUAL_STRING("First Quarter", phaseNameFor(0.25f));
  TEST_ASSERT_EQUAL_STRING("Full Moon", phaseNameFor(0.5f));
  TEST_ASSERT_EQUAL_STRING("Waning Crescent", phaseNameFor(0.9f));
}

// ---- aqiCategory ----------------------------------------------------------

void test_aqi_category(void) {
  TEST_ASSERT_EQUAL_STRING("--", aqiCategory(-1));
  TEST_ASSERT_EQUAL_STRING("Good", aqiCategory(25));
  TEST_ASSERT_EQUAL_STRING("Moderate", aqiCategory(75));
  TEST_ASSERT_EQUAL_STRING("Sensitive", aqiCategory(125));
  TEST_ASSERT_EQUAL_STRING("Unhealthy", aqiCategory(175));
  TEST_ASSERT_EQUAL_STRING("Very Unhealthy", aqiCategory(250));
  TEST_ASSERT_EQUAL_STRING("Hazardous", aqiCategory(400));
}

// ---- usageParse -----------------------------------------------------------

static const char* USAGE_JSON =
    "{\"access\":{\"meters\":{"
    "\"fiveHour\":{\"usedMicroCents\":\"2500000\",\"limitMicroCents\":\"10000000\","
    "\"resetsAt\":\"2026-10-08T06:00:00Z\"},"
    "\"week\":{\"usedMicroCents\":\"5000000\",\"limitMicroCents\":\"10000000\"},"
    "\"month\":{\"usedMicroCents\":\"1\",\"limitMicroCents\":\"3\"}}}}";

void test_usage_parse(void) {
  UsageData u;
  uint32_t now = isoToEpoch("2026-10-08T05:00:00Z");
  TEST_ASSERT_TRUE(usageParse(USAGE_JSON, now, u));
  TEST_ASSERT_TRUE(u.valid);
  TEST_ASSERT_EQUAL_INT(25, u.rollingPercent);
  TEST_ASSERT_EQUAL_INT(50, u.weeklyPercent);
  TEST_ASSERT_EQUAL_INT(33, u.monthlyPercent);
  TEST_ASSERT_EQUAL_UINT32(3600u, u.rollingResetSec);   // 06:00 - 05:00
  TEST_ASSERT_EQUAL_UINT32(0u, u.monthlyResetSec);      // no resetsAt
}

void test_usage_parse_clamps_and_zero_limit(void) {
  const char* json =
      "{\"access\":{\"meters\":{"
      "\"fiveHour\":{\"usedMicroCents\":\"999\",\"limitMicroCents\":\"100\"},"
      "\"week\":{\"usedMicroCents\":\"5\",\"limitMicroCents\":\"0\"}}}}";
  UsageData u;
  TEST_ASSERT_TRUE(usageParse(json, 0, u));
  TEST_ASSERT_EQUAL_INT(100, u.rollingPercent);   // clamped
  TEST_ASSERT_EQUAL_INT(0, u.weeklyPercent);      // zero limit -> 0
}

void test_usage_parse_bad_input(void) {
  UsageData u;
  TEST_ASSERT_FALSE(usageParse("not json", 0, u));
  TEST_ASSERT_FALSE(u.valid);
  TEST_ASSERT_FALSE(usageParse("{}", 0, u));
}

// ---- weatherParseLine -----------------------------------------------------

void test_weather_parse_line(void) {
  WeatherData w;
  const char* line =
      "Beijing|Sunny|+15\u00B0C|+14\u00B0C|40|10km/h|0.0mm|06:16|18:02";
  TEST_ASSERT_TRUE(weatherParseLine(line, w));
  TEST_ASSERT_TRUE(w.valid);
  TEST_ASSERT_EQUAL_STRING("Beijing", w.area);
  TEST_ASSERT_EQUAL_STRING("Sunny", w.condition);
  TEST_ASSERT_EQUAL_INT(15, w.tempC);
  TEST_ASSERT_EQUAL_INT(14, w.feelsC);
  TEST_ASSERT_EQUAL_INT(40, w.humidity);
  TEST_ASSERT_EQUAL_STRING("S", w.windDir);   // no arrow -> S
  TEST_ASSERT_EQUAL_INT(10, w.windKmph);
  TEST_ASSERT_EQUAL_INT(0, w.precipTenths);
  TEST_ASSERT_EQUAL_INT(376, w.sunriseMin);
  TEST_ASSERT_EQUAL_INT(1082, w.sunsetMin);
}

void test_weather_parse_too_few_fields(void) {
  WeatherData w;
  TEST_ASSERT_FALSE(weatherParseLine("a|b|c", w));
}

// ---- moonParseJson --------------------------------------------------------

void test_moon_parse(void) {
  MoonData m;
  const char* json =
      "{\"properties\":{"
      "\"moonrise\":{\"time\":\"2026-10-08T03:27+08:00\"},"
      "\"moonset\":{\"time\":\"2026-10-08T16:00+08:00\"},"
      "\"moonphase\":90.0}}";
  TEST_ASSERT_TRUE(moonParseJson(json, m));
  TEST_ASSERT_TRUE(m.valid);
  TEST_ASSERT_EQUAL_INT(207, m.moonriseMin);
  TEST_ASSERT_EQUAL_INT(960, m.moonsetMin);
  TEST_ASSERT_EQUAL_INT(50, m.illumPct);
  TEST_ASSERT_TRUE(m.waxing);
  TEST_ASSERT_EQUAL_STRING("First Quarter", m.phaseName);
}

// ---- airParseJson ---------------------------------------------------------

void test_air_parse(void) {
  AirData a;
  const char* json =
      "{\"status\":\"ok\",\"data\":{\"aqi\":75,"
      "\"iaqi\":{\"pm25\":{\"v\":22},\"pm10\":{\"v\":40}},"
      "\"time\":{\"s\":\"2026-10-08 14:00:00\"}}}";
  TEST_ASSERT_TRUE(airParseJson(json, a));
  TEST_ASSERT_TRUE(a.valid);
  TEST_ASSERT_EQUAL_INT(75, a.aqi);
  TEST_ASSERT_EQUAL_INT(22, a.pm25);
  TEST_ASSERT_EQUAL_INT(40, a.pm10);
  TEST_ASSERT_EQUAL_STRING("14:00", a.observedAt);
}

void test_air_parse_status_not_ok(void) {
  AirData a;
  TEST_ASSERT_FALSE(airParseJson("{\"status\":\"error\"}", a));
}

// ---- Interval -------------------------------------------------------------

void test_interval(void) {
  Interval iv(1000);
  TEST_ASSERT_TRUE(iv.due(0));          // pending on first use
  iv.ran(0);
  TEST_ASSERT_FALSE(iv.due(500));
  TEST_ASSERT_FALSE(iv.due(999));
  TEST_ASSERT_TRUE(iv.due(1000));
  iv.ran(1000);
  TEST_ASSERT_FALSE(iv.due(1999));
  TEST_ASSERT_TRUE(iv.due(2000));
  iv.forcePending();
  TEST_ASSERT_TRUE(iv.due(2000));
}

void test_interval_wrap(void) {
  Interval iv(1000);
  iv.ran(0xFFFFFF00u);                      // close to the uint32 wrap
  TEST_ASSERT_FALSE(iv.due(0xFFFFFF80u));   // 128 ms later
  TEST_ASSERT_TRUE(iv.due(0x00000400u));    // wrapped: 0x500 ms elapsed
}

// ---- dirtyRect16 ----------------------------------------------------------

void test_dirty_rect_identical_is_empty(void) {
  uint16_t a[8 * 4];
  for (int i = 0; i < 8 * 4; i++) a[i] = (uint16_t)i;
  uint16_t b[8 * 4];
  memcpy(b, a, sizeof(a));
  DirtyRect r = dirtyRect16(a, b, 8, 4);
  TEST_ASSERT_TRUE(r.empty());
  TEST_ASSERT_EQUAL_INT(0, r.w());
  TEST_ASSERT_EQUAL_INT(0, r.h());
}

void test_dirty_rect_single_pixel(void) {
  uint16_t a[8 * 4] = {0};
  uint16_t b[8 * 4] = {0};
  a[2 * 8 + 5] = 0x1234;              // (x=5, y=2)
  DirtyRect r = dirtyRect16(a, b, 8, 4);
  TEST_ASSERT_FALSE(r.empty());
  TEST_ASSERT_EQUAL_INT(5, r.x0);
  TEST_ASSERT_EQUAL_INT(2, r.y0);
  TEST_ASSERT_EQUAL_INT(5, r.x1);     // word granularity is folded back to pixels
  TEST_ASSERT_EQUAL_INT(2, r.y1);
  TEST_ASSERT_EQUAL_INT(1, r.w());
  TEST_ASSERT_EQUAL_INT(1, r.h());
}

void test_dirty_rect_pair_in_one_word(void) {
  uint16_t a[8 * 2] = {0};
  uint16_t b[8 * 2] = {0};
  a[0 * 8 + 3] = 1;                   // (3,0) and (4,0) land in adjacent words
  a[0 * 8 + 4] = 2;
  DirtyRect r = dirtyRect16(a, b, 8, 2);
  TEST_ASSERT_EQUAL_INT(3, r.x0);
  TEST_ASSERT_EQUAL_INT(4, r.x1);
  TEST_ASSERT_EQUAL_INT(0, r.y0);
  TEST_ASSERT_EQUAL_INT(0, r.y1);
}

void test_dirty_rect_bounding_box(void) {
  uint16_t a[8 * 5] = {0};
  uint16_t b[8 * 5] = {0};
  a[1 * 8 + 1] = 1;
  a[4 * 8 + 6] = 1;
  DirtyRect r = dirtyRect16(a, b, 8, 5);
  TEST_ASSERT_EQUAL_INT(1, r.x0);
  TEST_ASSERT_EQUAL_INT(1, r.y0);
  TEST_ASSERT_EQUAL_INT(6, r.x1);
  TEST_ASSERT_EQUAL_INT(4, r.y1);
  TEST_ASSERT_EQUAL_INT(6, r.w());
  TEST_ASSERT_EQUAL_INT(4, r.h());
}

void test_dirty_rect_odd_stride_is_full(void) {
  uint16_t a[5 * 3] = {0};
  uint16_t b[5 * 3] = {0};
  DirtyRect r = dirtyRect16(a, b, 5, 3);
  TEST_ASSERT_FALSE(r.empty());
  TEST_ASSERT_EQUAL_INT(0, r.x0);
  TEST_ASSERT_EQUAL_INT(4, r.x1);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_civil_epoch_unix_origin);
  RUN_TEST(test_civil_epoch_y2k);
  RUN_TEST(test_civil_epoch_known);
  RUN_TEST(test_iso_to_epoch);
  RUN_TEST(test_iso_to_epoch_bad_input);
  RUN_TEST(test_http_date_to_epoch);
  RUN_TEST(test_http_date_to_epoch_bad_input);
  RUN_TEST(test_parse_hms);
  RUN_TEST(test_parse_hms_bad_input);
  RUN_TEST(test_parse_iso_local_minutes);
  RUN_TEST(test_parse_iso_local_minutes_no_time);
  RUN_TEST(test_kind_from_text);
  RUN_TEST(test_kind_from_text_null_is_cloudy);
  RUN_TEST(test_phase_name);
  RUN_TEST(test_aqi_category);
  RUN_TEST(test_usage_parse);
  RUN_TEST(test_usage_parse_clamps_and_zero_limit);
  RUN_TEST(test_usage_parse_bad_input);
  RUN_TEST(test_weather_parse_line);
  RUN_TEST(test_weather_parse_too_few_fields);
  RUN_TEST(test_moon_parse);
  RUN_TEST(test_air_parse);
  RUN_TEST(test_air_parse_status_not_ok);
  RUN_TEST(test_interval);
  RUN_TEST(test_interval_wrap);
  RUN_TEST(test_dirty_rect_identical_is_empty);
  RUN_TEST(test_dirty_rect_single_pixel);
  RUN_TEST(test_dirty_rect_pair_in_one_word);
  RUN_TEST(test_dirty_rect_bounding_box);
  RUN_TEST(test_dirty_rect_odd_stride_is_full);
  return UNITY_END();
}
