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
#include "util/url_util.h"
#include "parse/settings_parse.h"
#include "parse/wifi_cred_parse.h"
#include "parse/usage_parse.h"
#include "parse/weather_parse.h"
#include "parse/http_request.h"
#include "parse/ble_frame.h"
#include "parse/status_machine.h"
#include "parse/agent_parse.h"
#include "util/config_limits.h"
#include "util/trend_util.h"
#include "util/usage_pace.h"
#include "util/usage_alert.h"
#include "util/health_util.h"
#include "util/trend_store_util.h"
#include "parse/notice_parse.h"
#include "parse/agent_sessions.h"
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

void test_url_decode(void) {
  char b[64];
  TEST_ASSERT_TRUE(urlDecode("Beijing", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Beijing", b);
  TEST_ASSERT_TRUE(urlDecode("New%20York", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("New York", b);
  TEST_ASSERT_TRUE(urlDecode("New+York", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("New York", b);      // '+' is the query-string space
  TEST_ASSERT_TRUE(urlDecode("%2B08:00", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("+08:00", b);
  TEST_ASSERT_TRUE(urlDecode("a%2", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("a%2", b);           // malformed escape passes through
  char small[8];
  TEST_ASSERT_FALSE(urlDecode("aaaaaaaaaaaaaaaa", small, sizeof(small)));
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

// ---- settings_parse -------------------------------------------------------

// Parse `query` into a fresh set and return whether it was accepted.
static bool settingsQuery(const char* query, DeviceSettings& out, uint16_t& changed,
                          char* err = nullptr, size_t errCap = 0) {
  settingsClear(out);
  changed = SET_NONE;
  return settingsFromQuery(query, out, changed, err, errCap);
}

void test_settings_query_single(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  TEST_ASSERT_TRUE(settingsQuery("chime=0", s, changed));
  TEST_ASSERT_EQUAL_HEX16(SET_CHIME, changed);
  TEST_ASSERT_EQUAL_HEX16(SET_CHIME, s.set);
  TEST_ASSERT_EQUAL_INT(0, s.chime);
  TEST_ASSERT_EQUAL_INT(0, s.set & SET_BRIGHTNESS);   // untouched stays clear
}

void test_settings_query_multi_and_encoding(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  TEST_ASSERT_TRUE(settingsQuery("chime=1&brightness=4&place=New%20York", s, changed));
  TEST_ASSERT_EQUAL_HEX16(SET_CHIME | SET_BRIGHTNESS | SET_PLACE, changed);
  TEST_ASSERT_EQUAL_INT(4, s.brightness);
  TEST_ASSERT_EQUAL_STRING("New York", s.place);

  TEST_ASSERT_TRUE(settingsQuery("place=New+York", s, changed));
  TEST_ASSERT_EQUAL_STRING("New York", s.place);
}

void test_settings_query_volume(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  TEST_ASSERT_TRUE(settingsQuery("volume=0", s, changed));
  TEST_ASSERT_EQUAL_HEX16(SET_VOLUME, changed);
  TEST_ASSERT_EQUAL_INT(0, s.volume);
  TEST_ASSERT_TRUE(settingsQuery("volume=5", s, changed));
  TEST_ASSERT_EQUAL_INT(5, s.volume);

  char err[96];
  TEST_ASSERT_FALSE(settingsQuery("volume=6", s, changed, err, sizeof(err)));
  TEST_ASSERT_FALSE(settingsQuery("volume=-1", s, changed, err, sizeof(err)));
  TEST_ASSERT_FALSE(settingsQuery("volume=2x", s, changed, err, sizeof(err)));

  // Empty value clears it, like every other field.
  TEST_ASSERT_TRUE(settingsQuery("volume=3", s, changed));
  TEST_ASSERT_TRUE(settingsFromQuery("volume=", s, changed, nullptr, 0));
  TEST_ASSERT_EQUAL_HEX16(SET_VOLUME, changed);
  TEST_ASSERT_EQUAL_HEX16(SET_NONE, s.set);
}

void test_settings_query_location_group(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  TEST_ASSERT_TRUE(settingsQuery(
      "place=Shanghai&lat=31.2304&lon=121.4737&tz_offset_min=480&waqi_station=@1437"
      "&air_label=Shanghai%20US", s, changed));
  TEST_ASSERT_EQUAL_HEX16(SET_PLACE | SET_COORDS | SET_TZ | SET_STATION | SET_AIR_LABEL,
                          changed);
  TEST_ASSERT_EQUAL_STRING("Shanghai", s.place);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 31.2304f, s.lat);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 121.4737f, s.lon);
  TEST_ASSERT_EQUAL_INT(480, s.tz_offset_min);
  TEST_ASSERT_EQUAL_STRING("@1437", s.waqi_station);
  TEST_ASSERT_EQUAL_STRING("Shanghai US", s.air_label);

  // A negative offset and negative coordinates are fine too.
  TEST_ASSERT_TRUE(settingsQuery("lat=40.7128&lon=-74.0060&tz_offset_min=-300", s, changed));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -74.0060f, s.lon);
  TEST_ASSERT_EQUAL_INT(-300, s.tz_offset_min);
}

void test_settings_query_empty_value_clears(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  TEST_ASSERT_TRUE(settingsQuery("place=Shanghai&lat=31&lon=121&chime=0", s, changed));
  TEST_ASSERT_EQUAL_HEX16(SET_PLACE | SET_COORDS | SET_CHIME, s.set);

  // Clearing reports the field as touched, so the caller re-derives it from
  // config.json rather than keeping the override.
  TEST_ASSERT_TRUE(settingsFromQuery("place=&chime=", s, changed, nullptr, 0));
  TEST_ASSERT_EQUAL_HEX16(SET_PLACE | SET_CHIME, changed);
  TEST_ASSERT_EQUAL_HEX16(SET_COORDS, s.set);        // coords untouched
  TEST_ASSERT_EQUAL_STRING("", s.place);
}

void test_settings_query_rejects_unknown_key(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  char err[96] = {0};
  TEST_ASSERT_FALSE(settingsQuery("chime=1&brightnes=4", s, changed, err, sizeof(err)));
  TEST_ASSERT_TRUE(err[0] != 0);
  TEST_ASSERT_EQUAL_HEX16(SET_NONE, s.set);          // nothing half-applied
}

void test_settings_query_rejects_out_of_range(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  char err[96];
  TEST_ASSERT_FALSE(settingsQuery("brightness=0", s, changed, err, sizeof(err)));
  TEST_ASSERT_FALSE(settingsQuery("brightness=5", s, changed, err, sizeof(err)));
  TEST_ASSERT_TRUE(settingsQuery("brightness=", s, changed, err, sizeof(err)));   // clear
  TEST_ASSERT_EQUAL_HEX16(SET_NONE, s.set);
  TEST_ASSERT_TRUE(settingsQuery("brightness=1", s, changed, err, sizeof(err)));  // in range
  TEST_ASSERT_FALSE(settingsQuery("chime=2", s, changed, err, sizeof(err)));
  TEST_ASSERT_FALSE(settingsQuery("tz_offset_min=961", s, changed, err, sizeof(err)));
  TEST_ASSERT_TRUE(settingsQuery("tz_offset_min=960", s, changed, err, sizeof(err)));
  TEST_ASSERT_FALSE(settingsQuery("lat=91&lon=0", s, changed, err, sizeof(err)));
  TEST_ASSERT_FALSE(settingsQuery("lat=0&lon=181", s, changed, err, sizeof(err)));
  // A rejected query never leaves a partial override behind.
  TEST_ASSERT_EQUAL_HEX16(SET_NONE, s.set);
}

void test_settings_query_rejects_junk_numbers(void) {
  // strtol()/strtof() stop at the first bad character; "4x" must not become 4.
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  char err[96];
  TEST_ASSERT_FALSE(settingsQuery("brightness=4x", s, changed, err, sizeof(err)));
  TEST_ASSERT_FALSE(settingsQuery("lat=abc&lon=1", s, changed, err, sizeof(err)));
  TEST_ASSERT_FALSE(settingsQuery("lat=12abc&lon=1", s, changed, err, sizeof(err)));
  TEST_ASSERT_FALSE(settingsQuery("tz_offset_min=+8:00", s, changed, err, sizeof(err)));
}

void test_settings_query_rejects_half_a_location(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  char err[96];
  TEST_ASSERT_FALSE(settingsQuery("lat=31.23", s, changed, err, sizeof(err)));
  TEST_ASSERT_FALSE(settingsQuery("lon=121.47", s, changed, err, sizeof(err)));
  TEST_ASSERT_EQUAL_HEX16(SET_NONE, s.set);
}

void test_settings_query_rejects_malformed(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  char err[96];
  TEST_ASSERT_FALSE(settingsQuery("chime", s, changed, err, sizeof(err)));      // no '='
  TEST_ASSERT_FALSE(settingsQuery("settings", s, changed, err, sizeof(err)));   // read-only token
  TEST_ASSERT_FALSE(settingsQuery("", s, changed, err, sizeof(err)));
}

void test_settings_query_rejects_long_values(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  char err[96];
  char q[128];
  // place[48] holds 47 characters plus the NUL.
  snprintf(q, sizeof(q), "place=%s", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); // 48
  TEST_ASSERT_FALSE(settingsQuery(q, s, changed, err, sizeof(err)));
  snprintf(q, sizeof(q), "place=%s", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");  // 47
  TEST_ASSERT_TRUE(settingsQuery(q, s, changed, err, sizeof(err)));
}

void test_settings_query_all_fields_at_once(void) {
  // The transport accepts a 255-byte query; the longest legitimate one must parse
  // (and fit the per-pair buffer) rather than being rejected as malformed.
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  char err[96];
  char q[256];
  snprintf(q, sizeof(q),
           "chime=1&brightness=4&volume=5&place=%s&lat=-89.9999&lon=-179.9999"
           "&tz_offset_min=-960&waqi_station=%s&air_label=%s",
           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
           "@1234567890123456789012",
           "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  TEST_ASSERT_TRUE(settingsQuery(q, s, changed, err, sizeof(err)));
  TEST_ASSERT_EQUAL_HEX16(SET_CHIME | SET_BRIGHTNESS | SET_VOLUME | SET_PLACE | SET_COORDS |
                          SET_TZ | SET_STATION | SET_AIR_LABEL, changed);
  TEST_ASSERT_EQUAL_INT(47, (int)strlen(s.place));
}

void test_settings_json_round_trip(void) {
  DeviceSettings s, back;
  uint16_t changed = SET_NONE;
  TEST_ASSERT_TRUE(settingsQuery(
      "chime=0&brightness=4&volume=2&place=Shanghai&lat=31.2304&lon=121.4737"
      "&tz_offset_min=480&waqi_station=@1437&air_label=Shanghai%20US", s, changed));

  char json[512];
  TEST_ASSERT_TRUE(settingsToJson(s, json, sizeof(json)));
  TEST_ASSERT_TRUE(strstr(json, "\"place\":\"Shanghai\"") != nullptr);
  TEST_ASSERT_TRUE(strstr(json, "\"chime\":0") != nullptr);

  settingsClear(back);
  uint16_t changedBack = SET_NONE;
  TEST_ASSERT_TRUE(settingsFromJson(json, back, changedBack));
  TEST_ASSERT_EQUAL_HEX16(s.set, back.set);
  TEST_ASSERT_EQUAL_INT(s.chime, back.chime);
  TEST_ASSERT_EQUAL_INT(s.brightness, back.brightness);
  TEST_ASSERT_EQUAL_INT(s.volume, back.volume);
  TEST_ASSERT_EQUAL_STRING(s.place, back.place);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, s.lat, back.lat);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, s.lon, back.lon);
  TEST_ASSERT_EQUAL_INT(s.tz_offset_min, back.tz_offset_min);
  TEST_ASSERT_EQUAL_STRING(s.waqi_station, back.waqi_station);
  TEST_ASSERT_EQUAL_STRING(s.air_label, back.air_label);
}

void test_settings_json_empty_and_small_buffer(void) {
  DeviceSettings s;
  settingsClear(s);
  char json[64];
  TEST_ASSERT_TRUE(settingsToJson(s, json, sizeof(json)));
  TEST_ASSERT_EQUAL_STRING("{}", json);

  char small[8];
  DeviceSettings full;
  uint16_t changed = SET_NONE;
  TEST_ASSERT_TRUE(settingsQuery("place=Shanghai&lat=31&lon=121&brightness=3",
                                 full, changed));
  TEST_ASSERT_FALSE(settingsToJson(full, small, sizeof(small)));
}

void test_settings_json_rejects_bad_documents(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  char json[512];
  settingsClear(s);

  TEST_ASSERT_FALSE(settingsFromJson("not json", s, changed));
  TEST_ASSERT_FALSE(settingsFromJson("[1,2]", s, changed));            // not an object
  TEST_ASSERT_FALSE(settingsFromJson("{\"brightness\":9}", s, changed));
  TEST_ASSERT_FALSE(settingsFromJson("{\"volume\":6}", s, changed));
  TEST_ASSERT_FALSE(settingsFromJson("{\"chime\":\"maybe\"}", s, changed));
  TEST_ASSERT_FALSE(settingsFromJson("{\"lat\":31.2}", s, changed));   // half a location
  TEST_ASSERT_EQUAL_HEX16(SET_NONE, s.set);

  // Unknown keys are ignored (forward compatibility), the known ones still load.
  settingsClear(s);
  TEST_ASSERT_TRUE(settingsFromJson("{\"future\":1,\"place\":\"Oslo\"}", s, changed));
  TEST_ASSERT_EQUAL_HEX16(SET_PLACE, s.set);
  TEST_ASSERT_EQUAL_STRING("Oslo", s.place);

  TEST_ASSERT_TRUE(settingsFromJson("{\"chime\":true,\"brightness\":2,\"volume\":1}", s, changed));
  TEST_ASSERT_EQUAL_INT(1, s.chime);
  TEST_ASSERT_EQUAL_INT(2, s.brightness);
  TEST_ASSERT_EQUAL_INT(1, s.volume);
}

void test_settings_describe(void) {
  DeviceSettings s;
  char out[512];
  settingsClear(s);
  settingsDescribe(s, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("no overrides (everything follows config.json)\n", out);

  uint16_t changed = SET_NONE;
  TEST_ASSERT_TRUE(settingsQuery("chime=0&volume=2&place=Shanghai&tz_offset_min=480", s, changed));
  settingsDescribe(s, out, sizeof(out));
  TEST_ASSERT_TRUE(strstr(out, "chime          = off") != nullptr);
  TEST_ASSERT_TRUE(strstr(out, "volume         = 2") != nullptr);
  TEST_ASSERT_TRUE(strstr(out, "place          = Shanghai") != nullptr);
  TEST_ASSERT_TRUE(strstr(out, "tz_offset_min  = 480") != nullptr);
  TEST_ASSERT_TRUE(strstr(out, "brightness") == nullptr);   // unset: not listed
}

// ---- wifi_cred_parse ------------------------------------------------------

// Parse into a fresh credential and return whether it was accepted.
static bool wifiQuery(const char* query, WifiCred& out, char* err = nullptr,
                      size_t errCap = 0) {
  out = WifiCred();
  return wifiCredFromQuery(query, out, err, errCap);
}

void test_wifi_cred_set(void) {
  WifiCred c;
  TEST_ASSERT_TRUE(wifiQuery("wifi_ssid=Home&wifi_pass=hunter2", c));
  TEST_ASSERT_EQUAL_INT(WIFI_CRED_SET, c.action);
  TEST_ASSERT_EQUAL_STRING("Home", c.ssid);
  TEST_ASSERT_EQUAL_STRING("hunter2", c.pass);

  // Key order is not part of the protocol.
  TEST_ASSERT_TRUE(wifiQuery("wifi_pass=hunter2&wifi_ssid=Home", c));
  TEST_ASSERT_EQUAL_STRING("Home", c.ssid);
  TEST_ASSERT_EQUAL_STRING("hunter2", c.pass);
}

void test_wifi_cred_encoding(void) {
  WifiCred c;
  TEST_ASSERT_TRUE(wifiQuery("wifi_ssid=My%20Net&wifi_pass=a%26b%3Dc%2Bd", c));
  TEST_ASSERT_EQUAL_STRING("My Net", c.ssid);
  TEST_ASSERT_EQUAL_STRING("a&b=c+d", c.pass);

  // '+' is the query-string spelling of a space (urlDecode), so a literal plus
  // in a passphrase has to be sent as %2B — the client's job, not the device's.
  TEST_ASSERT_TRUE(wifiQuery("wifi_ssid=Guest+Net&wifi_pass=two+words", c));
  TEST_ASSERT_EQUAL_STRING("Guest Net", c.ssid);
  TEST_ASSERT_EQUAL_STRING("two words", c.pass);
}

void test_wifi_cred_open_network_and_forget(void) {
  WifiCred c;
  // A named open network: a passphrase is optional, an empty one is not an error.
  TEST_ASSERT_TRUE(wifiQuery("wifi_ssid=Cafe&wifi_pass=", c));
  TEST_ASSERT_EQUAL_INT(WIFI_CRED_SET, c.action);
  TEST_ASSERT_EQUAL_STRING("Cafe", c.ssid);
  TEST_ASSERT_EQUAL_STRING("", c.pass);

  // Both empty: forget. The only way back from a network that no longer exists.
  TEST_ASSERT_TRUE(wifiQuery("wifi_ssid=&wifi_pass=", c));
  TEST_ASSERT_EQUAL_INT(WIFI_CRED_FORGET, c.action);
  TEST_ASSERT_EQUAL_STRING("", c.ssid);
  TEST_ASSERT_EQUAL_STRING("", c.pass);
}

void test_wifi_cred_rejects(void) {
  WifiCred c;
  char err[96];
  // Both halves are required: a name without a passphrase would silently keep the
  // old network's passphrase, which is exactly the kind of half-configured state
  // the all-or-nothing rule exists to prevent.
  TEST_ASSERT_FALSE(wifiQuery("wifi_ssid=Home", c, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("wifi_ssid and wifi_pass must be given together", err);
  TEST_ASSERT_EQUAL_INT(WIFI_CRED_NONE, c.action);
  TEST_ASSERT_FALSE(wifiQuery("wifi_pass=hunter2", c, err, sizeof(err)));
  TEST_ASSERT_EQUAL_INT(WIFI_CRED_NONE, c.action);
  TEST_ASSERT_FALSE(wifiQuery("wifi_ssid=&wifi_pass=hunter2", c, err, sizeof(err)));
  TEST_ASSERT_FALSE(wifiQuery("wifi_ssid=Home&wifi_pass=hunter2&chime=0", c, err,
                              sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("unknown wifi key", err);
  TEST_ASSERT_FALSE(wifiQuery("wifi_ssid=Home&wifi_sid=Home", c, err, sizeof(err)));
  TEST_ASSERT_FALSE(wifiQuery("wifi_ssidathome", c, err, sizeof(err)));
  TEST_ASSERT_FALSE(wifiQuery("", c, err, sizeof(err)));
  TEST_ASSERT_FALSE(wifiQuery(nullptr, c, err, sizeof(err)));
}

void test_wifi_cred_rejects_long_values(void) {
  // 33 characters (the 802.11 SSID maximum is 32) and 64 (the WPA2 passphrase
  // maximum is 63). A rejected request must leave the caller's struct alone.
  char ssid[64], pass[128], query[384], err[96];
  memset(ssid, 's', sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = 0;
  memset(pass, 'p', sizeof(pass) - 1);
  pass[sizeof(pass) - 1] = 0;
  snprintf(query, sizeof(query), "wifi_ssid=%s&wifi_pass=%s", ssid, pass);
  WifiCred c;
  TEST_ASSERT_FALSE(wifiQuery(query, c, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("wifi_ssid too long (max 32 characters)", err);
  TEST_ASSERT_EQUAL_INT(WIFI_CRED_NONE, c.action);

  char ssid32[33];
  memset(ssid32, 's', sizeof(ssid32) - 1);
  ssid32[sizeof(ssid32) - 1] = 0;
  snprintf(query, sizeof(query), "wifi_ssid=%s&wifi_pass=%s", ssid32, pass);
  TEST_ASSERT_FALSE(wifiQuery(query, c, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("wifi_pass too long (max 63 characters)", err);

  // The maxima themselves are accepted.
  char pass63[64];
  memset(pass63, 'p', sizeof(pass63) - 1);
  pass63[sizeof(pass63) - 1] = 0;
  snprintf(query, sizeof(query), "wifi_ssid=%s&wifi_pass=%s", ssid32, pass63);
  TEST_ASSERT_TRUE(wifiQuery(query, c));
  TEST_ASSERT_EQUAL_INT(32, (int)strlen(c.ssid));
  TEST_ASSERT_EQUAL_INT(63, (int)strlen(c.pass));
}

void test_wifi_cred_describe_redacts(void) {
  WifiCred c;
  char out[256];
  TEST_ASSERT_TRUE(wifiQuery("wifi_ssid=Home&wifi_pass=hunter2", c));
  wifiCredDescribe(c, out, sizeof(out));
  TEST_ASSERT_TRUE(strstr(out, "Home") != nullptr);
  TEST_ASSERT_TRUE(strstr(out, "7 chars") != nullptr);
  TEST_ASSERT_TRUE(strstr(out, "hunter2") == nullptr);   // never echoed back

  // …and the same for the transports' own log line.
  char safe[64];
  TEST_ASSERT_EQUAL_STRING("wifi_ssid=<credentials, passphrase redacted>",
                           wifiQueryForLog("wifi_ssid=Home&wifi_pass=hunter2", safe,
                                           sizeof(safe)));
  TEST_ASSERT_TRUE(strstr(wifiQueryForLog("chime=0&volume=2", safe, sizeof(safe)),
                          "chime=0") != nullptr);

  TEST_ASSERT_TRUE(wifiQuery("wifi_ssid=&wifi_pass=", c));
  wifiCredDescribe(c, out, sizeof(out));
  TEST_ASSERT_TRUE(strstr(out, "cleared") != nullptr);
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

void test_usage_parse_negative_micros(void) {
  // Negative micro-cents are a server bug, not an unsigned wrap: "-5" through
  // strtoull is ~1.8e19, which once parsed into an astronomic percent. Signed
  // input (string or JSON number) reads as 0.
  UsageData u;
  const char* json =
      "{\"access\":{\"meters\":{"
      "\"fiveHour\":{\"usedMicroCents\":\"-5\",\"limitMicroCents\":\"10000000\"},"
      "\"week\":{\"usedMicroCents\":-5,\"limitMicroCents\":\"10000000\"}}}}";
  TEST_ASSERT_TRUE(usageParse(json, 0, u));
  TEST_ASSERT_EQUAL_UINT64(0ULL, u.rollingUsedMicroCents);
  TEST_ASSERT_EQUAL_UINT64(0ULL, u.weeklyUsedMicroCents);
  TEST_ASSERT_EQUAL_INT(0, u.rollingPercent);
  TEST_ASSERT_EQUAL_INT(0, u.weeklyPercent);
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

// ---- usage pace -----------------------------------------------------------

void test_usage_pace_verdict(void) {
  // $1.00 of a $10.00 limit, $2.00/h, 1 h to reset → projected $3.00, on track.
  TEST_ASSERT_EQUAL_INT(usage_pace::PACE_ON_TRACK,
                        usage_pace::verdict(100000000ULL, 1000000000ULL, 200000000ULL, 3600));
  // Same meter at $10.00/h → projected $11.00, over.
  TEST_ASSERT_EQUAL_INT(usage_pace::PACE_OVER,
                        usage_pace::verdict(100000000ULL, 1000000000ULL, 1000000000ULL, 3600));
  // A zero rate is "not measured yet", never "safe".
  TEST_ASSERT_EQUAL_INT(usage_pace::PACE_UNKNOWN,
                        usage_pace::verdict(100000000ULL, 1000000000ULL, 0, 3600));
  // A meter with no limit gets no verdict.
  TEST_ASSERT_EQUAL_INT(usage_pace::PACE_UNKNOWN,
                        usage_pace::verdict(100000000ULL, 0, 200000000ULL, 3600));
  // Resetting right now: nothing to project.
  TEST_ASSERT_EQUAL_INT(usage_pace::PACE_UNKNOWN,
                        usage_pace::verdict(100000000ULL, 1000000000ULL, 200000000ULL, 0));
}

void test_usage_pace_projection(void) {
  // $1.00 + ($2.00/h × 1800 s) = $2.00.
  TEST_ASSERT_EQUAL_UINT64(200000000ULL,
      usage_pace::projectedAtReset(100000000ULL, 200000000ULL, 1800));
  // No burn → the projection is just what is already spent.
  TEST_ASSERT_EQUAL_UINT64(100000000ULL,
      usage_pace::projectedAtReset(100000000ULL, 0, 7200));
}

// ---- usage alert thresholds ----------------------------------------------

void test_usage_alert_crossings(void) {
  usage_alert::State s;
  // Booting into a 96% meter is not a crossing — and no cue at every power-on.
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_NONE, usage_alert::tick(s, 96, 960));
  // Neither is staying there.
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_NONE, usage_alert::tick(s, 97, 970));

  usage_alert::State t;
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_NONE,   usage_alert::tick(t, 10, 100));
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_NONE,   usage_alert::tick(t, 79, 790));
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_WARN,   usage_alert::tick(t, 80, 800));
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_NONE,   usage_alert::tick(t, 85, 850));  // once
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_FULL,   usage_alert::tick(t, 100, 1000));
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_NONE,   usage_alert::tick(t, 100, 1000)); // once
}

void test_usage_alert_jump_is_one_cue(void) {
  usage_alert::State s;
  usage_alert::tick(s, 10, 100);
  // 70 → 100 in one poll must not queue the 80% cue as well.
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_FULL, usage_alert::tick(s, 100, 1000));
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_NONE, usage_alert::tick(s, 100, 1000));
}

void test_usage_alert_rearms_after_reset(void) {
  usage_alert::State s;
  usage_alert::tick(s, 10, 100);
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_WARN, usage_alert::tick(s, 80, 800));
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_FULL, usage_alert::tick(s, 100, 1000));
  // The window rolls over: spend drops, both thresholds re-arm.
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_NONE, usage_alert::tick(s, 2, 20));
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_WARN, usage_alert::tick(s, 81, 810));
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_FULL, usage_alert::tick(s, 100, 1000));
}

void test_usage_alert_reset_helper(void) {
  usage_alert::State s;
  usage_alert::tick(s, 10, 100);
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_WARN, usage_alert::tick(s, 90, 900));
  usage_alert::reset(s);
  TEST_ASSERT_FALSE(s.seen);
  // A fresh seed, so a re-authorised device does not beep for an old crossing.
  TEST_ASSERT_EQUAL_INT(usage_alert::EV_NONE, usage_alert::tick(s, 90, 900));
}

// ---- health formatting ----------------------------------------------------

void test_health_format_uptime(void) {
  char b[16];
  health_fmt::formatUptime(b, sizeof(b), 0);
  TEST_ASSERT_EQUAL_STRING("0m", b);
  health_fmt::formatUptime(b, sizeof(b), 42 * 60);
  TEST_ASSERT_EQUAL_STRING("42m", b);
  health_fmt::formatUptime(b, sizeof(b), 4 * 3600 + 2 * 60);
  TEST_ASSERT_EQUAL_STRING("4h 02m", b);
  health_fmt::formatUptime(b, sizeof(b), 3 * 86400 + 4 * 3600 + 59 * 60);
  TEST_ASSERT_EQUAL_STRING("3d 04h", b);
}

void test_health_format_bytes(void) {
  char b[16];
  health_fmt::formatBytes(b, sizeof(b), 512);
  TEST_ASSERT_EQUAL_STRING("512B", b);
  health_fmt::formatBytes(b, sizeof(b), 4096);
  TEST_ASSERT_EQUAL_STRING("4K", b);
  health_fmt::formatBytes(b, sizeof(b), 1024u * 1024u + 400u * 1024u);
  TEST_ASSERT_EQUAL_STRING("1.3M", b);
}

void test_health_format_age(void) {
  char b[16];
  health_fmt::formatAge(b, sizeof(b), 5);
  TEST_ASSERT_EQUAL_STRING("5s", b);
  health_fmt::formatAge(b, sizeof(b), 3 * 60 + 20);
  TEST_ASSERT_EQUAL_STRING("3m", b);
  health_fmt::formatAge(b, sizeof(b), 2 * 3600 + 30);
  TEST_ASSERT_EQUAL_STRING("2h", b);
  health_fmt::formatAge(b, sizeof(b), 86400 * 9);
  TEST_ASSERT_EQUAL_STRING("9d", b);
}

void test_health_format_ms(void) {
  char b[16];
  health_fmt::formatMs(b, sizeof(b), 0);
  TEST_ASSERT_EQUAL_STRING("0ms", b);
  health_fmt::formatMs(b, sizeof(b), 820);
  TEST_ASSERT_EQUAL_STRING("820ms", b);
  health_fmt::formatMs(b, sizeof(b), 1840);
  TEST_ASSERT_EQUAL_STRING("1.8s", b);
}

// ---- agent context --------------------------------------------------------

void test_agent_parse_full(void) {
  AgentRequest r;
  char err[96] = {0};
  TEST_ASSERT_TRUE(agentParse("agent=1&state=waiting&repo=esp32-usage-monitor"
                              "&tool=bash&note=Allow%20bash%20to%20run",
                              r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("waiting", r.state);
  TEST_ASSERT_EQUAL_STRING("esp32-usage-monitor", r.info.repo);
  TEST_ASSERT_EQUAL_STRING("bash", r.info.tool);
  TEST_ASSERT_EQUAL_STRING("Allow bash to run", r.info.note);   // %20 and '+'
}

void test_agent_parse_plus_is_space(void) {
  AgentRequest r;
  char err[96] = {0};
  TEST_ASSERT_TRUE(agentParse("agent=1&state=running&note=rm+-rf+./build", r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("rm -rf ./build", r.info.note);
}

void test_agent_parse_optional_fields(void) {
  // Context is best-effort: state alone is a valid request, and the other
  // fields come back empty rather than as garbage.
  AgentRequest r;
  char err[96] = {0};
  TEST_ASSERT_TRUE(agentParse("agent=1&state=running", r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("running", r.state);
  TEST_ASSERT_EQUAL_STRING("", r.info.repo);
  TEST_ASSERT_EQUAL_STRING("", r.info.tool);
  TEST_ASSERT_EQUAL_STRING("", r.info.note);
}

void test_agent_parse_drops_control_bytes(void) {
  // A model-written note must not be able to put a control byte into a frame.
  AgentRequest r;
  char err[96] = {0};
  TEST_ASSERT_TRUE(agentParse("agent=1&state=running&note=a%0Ab%7Fc%01d", r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("abcd", r.info.note);
}

void test_agent_parse_ignores_unknown_keys(void) {
  // Forward compatibility on purpose: nothing is persisted, so a newer client
  // may add a field. (The settings channel is strict because a typo there would
  // half-configure the device.)
  AgentRequest r;
  char err[96] = {0};
  TEST_ASSERT_TRUE(agentParse("agent=1&state=done&model=gpt&future=1", r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("done", r.state);
}

void test_agent_parse_rejects(void) {
  AgentRequest r;
  char err[96];

  // Unknown state: the machine would ignore it, so the client gets told.
  TEST_ASSERT_FALSE(agentParse("agent=1&state=thinking", r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("unknown agent state", err);

  // State is mandatory.
  TEST_ASSERT_FALSE(agentParse("agent=1&repo=x", r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("missing state", err);

  // Not the agent kind at all.
  TEST_ASSERT_FALSE(agentParse("chime=0", r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("not an agent request", err);

  TEST_ASSERT_FALSE(agentParse("", r, err, sizeof(err)));
  TEST_ASSERT_FALSE(agentParse("agent=1&state", r, err, sizeof(err)));

  // Over-long fields are refused rather than truncated: the client owns the
  // clip (the plugin does), so a too-long value is a client bug worth a 400.
  char big[400];
  int n = snprintf(big, sizeof(big), "agent=1&state=running&repo=");
  for (int i = 0; i < 40 && n < (int)sizeof(big) - 2; i++) big[n++] = 'x';
  big[n] = 0;
  TEST_ASSERT_FALSE(agentParse(big, r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("repo too long", err);

  // …and the largest *legitimate* request is inside the transport cap, since a
  // fully percent-encoded value is three bytes per character.
  char worst[512];
  n = snprintf(worst, sizeof(worst), "agent=1&state=running&repo=");
  for (int i = 0; i < AGENT_REPO_CAP - 1; i++) n += snprintf(worst + n, sizeof(worst) - n, "%s", "%41");
  n += snprintf(worst + n, sizeof(worst) - n, "&tool=");
  for (int i = 0; i < AGENT_TOOL_CAP - 1; i++) n += snprintf(worst + n, sizeof(worst) - n, "%s", "%41");
  n += snprintf(worst + n, sizeof(worst) - n, "&note=");
  for (int i = 0; i < AGENT_NOTE_CAP - 1; i++) n += snprintf(worst + n, sizeof(worst) - n, "%s", "%41");
  TEST_ASSERT_TRUE(agentParse(worst, r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("AAAAAAAAAAAAAAAAAAAAAAAAAAA", r.info.repo);
  TEST_ASSERT_LESS_THAN_UINT(384, (unsigned)strlen(worst));   // STATUS_QUERY_MAX
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

// ---- http_request (the request head parser both transports rely on) --------

static const char* kHead =
    "GET /?chime=0&place=New+York HTTP/1.1\r\n"
    "Host: usage-monitor.local\r\n"
    "X-Ts: 1791436863\r\n"
    "x-siG:   deadbeef\r\n"
    "\r\n";

void test_http_query_from_head(void) {
  char q[128] = {0};
  bool tooLong = true;
  TEST_ASSERT_TRUE(httpQueryFromHead(kHead, strlen(kHead), q, sizeof(q), &tooLong));
  TEST_ASSERT_EQUAL_STRING("chime=0&place=New+York", q);
  TEST_ASSERT_FALSE(tooLong);

  // A health probe (no '?') is not an error — it just has no query.
  const char* probe = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  TEST_ASSERT_FALSE(httpQueryFromHead(probe, strlen(probe), q, sizeof(q), &tooLong));
  TEST_ASSERT_FALSE(tooLong);

  // A query that does not fit is reported separately from "no query": the HTTP
  // transport answers 414 instead of the health-probe body.
  char tiny[8];
  TEST_ASSERT_FALSE(httpQueryFromHead(kHead, strlen(kHead), tiny, sizeof(tiny), &tooLong));
  TEST_ASSERT_TRUE(tooLong);

  // A '?' after the request target (in a header) must not be mistaken for one.
  const char* headerQ = "GET /x HTTP/1.1\r\nReferer: /?not=a-query\r\n\r\n";
  TEST_ASSERT_FALSE(httpQueryFromHead(headerQ, strlen(headerQ), q, sizeof(q)));

  // Not a GET: the transport decides that before calling this, but the parser
  // must not invent a query out of some other method.
  const char* post = "POST /?a=1 HTTP/1.1\r\n\r\n";
  TEST_ASSERT_FALSE(httpQueryFromHead(post, strlen(post), q, sizeof(q)));
}

void test_http_header_value(void) {
  char out[32] = {0};
  TEST_ASSERT_TRUE(httpHeaderValue(kHead, strlen(kHead), "X-Ts", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("1791436863", out);

  // Case-insensitive, and whitespace after the colon is skipped.
  TEST_ASSERT_TRUE(httpHeaderValue(kHead, strlen(kHead), "x-SIG", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("deadbeef", out);

  TEST_ASSERT_FALSE(httpHeaderValue(kHead, strlen(kHead), "X-Missing", out, sizeof(out)));

  // A value longer than the buffer is truncated, never overrun.
  char small[5];
  TEST_ASSERT_TRUE(httpHeaderValue(kHead, strlen(kHead), "X-Ts", small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("1791", small);
}

// ---- ble_frame (the BLE transport's "<ts> <sig> <query>" framing) ----------

void test_ble_frame_parse(void) {
  char ts[24] = {0}, sig[80] = {0};
  char ok[] = "1791436863 abcdef0123 chime=0&place=New+York";
  const char* q = bleFrameParse(ok, ts, sizeof(ts), sig, sizeof(sig));
  TEST_ASSERT_NOT_NULL(q);
  TEST_ASSERT_EQUAL_STRING("1791436863", ts);
  TEST_ASSERT_EQUAL_STRING("abcdef0123", sig);
  TEST_ASSERT_EQUAL_STRING("chime=0&place=New+York", q);

  // The query may contain spaces; only the first two separate fields.
  char spaced[] = "1 ab state=done&place=New York";
  q = bleFrameParse(spaced, ts, sizeof(ts), sig, sizeof(sig));
  TEST_ASSERT_NOT_NULL(q);
  TEST_ASSERT_EQUAL_STRING("state=done&place=New York", q);

  char missing[] = "1onlyonefield";
  TEST_ASSERT_NULL(bleFrameParse(missing, ts, sizeof(ts), sig, sizeof(sig)));

  char noQuery[] = "1 ab ";
  TEST_ASSERT_NULL(bleFrameParse(noQuery, ts, sizeof(ts), sig, sizeof(sig)));

  char noSig[] = "1  q";
  TEST_ASSERT_NULL(bleFrameParse(noSig, ts, sizeof(ts), sig, sizeof(sig)));

  char longTs[] = "1234567890123456789012345 ab q";
  char tinyTs[8];
  TEST_ASSERT_NULL(bleFrameParse(longTs, tinyTs, sizeof(tinyTs), sig, sizeof(sig)));
}

// ---- status_machine (the state machine status_channel.cpp drives) ----------

void test_status_machine_transitions(void) {
  StatusMachine m;

  // A start wakes the panel and seeds the animation clock.
  TEST_ASSERT_TRUE(statusMachineApply(m, "running", 100));
  TEST_ASSERT_EQUAL_INT(STATUS_RUNNING, m.state);
  TEST_ASSERT_EQUAL_UINT32(100, m.since);
  TEST_ASSERT_EQUAL_INT(STATUS_EV_NONE, m.pending);
  TEST_ASSERT_TRUE(statusMachineThinking(m));

  // A duplicate is not a transition: no wake, and the clock is not restarted.
  TEST_ASSERT_FALSE(statusMachineApply(m, "running", 200));
  TEST_ASSERT_EQUAL_UINT32(100, m.since);

  // Permission asked → needs-you animation, one waiting cue.
  TEST_ASSERT_TRUE(statusMachineApply(m, "waiting", 300));
  TEST_ASSERT_TRUE(statusMachineWaiting(m));
  TEST_ASSERT_EQUAL_INT(STATUS_EV_WAITING, m.pending);

  // Still waiting: no second cue, no wake.
  TEST_ASSERT_FALSE(statusMachineApply(m, "waiting", 400));
  TEST_ASSERT_EQUAL_INT(STATUS_EV_WAITING, m.pending);

  // A completion from an active state queues exactly one event and goes idle.
  TEST_ASSERT_FALSE(statusMachineApply(m, "done", 500));
  TEST_ASSERT_EQUAL_INT(STATUS_IDLE, m.state);
  TEST_ASSERT_EQUAL_INT(STATUS_EV_DONE, m.pending);
  TEST_ASSERT_FALSE(statusMachineThinking(m));

  // The event is consumed once, by loop().
  TEST_ASSERT_EQUAL_INT(STATUS_EV_DONE, statusMachineTakeEvent(m));
  TEST_ASSERT_EQUAL_INT(STATUS_EV_NONE, statusMachineTakeEvent(m));

  // A duplicate report while idle must not chime.
  TEST_ASSERT_FALSE(statusMachineApply(m, "done", 600));
  TEST_ASSERT_EQUAL_INT(STATUS_EV_NONE, m.pending);

  // Failure after a real start is a failure cue, not a success one.
  TEST_ASSERT_TRUE(statusMachineApply(m, "running", 700));
  TEST_ASSERT_FALSE(statusMachineApply(m, "failed", 800));
  TEST_ASSERT_EQUAL_INT(STATUS_EV_FAILED, m.pending);

  // Key1's dismiss is silent and leaves any pending event alone (the failure
  // cue from the step above is still queued).
  TEST_ASSERT_TRUE(statusMachineApply(m, "running", 900));
  statusMachineClear(m);
  TEST_ASSERT_EQUAL_INT(STATUS_IDLE, m.state);
  TEST_ASSERT_EQUAL_INT(STATUS_EV_FAILED, m.pending);
  statusMachineTakeEvent(m);
  TEST_ASSERT_EQUAL_INT(STATUS_EV_NONE, m.pending);

  // Unknown/absent state names are no-ops.
  TEST_ASSERT_FALSE(statusMachineApply(m, "nonsense", 1000));
  TEST_ASSERT_FALSE(statusMachineApply(m, nullptr, 1000));
}

// ---- config_limits (the shared config.json / settings range table) ---------

void test_config_limits(void) {
  TEST_ASSERT_TRUE(config_limits::latOk(-90));
  TEST_ASSERT_TRUE(config_limits::latOk(39.9042));
  TEST_ASSERT_TRUE(config_limits::latOk(90));
  TEST_ASSERT_FALSE(config_limits::latOk(90.1));
  TEST_ASSERT_FALSE(config_limits::latOk(-91));

  TEST_ASSERT_TRUE(config_limits::lonOk(-180));
  TEST_ASSERT_TRUE(config_limits::lonOk(180));
  TEST_ASSERT_FALSE(config_limits::lonOk(181));

  TEST_ASSERT_TRUE(config_limits::tzOk(-960));
  TEST_ASSERT_TRUE(config_limits::tzOk(480));
  TEST_ASSERT_TRUE(config_limits::tzOk(960));
  TEST_ASSERT_FALSE(config_limits::tzOk(961));

  // Zero is the one that matters: Interval(0) is due on every iteration.
  TEST_ASSERT_FALSE(config_limits::usagePollOk(0));
  TEST_ASSERT_FALSE(config_limits::usagePollOk(9));
  TEST_ASSERT_TRUE(config_limits::usagePollOk(10));
  TEST_ASSERT_TRUE(config_limits::usagePollOk(86400));
  TEST_ASSERT_FALSE(config_limits::usagePollOk(86401));

  TEST_ASSERT_FALSE(config_limits::otherPollOk(0));
  TEST_ASSERT_FALSE(config_limits::otherPollOk(59));
  TEST_ASSERT_TRUE(config_limits::otherPollOk(60));
  TEST_ASSERT_TRUE(config_limits::otherPollOk(604800));
  TEST_ASSERT_FALSE(config_limits::otherPollOk(604801));
}

void test_settings_json_rejects_non_numeric_location(void) {
  DeviceSettings s;
  uint16_t changed = SET_NONE;
  settingsClear(s);

  // as<float>() would turn each of these into 0, which is a valid coordinate
  // and a valid offset — the exact silent misconfiguration this rejects.
  TEST_ASSERT_FALSE(settingsFromJson("{\"lat\":\"31.2\",\"lon\":121.4}", s, changed));
  TEST_ASSERT_FALSE(settingsFromJson("{\"lat\":31.2,\"lon\":\"121.4\"}", s, changed));
  TEST_ASSERT_FALSE(settingsFromJson("{\"tz_offset_min\":\"480\"}", s, changed));
  TEST_ASSERT_EQUAL_HEX16(SET_NONE, s.set);

  // Numeric forms still load: integers and floats are both JSON numbers.
  TEST_ASSERT_TRUE(settingsFromJson(
      "{\"lat\":31,\"lon\":121.4,\"tz_offset_min\":480}", s, changed));
  TEST_ASSERT_EQUAL_HEX16(SET_COORDS | SET_TZ, changed);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 31.0f, s.lat);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 121.4f, s.lon);
  TEST_ASSERT_EQUAL_INT(480, s.tz_offset_min);
}

// ---- even-burn pace, notices, sessions, forecast, trend file --------------

void test_usage_pace_even(void) {
  using namespace usage_pace;
  // A week: one day in = 14% elapsed. Ten points of slack puts the line at 24%.
  const uint32_t reset = WINDOW_WEEK_SEC - 86400;
  TEST_ASSERT_EQUAL_INT(14, elapsedPct(WINDOW_WEEK_SEC, reset));
  TEST_ASSERT_EQUAL_INT(PACE_ON_TRACK, evenVerdict(20, WINDOW_WEEK_SEC, reset));
  TEST_ASSERT_EQUAL_INT(PACE_ON_TRACK, evenVerdict(24, WINDOW_WEEK_SEC, reset));
  TEST_ASSERT_EQUAL_INT(PACE_OVER,     evenVerdict(25, WINDOW_WEEK_SEC, reset));
  TEST_ASSERT_EQUAL_INT(PACE_OVER,     evenVerdict(30, WINDOW_WEEK_SEC, reset));

  // Resetting right now: the percentage may still be the old window's.
  TEST_ASSERT_EQUAL_INT(PACE_UNKNOWN, evenVerdict(90, WINDOW_WEEK_SEC, 0));
  // A countdown longer than the window means the two disagree; not negative.
  TEST_ASSERT_EQUAL_INT(0, elapsedPct(WINDOW_WEEK_SEC, WINDOW_WEEK_SEC + 100));
  TEST_ASSERT_EQUAL_INT(PACE_UNKNOWN,
                        evenVerdict(90, WINDOW_WEEK_SEC, WINDOW_WEEK_SEC + 100));
  // Fresh window: any burst beyond the slack is over-pace, and a small one is not.
  TEST_ASSERT_EQUAL_INT(PACE_OVER,     evenVerdict(11, WINDOW_MONTH_SEC, WINDOW_MONTH_SEC));
  TEST_ASSERT_EQUAL_INT(PACE_ON_TRACK, evenVerdict(5,  WINDOW_MONTH_SEC, WINDOW_MONTH_SEC));
  // …and a negative percentage is clamped, not flipped into "over".
  TEST_ASSERT_EQUAL_INT(PACE_ON_TRACK, evenVerdict(-5, WINDOW_MONTH_SEC, WINDOW_MONTH_SEC));
}

void test_weekday_from_iso(void) {
  TEST_ASSERT_EQUAL_INT(4, weekdayFromIsoDate("2026-10-08"));   // Thursday
  TEST_ASSERT_EQUAL_INT(2, weekdayFromIsoDate("2026-10-20"));   // Tuesday
  TEST_ASSERT_EQUAL_INT(0, weekdayFromIsoDate("2026-10-18"));   // Sunday
  TEST_ASSERT_EQUAL_INT(-1, weekdayFromIsoDate("nope"));
  TEST_ASSERT_EQUAL_INT(-1, weekdayFromIsoDate(nullptr));
}

void test_notice_parse_full(void) {
  NoticeRequest r;
  char err[96] = {0};
  TEST_ASSERT_TRUE(noticeParse(
      "notice=1&kind=meeting&text=Standup%20in%208%20min&ttl=900&left=480",
      r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("meeting", r.kind);
  TEST_ASSERT_EQUAL_STRING("Standup in 8 min", r.text);
  TEST_ASSERT_EQUAL_UINT32(900, r.ttlSec);
  TEST_ASSERT_EQUAL_UINT32(480, r.leftSec);
  TEST_ASSERT_TRUE(r.hasLeft);
}

void test_notice_parse_defaults_and_unknown_keys(void) {
  NoticeRequest r;
  char err[96] = {0};
  // No kind → "note"; no left → no countdown; unknown keys ignored so a newer
  // client can add a field without a firmware update.
  TEST_ASSERT_TRUE(noticeParse("notice=1&text=hello&ttl=60&future=x", r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("note", r.kind);
  TEST_ASSERT_EQUAL_STRING("hello", r.text);
  TEST_ASSERT_FALSE(r.hasLeft);
}

void test_notice_parse_clear(void) {
  NoticeRequest r;
  char err[96] = {0};
  // Clearing needs neither text nor TTL: there is nothing left to expire.
  TEST_ASSERT_TRUE(noticeParse("notice=1&clear=1", r, err, sizeof(err)));
  TEST_ASSERT_TRUE(r.clear);

  TEST_ASSERT_TRUE(noticeParse("notice=1&clear=on&text=ignored", r, err, sizeof(err)));
  TEST_ASSERT_TRUE(r.clear);
  TEST_ASSERT_FALSE(noticeParse("notice=1&clear=maybe", r, err, sizeof(err)));
}

void test_notice_parse_strips_control_bytes(void) {
  NoticeRequest r;
  char err[96] = {0};
  // Same rule as the agent text: refusing the whole notice over one stray byte
  // would be worse than dropping the byte.
  TEST_ASSERT_TRUE(noticeParse("notice=1&text=a%0Ab%7Fc&ttl=60", r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("abc", r.text);
}

void test_notice_parse_rejects(void) {
  NoticeRequest r;
  char err[96] = {0};
  TEST_ASSERT_FALSE(noticeParse(nullptr, r, err, sizeof(err)));
  TEST_ASSERT_FALSE(noticeParse("", r, err, sizeof(err)));
  TEST_ASSERT_FALSE(noticeParse("text=hi&ttl=60", r, err, sizeof(err)));       // no marker
  TEST_ASSERT_FALSE(noticeParse("notice=1&ttl=60", r, err, sizeof(err)));      // no text
  TEST_ASSERT_FALSE(noticeParse("notice=1&text=hi", r, err, sizeof(err)));     // no ttl
  TEST_ASSERT_FALSE(noticeParse("notice=1&text=hi&ttl=0", r, err, sizeof(err)));
  TEST_ASSERT_FALSE(noticeParse("notice=1&text=hi&ttl=86401", r, err, sizeof(err)));
  TEST_ASSERT_FALSE(noticeParse("notice=1&text=hi&ttl=60x", r, err, sizeof(err)));
  TEST_ASSERT_FALSE(noticeParse("notice=1&text=hi&ttl=-1", r, err, sizeof(err)));
  TEST_ASSERT_FALSE(noticeParse("notice=1&text=hi&ttl=60&left=9999999", r, err, sizeof(err)));
  TEST_ASSERT_FALSE(noticeParse("notice=1&text=&ttl=60", r, err, sizeof(err)));
}

void test_notice_parse_length_budget(void) {
  // The point of the caps: a fully percent-encoded request still fits the 384
  // bytes both transports accept. 63 text chars + 11 kind chars is the worst
  // case, so fill them with characters that encode to three bytes each.
  char kind[12], text[64], encKind[64], encText[256], q[512];
  memset(kind, '/', 11); kind[11] = '\0';
  memset(text, ' ', 63); text[63] = '\0';
  TEST_ASSERT_TRUE(urlEncode(kind, encKind, sizeof(encKind)));
  TEST_ASSERT_TRUE(urlEncode(text, encText, sizeof(encText)));
  snprintf(q, sizeof(q), "notice=1&kind=%s&text=%s&ttl=86400&left=604800", encKind, encText);
  TEST_ASSERT_LESS_THAN_UINT32(384, strlen(q));
  NoticeRequest r;
  char err[96] = {0};
  TEST_ASSERT_TRUE(noticeParse(q, r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_UINT32(11, strlen(r.kind));
}

void test_agent_parse_session(void) {
  AgentRequest r;
  char err[96] = {0};
  TEST_ASSERT_TRUE(agentParse("agent=1&state=running&session=s12&repo=x", r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("s12", r.session);
  TEST_ASSERT_EQUAL_STRING("x", r.info.repo);

  // 7 characters is the cap (8 with the NUL): the budget has no room for more.
  AgentRequest s;
  TEST_ASSERT_TRUE(agentParse("agent=1&state=running&session=1234567", s, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("1234567", s.session);
  AgentRequest t;
  TEST_ASSERT_FALSE(agentParse("agent=1&state=running&session=12345678", t, err, sizeof(err)));
}

void test_agent_parse_length_budget(void) {
  // The session field is what made this budget tight: every character costs
  // three bytes when percent-encoded, and the whole request has to stay inside
  // STATUS_QUERY_MAX (384) or it cannot be signed / sent over BLE at all.
  char repo[28], tool[20], note[52], q[512], enc[1024];
  memset(repo, ' ', 27); repo[27] = '\0';
  memset(tool, ' ', 19); tool[19] = '\0';
  memset(note, ' ', 51); note[51] = '\0';

  char eRepo[128], eTool[96], eNote[256];
  TEST_ASSERT_TRUE(urlEncode(repo, eRepo, sizeof(eRepo)));
  TEST_ASSERT_TRUE(urlEncode(tool, eTool, sizeof(eTool)));
  TEST_ASSERT_TRUE(urlEncode(note, eNote, sizeof(eNote)));
  snprintf(q, sizeof(q), "agent=1&state=running&session=1234567&repo=%s&tool=%s&note=%s",
           eRepo, eTool, eNote);
  TEST_ASSERT_LESS_THAN_UINT32(384, strlen(q));

  AgentRequest r;
  char err[96] = {0};
  TEST_ASSERT_TRUE(agentParse(q, r, err, sizeof(err)));
  TEST_ASSERT_EQUAL_STRING("1234567", r.session);
  TEST_ASSERT_EQUAL_UINT32(27, strlen(r.info.repo));
}

void test_agent_sessions_aggregate(void) {
  AgentTable t;
  agentSessionsClear(t);
  AgentInfo a, b;
  agentInfoClear(a); strcpy(a.repo, "alpha");
  agentInfoClear(b); strcpy(b.repo, "beta");

  TEST_ASSERT_NULL(agentSessionsStateName(t));
  TEST_ASSERT_TRUE(agentSessionsApply(t, "s1", "running", a, 1000));
  TEST_ASSERT_EQUAL_STRING("running", agentSessionsStateName(t));
  TEST_ASSERT_TRUE(agentSessionsApply(t, "s2", "running", b, 2000));
  TEST_ASSERT_EQUAL_INT(2, agentSessionsActive(t));
  // The most recently heard from owns the context on the thinking screen.
  TEST_ASSERT_EQUAL_STRING("beta", agentSessionsInfo(t)->repo);

  // Waiting wins over running, and the session blocked longest owns it.
  TEST_ASSERT_TRUE(agentSessionsApply(t, "s1", "waiting", a, 3000));
  TEST_ASSERT_EQUAL_STRING("waiting", agentSessionsStateName(t));
  TEST_ASSERT_EQUAL_STRING("alpha", agentSessionsInfo(t)->repo);
  TEST_ASSERT_EQUAL_UINT32(3000, agentSessionsSince(t, 9000));

  // One window finishing leaves the other alone — the bug this table exists for.
  TEST_ASSERT_TRUE(agentSessionsApply(t, "s1", "done", AgentInfo{}, 4000));
  TEST_ASSERT_EQUAL_STRING("running", agentSessionsStateName(t));
  TEST_ASSERT_EQUAL_STRING("beta", agentSessionsInfo(t)->repo);

  TEST_ASSERT_TRUE(agentSessionsApply(t, "s2", "failed", AgentInfo{}, 5000));
  TEST_ASSERT_NULL(agentSessionsStateName(t));
  TEST_ASSERT_TRUE(t.lastEndedFailed);

  // A completion with no matching session is the old quiet case.
  TEST_ASSERT_FALSE(agentSessionsApply(t, "nope", "done", AgentInfo{}, 6000));
}

void test_agent_sessions_generation(void) {
  AgentTable t;
  AgentInfo a;
  agentInfoClear(a);
  agentSessionsApply(t, "s1", "running", a, 0);
  const uint32_t g = t.generation;

  // A heartbeat is not a state change: a screen dismissed with Key1 stays down.
  TEST_ASSERT_FALSE(agentSessionsApply(t, "s1", "running", a, 1000));
  TEST_ASSERT_EQUAL_UINT32(g, t.generation);

  // A real transition re-opens it.
  TEST_ASSERT_TRUE(agentSessionsApply(t, "s1", "waiting", a, 2000));
  TEST_ASSERT_TRUE(t.generation != g);
}

void test_agent_sessions_expire(void) {
  AgentTable t;
  AgentInfo a;
  agentInfoClear(a);
  agentSessionsApply(t, "s1", "running", a, 0);
  agentSessionsApply(t, "s2", "waiting", a, 1000);

  TEST_ASSERT_EQUAL_INT(0, agentSessionsExpire(t, AGENT_SESSION_TTL_MS - 1, AGENT_SESSION_TTL_MS));
  // s1 was last heard at 0, so it is the first to go.
  TEST_ASSERT_EQUAL_INT(1, agentSessionsExpire(t, AGENT_SESSION_TTL_MS, AGENT_SESSION_TTL_MS));
  TEST_ASSERT_EQUAL_STRING("waiting", agentSessionsStateName(t));

  TEST_ASSERT_EQUAL_INT(1, agentSessionsExpire(t, 1000 + AGENT_SESSION_TTL_MS, AGENT_SESSION_TTL_MS));
  TEST_ASSERT_NULL(agentSessionsStateName(t));
  // Silence is not a failure: it must not queue the failure chime.
  TEST_ASSERT_FALSE(t.lastEndedFailed);
}

void test_agent_sessions_capacity(void) {
  AgentTable t;
  AgentInfo a;
  agentInfoClear(a);
  char id[16];
  // One more than the table holds: the quietest is evicted, the newest is kept.
  for (int i = 0; i < AGENT_MAX_SESSIONS + 1; i++) {
    snprintf(id, sizeof(id), "s%d", i);
    TEST_ASSERT_TRUE(agentSessionsApply(t, id, "running", a, 1000 + i));
  }
  TEST_ASSERT_EQUAL_INT(AGENT_MAX_SESSIONS, t.n);
}

void test_forecast_parse(void) {
  const char* body =
      "{\"daily\":{\"time\":[\"2026-10-20\",\"2026-10-21\",\"2026-10-22\"],"
      "\"weather_code\":[3,61,0],"
      "\"temperature_2m_max\":[18.4,16.6,20.2],"
      "\"temperature_2m_min\":[9.1,8.4,7.0],"
      "\"precipitation_probability_max\":[20,80,null]}}";
  ForecastData f;
  TEST_ASSERT_TRUE(forecastParseJson(body, f));
  TEST_ASSERT_EQUAL_STRING("Today", f.day[0].label);
  TEST_ASSERT_EQUAL_STRING("Tomorrow", f.day[1].label);
  TEST_ASSERT_EQUAL_STRING("Thu", f.day[2].label);
  TEST_ASSERT_EQUAL_INT(18, f.day[0].maxC);
  TEST_ASSERT_EQUAL_INT(9, f.day[0].minC);
  TEST_ASSERT_EQUAL_STRING("Overcast", f.day[0].cond);
  TEST_ASSERT_EQUAL_STRING("Rain", f.day[1].cond);
  TEST_ASSERT_EQUAL_INT(80, f.day[1].rainPct);
  // A null chance is unknown, never "0% chance".
  TEST_ASSERT_EQUAL_INT(-1, f.day[2].rainPct);
}

void test_forecast_parse_rejects(void) {
  ForecastData f;
  TEST_ASSERT_FALSE(forecastParseJson("not json", f));
  TEST_ASSERT_FALSE(forecastParseJson("{}", f));
  TEST_ASSERT_FALSE(forecastParseJson("{\"daily\":{\"time\":[\"2026-10-20\"]}}", f));
  TEST_ASSERT_FALSE(forecastParseJson(
      "{\"daily\":{\"time\":[],\"temperature_2m_max\":[],\"temperature_2m_min\":[]}}", f));
}

void test_forecast_path(void) {
  char p[224];
  TEST_ASSERT_TRUE(forecastApiPath(39.9042, 116.4074, p, sizeof(p)));
  TEST_ASSERT_NOT_NULL(strstr(p, "latitude=39.9042"));
  TEST_ASSERT_NOT_NULL(strstr(p, "forecast_days=3"));
  TEST_ASSERT_NOT_NULL(strstr(p, "timezone=auto"));
  // The cap is part of the contract: the request must not be truncated.
  TEST_ASSERT_LESS_THAN_UINT32(sizeof(p), strlen(p));
}

void test_trend_store_line(void) {
  TrendRecord r;
  r.epoch = 1791436863u;
  r.usedMicroCents = 320000000ULL;
  r.pct = 45;

  char buf[64];
  TEST_ASSERT_EQUAL_INT(23, trendStoreLine(r, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("1791436863,320000000,45", buf);

  TrendRecord out;
  TEST_ASSERT_TRUE(trendStoreParseLine(buf, out));
  TEST_ASSERT_EQUAL_UINT32(r.epoch, out.epoch);
  TEST_ASSERT_EQUAL_UINT64(r.usedMicroCents, out.usedMicroCents);
  TEST_ASSERT_EQUAL_UINT8(r.pct, out.pct);

  TEST_ASSERT_FALSE(trendStoreParseLine("", out));
  TEST_ASSERT_FALSE(trendStoreParseLine("1791436863,320000000", out));   // truncated
  TEST_ASSERT_FALSE(trendStoreParseLine("0,1,2", out));                  // no wall clock
  TEST_ASSERT_FALSE(trendStoreParseLine("1791436863,1,101", out));       // pct > 100
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
  RUN_TEST(test_url_decode);
  RUN_TEST(test_tz_offset_query);
  RUN_TEST(test_tz_offset_query_clamps);
  RUN_TEST(test_wttr_one_line_path);
  RUN_TEST(test_wttr_path_rejects_bad_input);
  RUN_TEST(test_wttr_path_worst_case_place);
  RUN_TEST(test_moon_api_path);
  RUN_TEST(test_settings_query_single);
  RUN_TEST(test_settings_query_multi_and_encoding);
  RUN_TEST(test_settings_query_volume);
  RUN_TEST(test_settings_query_location_group);
  RUN_TEST(test_settings_query_empty_value_clears);
  RUN_TEST(test_settings_query_rejects_unknown_key);
  RUN_TEST(test_settings_query_rejects_out_of_range);
  RUN_TEST(test_settings_query_rejects_junk_numbers);
  RUN_TEST(test_settings_query_rejects_half_a_location);
  RUN_TEST(test_settings_query_rejects_malformed);
  RUN_TEST(test_settings_query_rejects_long_values);
  RUN_TEST(test_settings_query_all_fields_at_once);
  RUN_TEST(test_settings_json_round_trip);
  RUN_TEST(test_settings_json_empty_and_small_buffer);
  RUN_TEST(test_settings_json_rejects_bad_documents);
  RUN_TEST(test_settings_describe);
  RUN_TEST(test_wifi_cred_set);
  RUN_TEST(test_wifi_cred_encoding);
  RUN_TEST(test_wifi_cred_open_network_and_forget);
  RUN_TEST(test_wifi_cred_rejects);
  RUN_TEST(test_wifi_cred_rejects_long_values);
  RUN_TEST(test_wifi_cred_describe_redacts);
  RUN_TEST(test_usage_parse);
  RUN_TEST(test_usage_parse_clamps_and_zero_limit);
  RUN_TEST(test_usage_parse_bad_input);
  RUN_TEST(test_usage_parse_truncated_json);
  RUN_TEST(test_usage_parse_meters_not_object);
  RUN_TEST(test_usage_parse_numeric_micros);
  RUN_TEST(test_usage_parse_reset_in_past_or_unset_clock);
  RUN_TEST(test_usage_parse_negative_micros);
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
  RUN_TEST(test_usage_pace_verdict);
  RUN_TEST(test_usage_pace_projection);
  RUN_TEST(test_usage_alert_crossings);
  RUN_TEST(test_usage_alert_jump_is_one_cue);
  RUN_TEST(test_usage_alert_rearms_after_reset);
  RUN_TEST(test_usage_alert_reset_helper);
  RUN_TEST(test_health_format_uptime);
  RUN_TEST(test_health_format_bytes);
  RUN_TEST(test_health_format_age);
  RUN_TEST(test_health_format_ms);
  RUN_TEST(test_agent_parse_full);
  RUN_TEST(test_agent_parse_plus_is_space);
  RUN_TEST(test_agent_parse_optional_fields);
  RUN_TEST(test_agent_parse_drops_control_bytes);
  RUN_TEST(test_agent_parse_ignores_unknown_keys);
  RUN_TEST(test_agent_parse_rejects);
  RUN_TEST(test_usage_parse_micro_cents);
  RUN_TEST(test_ui_model_page_wrap);
  RUN_TEST(test_ui_model_card_wrap);
  RUN_TEST(test_http_query_from_head);
  RUN_TEST(test_http_header_value);
  RUN_TEST(test_ble_frame_parse);
  RUN_TEST(test_status_machine_transitions);
  RUN_TEST(test_config_limits);
  RUN_TEST(test_settings_json_rejects_non_numeric_location);
  RUN_TEST(test_usage_pace_even);
  RUN_TEST(test_weekday_from_iso);
  RUN_TEST(test_notice_parse_full);
  RUN_TEST(test_notice_parse_defaults_and_unknown_keys);
  RUN_TEST(test_notice_parse_clear);
  RUN_TEST(test_notice_parse_strips_control_bytes);
  RUN_TEST(test_notice_parse_rejects);
  RUN_TEST(test_notice_parse_length_budget);
  RUN_TEST(test_agent_parse_session);
  RUN_TEST(test_agent_parse_length_budget);
  RUN_TEST(test_agent_sessions_aggregate);
  RUN_TEST(test_agent_sessions_generation);
  RUN_TEST(test_agent_sessions_expire);
  RUN_TEST(test_agent_sessions_capacity);
  RUN_TEST(test_forecast_parse);
  RUN_TEST(test_forecast_parse_rejects);
  RUN_TEST(test_forecast_path);
  RUN_TEST(test_trend_store_line);
  return UNITY_END();
}
