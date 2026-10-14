#pragma once
#include <Arduino.h>
#include <WiFi.h>

#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 64

// Station power-save policy. The two phases matter: while the station is still
// associating (boot / scan / first connect) MAX_MODEM's aggressive sleep can
// miss the AP's association frames and push the connect past the setup UI's
// grace window, so we use MIN_MODEM there. Once associated we switch to
// MAX_MODEM for the lowest idle radio power (it costs up to the AP's listen
// interval of added latency on inbound status-server webhooks, which is fine
// for a desk gadget).
#define APP_WIFI_PS_BOOT WIFI_PS_MIN_MODEM
#define APP_WIFI_PS      WIFI_PS_MAX_MODEM

struct NetScanResult {
  char ssid[33];
  int32_t rssi;
  uint8_t encryption;
};

int  netScan(NetScanResult* out, int max);
bool netConnect(const char* ssid, const char* pass);
IPAddress netIP();
void netSaveCred(const char* ssid, const char* pass);
bool netLoadCred(char* ssid, size_t ssidLen, char* pass, size_t passLen);
