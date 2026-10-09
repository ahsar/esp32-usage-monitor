#pragma once
#include <stdint.h>

struct WifiState {
  bool open;
  int step;         // 0=scanning, 1=list, 2=password, 3=connecting/result,
                    // 4=auto-connecting with saved creds
  int sel;
  char pass[64];
  int charGroup;    // 0=lower, 1=upper, 2=digits, 3=symbols
  int charIdx;
  int netsCount;
  bool scanFailed;   // true if last scan errored (radio race / WIFI_SCAN_FAILED)
  bool connDone;
  bool connOk;
  bool haveCreds;    // saved creds exist → try connecting before scanning
  char savedSsid[33]; // SSID to connect to in step 4
  uint32_t scanStart;
  uint32_t connStart; // millis() when the step-4 auto-connect grace began
  int flashIdx;      // index of last-touched character cell for visual feedback, -1 = none
  uint32_t flashTime; // millis() when the flash animation started
};

// `savedSsid` is the stored STA SSID (NULL/empty = no credentials).
void wifiSetupInit(WifiState& state, const char* savedSsid);
void wifiSetupDraw(WifiState& state);
