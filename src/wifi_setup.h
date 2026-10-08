#pragma once
#include <stdint.h>

struct WifiState {
  bool open;
  int step;         // 0=scanning, 1=list, 2=password, 3=connecting/result
  int sel;
  char pass[64];
  int charGroup;    // 0=lower, 1=upper, 2=digits, 3=symbols
  int charIdx;
  int netsCount;
  bool scanFailed;   // true if last scan errored (radio race / WIFI_SCAN_FAILED)
  bool connDone;
  bool connOk;
  uint32_t scanStart;
  int flashIdx;      // index of last-touched character cell for visual feedback, -1 = none
  uint32_t flashTime; // millis() when the flash animation started
};

void wifiSetupInit(WifiState& state);
void wifiSetupDraw(WifiState& state);
void wifiSetupHandleButtons(WifiState& state);
