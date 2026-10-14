#include "hw/net.h"
#include "voice/i2s_audio.h"
#include <Preferences.h>
#include <esp_wifi.h>

static Preferences _prefs;

// How long a single blocking scan waits per channel. The Arduino default is
// 300 ms/channel; 13 channels of that is ~4 s before overheads. 150 ms is
// plenty for nearby APs and roughly halves the sweep.
static const uint32_t SCAN_MS_PER_CHAN = 150;

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

void netSaveCred(const char* ssid, const char* pass) {
  _prefs.begin("buddy", false);
  _prefs.putString("wifi_ssid", ssid);
  _prefs.putString("wifi_pass", pass);
  _prefs.end();
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
