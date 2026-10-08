#include "hw/net.h"
#include "voice/i2s_audio.h"
#include <Preferences.h>

static Preferences _prefs;

int netScan(NetScanResult* out, int max) {
  // Scan with radio power-cycle. Avoids a race where a stale
  // WiFi.begin() (saved creds for an AP out of range) leaves the radio
  // mid-connect when scanNetworks() fires; on Arduino-ESP32 v3 this
  // commonly yields WIFI_SCAN_FAILED (-1).
  //
  // Returns:
  //    -1  scan failed (radio busy / scan error)
  //     0  scan ok, no APs in range
  //    >0  network count, copied into `out`
  audioPauseForScan();

  int result = -1;  // pessimistic default = failed
  for (int attempt = 0; attempt < 3; ++attempt) {
    WiFi.disconnect(true);     // true = wipe stale AP config too
    delay(150);
    WiFi.mode(WIFI_OFF);
    delay(50);
    WiFi.mode(WIFI_STA);
    delay(100);                // let the radio re-arm

    int n = WiFi.scanNetworks();
    if (n > 0) { result = n; break; }
    if (n == 0) { result = 0; delay(200); continue; }  // retry — APs may still beacon
    // n < 0 (WIFI_SCAN_FAILED) — try again
    result = -1;
    delay(200);
  }

  if (result <= 0) {
    audioResumeAfterScan();
    return result;
  }

  int n = result;
  if (n > max) n = max;
  for (int i = 0; i < n; i++) {
    strncpy(out[i].ssid, WiFi.SSID(i).c_str(), 32);
    out[i].ssid[32] = 0;
    out[i].rssi = WiFi.RSSI(i);
    out[i].encryption = WiFi.encryptionType(i);
  }

  WiFi.scanDelete();          // free internal scan cache (copies done)
  audioResumeAfterScan();
  return n;
}

bool netConnect(const char* ssid, const char* pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  for (int i = 0; i < 50; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      netSaveCred(ssid, pass);
      return true;
    }
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

void netDisconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool netConnected() {
  return WiFi.status() == WL_CONNECTED;
}

const char* netSSID() {
  return WiFi.SSID().c_str();
}

IPAddress netIP() {
  return WiFi.localIP();
}

int8_t netRSSI() {
  return WiFi.RSSI();
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
