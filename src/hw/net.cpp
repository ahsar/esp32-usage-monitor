#include "hw/net.h"
#include "voice/i2s_audio.h"
#include <Preferences.h>
#include <esp_wifi.h>

static Preferences _prefs;

// How long a single blocking scan waits per channel. The Arduino default is
// 300 ms/channel; 13 channels of that is ~4 s before overheads. 150 ms is
// plenty for nearby APs and roughly halves the sweep.
static const uint32_t SCAN_MS_PER_CHAN = 150;

// netRoam(): an AP below this cannot carry a TLS handshake anyway, so it is not
// worth leaving a working association for. Not a *roaming threshold* — see
// netRoam() for why rssi does not choose the APs here.
static const int32_t ROAM_MIN_RSSI = -80;
static const int   ROAM_MAX_APS   = 8;

int netScan(NetScanResult* out, int max) {
  // Fast scan. The old implementation power-cycled the radio
  // (WIFI_OFF → WIFI_STA) up to three times, and each cycle re-runs WiFi
  // bring-up + RF calibration. Combined with modem sleep being enabled at
  // boot, a single boot scan could take 30 s+.
  //
  // Instead: abort any in-flight connect with a raw esp_wifi_disconnect()
  // (cheap, keeps the station up), power save off for the duration of the
  // scan, and scan once with a short per-channel dwell. A single retry
  // covers the rare WIFI_SCAN_FAILED race.
  //
  // Returns:
  //    -1  scan failed (radio busy / scan error)
  //     0  scan ok, no APs in range
  //    >0  network count, copied into `out`
  audioPauseForScan();

  const uint32_t t0 = millis();

  // Stop a pending/slow connect from racing the scan. WiFi.disconnect(true)
  // would tear the radio down (expensive); esp_wifi_disconnect() aborts the
  // connect but leaves the station up. Auto-reconnect would immediately
  // restart the connect, so disable it for the scan.
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  esp_wifi_disconnect();          // ESP_ERR_WIFI_NOT_CONNECT is harmless
  delay(30);

  // Power save off: with WIFI_PS_MIN_MODEM the driver wakes on DTIM beacons
  // between channels and stretches the sweep.
  WiFi.setSleep(WIFI_PS_NONE);

  int n = -1;
  for (int attempt = 0; attempt < 2; ++attempt) {
    n = WiFi.scanNetworks(false /*async*/, false /*show_hidden*/,
                          false /*passive*/, SCAN_MS_PER_CHAN);
    if (n >= 0) break;            // 0 = ok but empty, >0 = found
    delay(100);                   // WIFI_SCAN_FAILED — one retry
  }

  WiFi.setSleep(APP_WIFI_PS_BOOT);
  WiFi.setAutoReconnect(true);

  if (n <= 0) {
    Serial.printf("netScan: %s in %ums\n",
                  n == 0 ? "no networks" : "FAILED", (unsigned)(millis() - t0));
    audioResumeAfterScan();
    return n == 0 ? 0 : -1;
  }

  int cnt = (n > max) ? max : n;
  for (int i = 0; i < cnt; i++) {
    strncpy(out[i].ssid, WiFi.SSID(i).c_str(), 32);
    out[i].ssid[32] = 0;
    out[i].rssi = WiFi.RSSI(i);
    out[i].encryption = WiFi.encryptionType(i);
    memset(out[i].bssid, 0, sizeof(out[i].bssid));
    WiFi.BSSID(i, out[i].bssid);
    out[i].channel = (uint8_t)WiFi.channel(i);
  }

  WiFi.scanDelete();          // free internal scan cache (copies done)
  Serial.printf("netScan: %d networks in %ums\n", cnt, (unsigned)(millis() - t0));
  audioResumeAfterScan();
  return cnt;
}

bool netConnect(const char* ssid, const char* pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  for (int i = 0; i < 50; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      netSaveCred(ssid, pass);
      WiFi.setSleep(APP_WIFI_PS);   // associated → allow max modem sleep
      return true;
    }
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

IPAddress netIP() {
  return WiFi.localIP();
}

static String bssidStr(const uint8_t* b) {
  char s[18];
  snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
           b[0], b[1], b[2], b[3], b[4], b[5]);
  return String(s);
}

bool netRoam() {
  // Everything in here delays on the calling task — loop(). One line up front,
  // so the expected freeze is in the log even when the roam fails early.
  Serial.println("wifi: roam: starting (loop() blocked for the whole roam)");
  char ssid[WIFI_SSID_MAX + 1], pass[WIFI_PASS_MAX + 1];
  if (!netLoadCred(ssid, sizeof(ssid), pass, sizeof(pass))) return false;

  uint8_t cur[6] = {0};
  WiFi.BSSID(cur);

  // Scan while associated: the driver hops channels and comes back, so the link
  // — and with it the mDNS/OTA/status servers — survives a roam that finds
  // nothing worth moving to.
  WiFi.setSleep(WIFI_PS_NONE);        // as netScan(): the sweep is the point
  NetScanResult cand[ROAM_MAX_APS];
  int seen = 0, found = 0;
  int n = WiFi.scanNetworks(false /*async*/, false /*show_hidden*/,
                            false /*passive*/, SCAN_MS_PER_CHAN);
  for (int i = 0; i < n; ++i) {
    seen++;
    if (WiFi.SSID(i) != ssid) continue;
    uint8_t bssid[6] = {0};
    if (!WiFi.BSSID(i, bssid)) continue;
    if (memcmp(bssid, cur, 6) == 0) continue;      // the one we are leaving
    if (WiFi.RSSI(i) < ROAM_MIN_RSSI) continue;
    if (found >= ROAM_MAX_APS) continue;
    NetScanResult& c = cand[found++];
    strncpy(c.ssid, ssid, sizeof(c.ssid) - 1);
    c.ssid[sizeof(c.ssid) - 1] = 0;
    memcpy(c.bssid, bssid, 6);
    c.rssi = WiFi.RSSI(i);
    c.channel = (uint8_t)WiFi.channel(i);
  }
  WiFi.scanDelete();

  int best = -1;
  for (int i = 0; i < found; ++i) {
    if (best < 0 || cand[i].rssi > cand[best].rssi) best = i;
  }
  if (best < 0) {
    WiFi.setSleep(APP_WIFI_PS);
    // Distinguish the two: a scan that never ran would otherwise read exactly
    // like "this SSID has no other AP", and that is a silent no-op roam.
    if (n < 0) {
      Serial.println("wifi: roam: scan failed, staying put");
    } else {
      Serial.printf("wifi: roam: no other AP for \"%s\" (%d seen)\n", ssid, seen);
    }
    return false;
  }

  Serial.printf("wifi: roam -> %s ch%u rssi=%d (leaving %s rssi=%d)\n",
                bssidStr(cand[best].bssid).c_str(), (unsigned)cand[best].channel,
                (int)cand[best].rssi, bssidStr(cur).c_str(), (int)WiFi.RSSI());
  // Say the quiet part out loud: everything below is a blocking wait on the
  // caller's task, which is loop() — touch, the channel transports, the token
  // flush and the wedge watchdog are all frozen until this returns. Worst case
  // is the pinned attempt (8 s) plus the un-pinned fallback (4 s) plus the
  // sweep before them; the diag stall reporter should never be the first to
  // explain a freeze this code causes on purpose.
  Serial.println("wifi: roam: loop() will stall until the new link is up");

  uint32_t t0 = millis();
  WiFi.begin(ssid, pass, cand[best].channel, cand[best].bssid);
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    // The AP we aimed at is gone, which a scan taken seconds ago cannot rule
    // out. Fall back to un-pinned selection rather than sitting disconnected.
    // 4 s, not the 12 s it used to get: the fallback mostly covers a momentary
    // radio hiccup, a dead AP just fails here too, and every extra second is
    // another second loop() is frozen. auto-reconnect picks the old AP back up
    // on the next beacons if this attempt also misses.
    Serial.println("wifi: roam: pinned AP did not answer, retrying un-pinned");
    uint32_t t1 = millis();
    WiFi.begin(ssid, pass);
    while (WiFi.status() != WL_CONNECTED && millis() - t1 < 4000) delay(100);
  }
  // main.cpp's connect-once block has already run, so a roam restores the radio
  // policy itself.
  WiFi.setSleep(APP_WIFI_PS);
  WiFi.setTxPower(APP_WIFI_TX_DBM);

  bool ok = WiFi.status() == WL_CONNECTED;
  uint8_t now[6] = {0};
  WiFi.BSSID(now);
  Serial.printf("wifi: roam %s in %lums (rssi=%d bssid=%s)\n",
                ok ? "ok" : "FAILED", (unsigned long)(millis() - t0),
                (int)WiFi.RSSI(), bssidStr(now).c_str());
  return ok;
}

// See the note in hw/net.h: the station stays awake for the duration of a
// fetch, because a handshake pays the DTIM wake-up wait on every round trip.
void netRadioWake() { WiFi.setSleep(WIFI_PS_NONE); }
void netRadioRest() { WiFi.setSleep(APP_WIFI_PS); }

bool netSaveCred(const char* ssid, const char* pass) {
  _prefs.begin("buddy", false);
  bool ok = _prefs.putString("wifi_ssid", ssid) > 0;
  ok = (_prefs.putString("wifi_pass", pass) > 0) && ok;
  _prefs.end();
  // 0 means the namespace could not be written (full or read-only NVS). The
  // caller reports the outcome rather than claiming credentials it does not have.
  return ok;
}

void netForgetCred() {
  _prefs.begin("buddy", false);
  _prefs.remove("wifi_ssid");
  _prefs.remove("wifi_pass");
  _prefs.end();
  // The driver keeps its own copy of the AP config in NVS (arduino-esp32 is
  // persistent by default), and it would associate at the next esp_wifi_start —
  // before our setup() ever calls WiFi.begin(). eraseAP=true clears that too.
  WiFi.disconnect(/*wifioff*/ false, /*eraseap*/ true);
}

bool netLoadCred(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
  _prefs.begin("buddy", true);
  String s = _prefs.getString("wifi_ssid", "");
  String p = _prefs.getString("wifi_pass", "");
  _prefs.end();
  if (s.length() == 0) return false;
  strncpy(ssid, s.c_str(), ssidLen - 1); ssid[ssidLen - 1] = 0;
  strncpy(pass, p.c_str(), passLen - 1); pass[passLen - 1] = 0;
  return true;
}
