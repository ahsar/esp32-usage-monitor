#pragma once
#include <Arduino.h>
#include <WiFi.h>

#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 64

// Station power-save policy. MIN_MODEM for both phases, deliberately.
//
// MAX_MODEM (radio dozing between beacons) looked attractive for idle power but
// is measured out on this desk setup: minutes apart at the same spot, MIN_MODEM
// gave TLS handshakes of 2.2-4.4 s and all four pollers succeeded, while
// MAX_MODEM gave 4.5/11.7/31.7/30.7 s and every poll failed — the bursty
// handshake loses packets and then waits out TCP retransmit backoff, and the
// driver even reports a much lower rssi (-69 vs -57). Fetching is this device's
// whole job and it is mains/battery desk hardware, so it keeps the radio awake
// between polls. Re-measure `http: … tls=…` over a day before switching back.
#define APP_WIFI_PS_BOOT WIFI_PS_MIN_MODEM
#define APP_WIFI_PS      WIFI_PS_MIN_MODEM

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
