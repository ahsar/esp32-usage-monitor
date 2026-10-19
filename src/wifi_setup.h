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
  char savedPass[64]; // …and its passphrase, so re-picking that network is one tap
  uint32_t scanStart;
  uint32_t connStart; // millis() when the step-4 auto-connect grace began
  int flashIdx;      // index of last-touched character cell for visual feedback, -1 = none
  uint32_t flashTime; // millis() when the flash animation started
};

// `savedSsid` / `savedPass` are the stored STA credentials (NULL/empty = none).
// The passphrase is kept so that selecting the SSID already in NVS prefills the
// password page instead of making the user type it again.
void wifiSetupInit(WifiState& state, const char* savedSsid, const char* savedPass);
void wifiSetupDraw(WifiState& state);

#ifdef FRAME_DUMP
// Debug-only (-DFRAME_DUMP): fill the scan results the list and password pages
// index, so tools/frame_dump.py can render them with no scan and no radio.
void wifiSetupDebugSeed(int count);
#endif
