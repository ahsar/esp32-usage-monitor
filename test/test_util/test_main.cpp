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
#include "util/loc_util.h"
#include "parse/usage_parse.h"
#include "parse/weather_parse.h"
#include "util/trend_util.h"
#include "ui_model.h"

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

// ---- loc_util -------------------------------------------------------------

void test_url_encode(void) {
  char b[64];
  TEST_ASSERT_TRUE(urlEncode("Beijing", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Beijing", b);
  TEST_ASSERT_TRUE(urlEncode("New York", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("New%20York", b);
  TEST_ASSERT_TRUE(urlEncode("a/b?c=1", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("a%2Fb%3Fc%3D1", b);
  TEST_ASSERT_TRUE(urlEncode("", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("", b);
}

void test_url_encode_rejects_overflow(void) {
  char small[4];
  TEST_ASSERT_FALSE(urlEncode("Beijing", small, sizeof(small)));
}

void test_tz_offset_query(void) {
  char b[16];
  TEST_ASSERT_TRUE(formatTzOffsetQuery(480, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("%2B08:00", b);      // '+' must be encoded
  TEST_ASSERT_TRUE(formatTzOffsetQuery(90, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("%2B01:30", b);      // half-hour zone
  TEST_ASSERT_TRUE(formatTzOffsetQuery(0, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("%2B00:00", b);      // UTC is not "-00:00"
  TEST_ASSERT_TRUE(formatTzOffsetQuery(-330, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("-05:30", b);
  TEST_ASSERT_FALSE(formatTzOffsetQuery(480, b, 8));   // buffer too small
}

void test_tz_offset_query_clamps(void) {
  char b[16];
  TEST_ASSERT_TRUE(formatTzOffsetQuery(9999, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("%2B16:00", b);
  TEST_ASSERT_TRUE(formatTzOffsetQuery(-9999, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("-16:00", b);
}

void test_wttr_one_line_path(void) {
  char b[160];
  TEST_ASSERT_TRUE(wttrOneLinePath("Beijing", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("/Beijing?m&format=%l|%C|%t|%f|%h|%w|%p|%S|%s", b);
  TEST_ASSERT_TRUE(wttrOneLinePath("New York", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("/New%20York?m&format=%l|%C|%t|%f|%h|%w|%p|%S|%s", b);
}

void test_wttr_path_rejects_bad_input(void) {
  char b[160];
  TEST_ASSERT_FALSE(wttrOneLinePath("", b, sizeof(b)));
  TEST_ASSERT_FALSE(wttrOneLinePath(nullptr, b, sizeof(b)));
  char small[16];
  TEST_ASSERT_FALSE(wttrOneLinePath("Beijing", small, sizeof(small)));
}

void test_wttr_path_worst_case_place(void) {
  // The largest place AppConfig allows (place[48] -> 47 chars) with every
  // character needing escaping is the worst case the path buffers must hold.
  char place[48];
  for (int i = 0; i < 47; i++) place[i] = ' ';
  place[47] = '\0';

  char b[192];
  TEST_ASSERT_TRUE(wttrOneLinePath(place, b, sizeof(b)));
  TEST_ASSERT_EQUAL_INT(178, (int)strlen(b));   // 1 + 47*3 + 10 + 26
  TEST_ASSERT_EQUAL_INT(0, strncmp(b, "/%20%20%20", 10));
  TEST_ASSERT_EQUAL_STRING("?m&format=%l|%C|%t|%f|%h|%w|%p|%S|%s", b + 142);
}

void test_moon_api_path(void) {
  char b[160];
  TEST_ASSERT_TRUE(moonApiPath(39.9042, 116.4074, 480, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING(
      "/weatherapi/sunrise/3.0/moon?lat=39.9042&lon=116.4074&offset=%2B08:00", b);
  TEST_ASSERT_TRUE(moonApiPath(40.7128, -74.0060, -300, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING(
      "/weatherapi/sunrise/3.0/moon?lat=40.7128&lon=-74.0060&offset=-05:00", b);
  char small[16];
  TEST_ASSERT_FALSE(moonApiPath(0, 0, 0, small, sizeof(small)));
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

void test_usage_parse_truncated_json(void) {
  UsageData u;
  // A network read cut short mid-body must be rejected, not half-applied.
  TEST_ASSERT_FALSE(usageParse("{\"access\":{\"meters\":{\"fiveHour\":", 0, u));
  TEST_ASSERT_FALSE(u.valid);
}

void test_usage_parse_meters_not_object(void) {
  UsageData u;
  TEST_ASSERT_FALSE(usageParse("{\"access\":{\"meters\":[1,2]}}", 0, u));
  TEST_ASSERT_FALSE(u.valid);
}

void test_usage_parse_numeric_micros(void) {
  // The console sometimes sends the micro-cent fields as JSON numbers rather
  // than strings; both must decode.
  UsageData u;
  const char* json =
      "{\"access\":{\"meters\":{"
      "\"fiveHour\":{\"usedMicroCents\":2500000,\"limitMicroCents\":10000000},"
      "\"week\":{\"usedMicroCents\":1.0e7,\"limitMicroCents\":1.0e7}}}}";
  TEST_ASSERT_TRUE(usageParse(json, 0, u));
  TEST_ASSERT_EQUAL_INT(25, u.rollingPercent);
  TEST_ASSERT_EQUAL_INT(100, u.weeklyPercent);
}

void test_usage_parse_reset_in_past_or_unset_clock(void) {
  const char* json =
      "{\"access\":{\"meters\":{"
      "\"fiveHour\":{\"usedMicroCents\":\"1\",\"limitMicroCents\":\"10\","
      "\"resetsAt\":\"2026-10-08T06:00:00Z\"}}}}";
  UsageData u;
  TEST_ASSERT_TRUE(usageParse(json, isoToEpoch("2026-10-08T07:00:00Z"), u));
  TEST_ASSERT_EQUAL_UINT32(0u, u.rollingResetSec);   // reset already passed
  TEST_ASSERT_TRUE(usageParse(json, 0, u));          // clock unset
  TEST_ASSERT_EQUAL_UINT32(0u, u.rollingResetSec);
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

void test_weather_parse_no_input(void) {
  WeatherData w;
  TEST_ASSERT_FALSE(weatherParseLine("", w));
  TEST_ASSERT_FALSE(weatherParseLine(nullptr, w));
}

void test_weather_parse_extra_fields_ignored(void) {
  WeatherData w;
  const char* line =
      "Beijing|Sunny|+15\u00B0C|+14\u00B0C|40|10km/h|0.0mm|06:16|18:02|extra";
  TEST_ASSERT_TRUE(weatherParseLine(line, w));
  TEST_ASSERT_EQUAL_STRING("Beijing", w.area);
  TEST_ASSERT_EQUAL_INT(1082, w.sunsetMin);   // untouched by the trailing field
}

void test_weather_parse_fields_are_trimmed(void) {
  WeatherData w;
  const char* line =
      "  Beijing  |  Sunny  |+15\u00B0C|+14\u00B0C|40|10km/h|0.0mm|06:16|18:02";
  TEST_ASSERT_TRUE(weatherParseLine(line, w));
  TEST_ASSERT_EQUAL_STRING("Beijing", w.area);
  TEST_ASSERT_EQUAL_STRING("Sunny", w.condition);
}

void test_weather_parse_long_area_is_truncated(void) {
  WeatherData w;
  const char* line =
      "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDD|Sunny|+15\u00B0C|+14\u00B0C|40|"
      "10km/h|0.0mm|06:16|18:02";
  TEST_ASSERT_TRUE(weatherParseLine(line, w));
  TEST_ASSERT_EQUAL_INT(31, (int)strlen(w.area));   // fits area[32]
  TEST_ASSERT_EQUAL_INT(0, w.area[31]);             // NUL-terminated
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

void test_moon_parse_truncated_and_missing_properties(void) {
  MoonData m;
  TEST_ASSERT_FALSE(moonParseJson("{\"properties\":{\"moonrise\":", m));
  TEST_ASSERT_FALSE(moonParseJson("{}", m));
}

void test_moon_parse_empty_properties_defaults(void) {
  MoonData m;
  TEST_ASSERT_TRUE(moonParseJson("{\"properties\":{}}", m));
  TEST_ASSERT_TRUE(m.valid);
  TEST_ASSERT_EQUAL_INT(-1, m.moonriseMin);   // no times reported
  TEST_ASSERT_EQUAL_INT(-1, m.moonsetMin);
  TEST_ASSERT_EQUAL_INT(0, m.illumPct);       // phase 0 -> new moon
  TEST_ASSERT_TRUE(m.waxing);
  TEST_ASSERT_EQUAL_STRING("New Moon", m.phaseName);
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

void test_air_parse_truncated_and_missing_status(void) {
  AirData a;
  TEST_ASSERT_FALSE(airParseJson("{\"status\":\"ok\",\"data\":", a));
  TEST_ASSERT_FALSE(airParseJson("{}", a));
}

void test_air_parse_missing_data_defaults(void) {
  AirData a;
  TEST_ASSERT_TRUE(airParseJson("{\"status\":\"ok\"}", a));
  TEST_ASSERT_TRUE(a.valid);
  TEST_ASSERT_EQUAL_INT(-1, a.aqi);       // unknowns stay -1
  TEST_ASSERT_EQUAL_INT(-1, a.pm25);
  TEST_ASSERT_EQUAL_INT(-1, a.pm10);
  TEST_ASSERT_EQUAL_STRING("", a.observedAt);
}

void test_air_parse_string_values(void) {
  AirData a;
  const char* json =
      "{\"status\":\"ok\",\"data\":{\"aqi\":\"75\","
      "\"iaqi\":{\"pm25\":{\"v\":\"22\"},\"pm10\":{\"v\":null}},"
      "\"time\":{\"s\":\"2026-10-08 14:00:00\"}}}";
  TEST_ASSERT_TRUE(airParseJson(json, a));
  TEST_ASSERT_EQUAL_INT(75, a.aqi);       // numeric string
  TEST_ASSERT_EQUAL_INT(22, a.pm25);      // numeric string
  TEST_ASSERT_EQUAL_INT(-1, a.pm10);      // null -> unknown
  TEST_ASSERT_EQUAL_STRING("14:00", a.observedAt);
}

void test_air_parse_bad_aqi_and_short_time(void) {
  AirData a;
  const char* json =
      "{\"status\":\"ok\",\"data\":{\"aqi\":\"n/a\",\"time\":{\"s\":\"2026\"}}}";
  TEST_ASSERT_TRUE(airParseJson(json, a));
  TEST_ASSERT_EQUAL_INT(-1, a.aqi);       // non-numeric -> unknown
  TEST_ASSERT_EQUAL_STRING("", a.observedAt);
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

// ---- trend_util -----------------------------------------------------------

void test_trend_format_usd(void) {
  char b[24];
  trend::formatUsd(b, sizeof(b), 0);                // $0.00
  TEST_ASSERT_EQUAL_STRING("$0.00", b);
  trend::formatUsd(b, sizeof(b), 142000000ULL);     // $1.42
  TEST_ASSERT_EQUAL_STRING("$1.42", b);
  trend::formatUsd(b, sizeof(b), 12345678900ULL);   // $123.45
  TEST_ASSERT_EQUAL_STRING("$123.45", b);
}

void test_trend_burn_and_eta(void) {
  // $1.00 spent over 2 h = $0.50/h = 50_000_000 microcents/h.
  TEST_ASSERT_EQUAL_UINT64(50000000ULL, trend::burnPerHour(100000000ULL, 2 * 3600));
  TEST_ASSERT_EQUAL_UINT64(0ULL, trend::burnPerHour(100000000ULL, 0));

  // At $0.50/h, $1.00 of headroom is 2 h away.
  TEST_ASSERT_EQUAL_UINT32(7200u, trend::etaSeconds(100000000ULL, 50000000ULL));
  TEST_ASSERT_EQUAL_UINT32(0u, trend::etaSeconds(100000000ULL, 0));   // unknown rate
}

void test_trend_format_eta(void) {
  char b[16];
  trend::formatEta(b, sizeof(b), 0);
  TEST_ASSERT_EQUAL_STRING("--", b);
  trend::formatEta(b, sizeof(b), 45 * 60);
  TEST_ASSERT_EQUAL_STRING("45m", b);
  trend::formatEta(b, sizeof(b), 2 * 3600 + 10 * 60);
  TEST_ASSERT_EQUAL_STRING("2h 10m", b);
  trend::formatEta(b, sizeof(b), 3 * 86400 + 4 * 3600);
  TEST_ASSERT_EQUAL_STRING("3d 4h", b);
}

void test_usage_parse_micro_cents(void) {
  const char* json =
      "{\"access\":{\"meters\":{"
      "\"fiveHour\":{\"usedMicroCents\":\"142000000\",\"limitMicroCents\":\"1000000000\"}}}}";
  UsageData u;
  TEST_ASSERT_TRUE(usageParse(json, 0, u));
  TEST_ASSERT_EQUAL_UINT64(142000000ULL, u.rollingUsedMicroCents);
  TEST_ASSERT_EQUAL_UINT64(1000000000ULL, u.rollingLimitMicroCents);
  TEST_ASSERT_EQUAL_UINT64(0ULL, u.weeklyUsedMicroCents);   // absent meter stays 0
}

// ---- ui_model wrap helpers ------------------------------------------------

void test_ui_model_page_wrap(void) {
  // Endless carousel: stepping past either end lands on the far end, and the
  // pager's commit animation + ui_compose's neighbour tile both use these, so
  // they must agree with each other.
  TEST_ASSERT_EQUAL_INT(1, uiPageNext(0));
  TEST_ASSERT_EQUAL_INT(0, uiPageNext(UI_PAGE_COUNT - 1));
  TEST_ASSERT_EQUAL_INT(UI_PAGE_COUNT - 1, uiPagePrev(0));
  TEST_ASSERT_EQUAL_INT(0, uiPagePrev(1));
}

void test_ui_model_card_wrap(void) {
  TEST_ASSERT_EQUAL_INT(1, uiCardNext(0));
  TEST_ASSERT_EQUAL_INT(0, uiCardNext(UI_WC_COUNT - 1));
  TEST_ASSERT_EQUAL_INT(UI_WC_COUNT - 1, uiCardPrev(0));
  TEST_ASSERT_EQUAL_INT(0, uiCardPrev(1));
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
  RUN_TEST(test_url_encode);
  RUN_TEST(test_url_encode_rejects_overflow);
  RUN_TEST(test_tz_offset_query);
  RUN_TEST(test_tz_offset_query_clamps);
  RUN_TEST(test_wttr_one_line_path);
  RUN_TEST(test_wttr_path_rejects_bad_input);
  RUN_TEST(test_wttr_path_worst_case_place);
  RUN_TEST(test_moon_api_path);
  RUN_TEST(test_usage_parse);
  RUN_TEST(test_usage_parse_clamps_and_zero_limit);
  RUN_TEST(test_usage_parse_bad_input);
  RUN_TEST(test_usage_parse_truncated_json);
  RUN_TEST(test_usage_parse_meters_not_object);
  RUN_TEST(test_usage_parse_numeric_micros);
  RUN_TEST(test_usage_parse_reset_in_past_or_unset_clock);
  RUN_TEST(test_weather_parse_line);
  RUN_TEST(test_weather_parse_too_few_fields);
  RUN_TEST(test_weather_parse_no_input);
  RUN_TEST(test_weather_parse_extra_fields_ignored);
  RUN_TEST(test_weather_parse_fields_are_trimmed);
  RUN_TEST(test_weather_parse_long_area_is_truncated);
  RUN_TEST(test_moon_parse);
  RUN_TEST(test_moon_parse_truncated_and_missing_properties);
  RUN_TEST(test_moon_parse_empty_properties_defaults);
  RUN_TEST(test_air_parse);
  RUN_TEST(test_air_parse_status_not_ok);
  RUN_TEST(test_air_parse_truncated_and_missing_status);
  RUN_TEST(test_air_parse_missing_data_defaults);
  RUN_TEST(test_air_parse_string_values);
  RUN_TEST(test_air_parse_bad_aqi_and_short_time);
  RUN_TEST(test_interval);
  RUN_TEST(test_interval_wrap);
  RUN_TEST(test_trend_format_usd);
  RUN_TEST(test_trend_burn_and_eta);
  RUN_TEST(test_trend_format_eta);
  RUN_TEST(test_usage_parse_micro_cents);
  RUN_TEST(test_ui_model_page_wrap);
  RUN_TEST(test_ui_model_card_wrap);
  return UNITY_END();
}
