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
  return UNITY_END();
}
