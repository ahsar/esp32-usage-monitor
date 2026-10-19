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

// Radio policy for the one moment the link is actually used. MIN_MODEM still
// sleeps the station between DTIM beacons, and that wake-up wait is paid on
// every round trip of a TLS handshake, so a fetch runs with power save off and
// the radio returns to APP_WIFI_PS when the job is done — the same shape as
// netScan()'s "power save off for the duration of the scan" and the OTA
// transfer window. Measured on the 2026-10-15 congested-AP sample and the
// gradient it extends: docs/power.md.
void netRadioWake();
void netRadioRest();

// Station TX power. 11 dBm (~13 mW) is plenty for a desk device next to the AP
// and cuts radio heat against the ~19.5 dBm default (~89 mW) — but that tuning
// does not survive a move to another AP, and the rssi above only describes the
// *downlink*. It is the uplink the AP has to hear, and a bursty TLS handshake
// that loses segments waits out TCP retransmit backoff (1+2+4+8…), which is how
// a handshake ends up in the 15-30 s band and then hits the cap. Override per
// site rather than by eye, and measure both arms with tools/link_probe.py:
//
//   PLATFORMIO_BUILD_FLAGS='-DAPP_WIFI_TX_DBM=WIFI_POWER_19_5dBm' \
//     pio run -e esp32-ota -t upload --upload-port <ip>
#ifndef APP_WIFI_TX_DBM
#define APP_WIFI_TX_DBM WIFI_POWER_11dBm
#endif

struct NetScanResult {
  char ssid[33];
  int32_t rssi;
  uint8_t encryption;
  uint8_t bssid[6];   // filled by netScan(); this is what netRoam() pins
  uint8_t channel;    // …on the channel it was heard on
};

int  netScan(NetScanResult* out, int max);
bool netConnect(const char* ssid, const char* pass);

// Re-select an AP after the path has been dead for several fetches in a row
// (pollerRoamDue()). Scans while associated and re-associates pinned to the
// strongest BSSID of our own SSID that is not the one we are on — "not the one
// we are on" rather than "stronger than the one we are on", because the AP that
// motivated this associates fine and then carries nothing, with an rssi inside
// the good AP's band. Returns true when the station was moved; a scan that
// finds nothing better is a no-op, and the policy's cooldown stops it from
// being retried every iteration.
bool netRoam();
IPAddress netIP();
bool netSaveCred(const char* ssid, const char* pass);

// Drop the stored credentials (and the driver's own copy of the AP config, which
// it persists separately). The next boot then comes up in the setup UI, which is
// the only way back for a device whose network no longer exists.
void netForgetCred();

bool netLoadCred(char* ssid, size_t ssidLen, char* pass, size_t passLen);
