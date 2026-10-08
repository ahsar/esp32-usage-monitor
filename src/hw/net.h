#pragma once
#include <Arduino.h>
#include <WiFi.h>

#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 64

struct NetScanResult {
  char ssid[33];
  int32_t rssi;
  uint8_t encryption;
};

int  netScan(NetScanResult* out, int max);
bool netConnect(const char* ssid, const char* pass);
void netDisconnect();
bool netConnected();
const char* netSSID();
IPAddress netIP();
int8_t netRSSI();
void netSaveCred(const char* ssid, const char* pass);
bool netLoadCred(char* ssid, size_t ssidLen, char* pass, size_t passLen);
