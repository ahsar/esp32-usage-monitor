#ifdef FRAME_DUMP
#include "frame_dump.h"
#include "ui_model.h"
#include "pager.h"
#include "ui_compose.h"
#include "wifi_setup.h"
#include "poller.h"
#include "hw/display.h"
#include <Arduino.h>

static uint32_t s_dumpIdx = 0;
static uint32_t s_dumpNext = 0;

// One canvas, framed for tools/frame_dump.py.
static void dumpCanvas(uint32_t idx) {
  hwDisplayPush();
  uint16_t* fb = (uint16_t*)hwCanvas()->getFramebuffer();
  Serial.printf("FRAME %u %d %d\n", idx, HW_CANVAS_W, HW_CANVAS_H);
  delay(30);
  Serial.write((uint8_t*)fb, HW_CANVAS_W * HW_CANVAS_H * 2);
  Serial.write('\n');
  Serial.flush();
}

// ── WiFi setup screens ──────────────────────────────────────────────────────
// These are what the panel shows while the device is off the air, so they are
// also the screens nobody can look at remotely: the page walk below never sees
// them. Dumped with a seeded scan so the list and password pages have content,
// in the order a user meets them (boot connect, scanning, list, password,
// result), then handed back to the real screen.
static const int kWifiDumpSteps[] = { 4, 0, 1, 2, 3 };
static const int kWIFI_DUMP_N = (int)(sizeof(kWifiDumpSteps) / sizeof(kWifiDumpSteps[0]));
static WifiState s_dbgWifi;
static uint32_t  s_wifiIdx  = 0;
static uint32_t  s_wifiNext = 0;
static bool      s_wifiSeeded = false;

// The walk keeps the fetch worker parked while it streams. The payload and the
// device's own log lines share one USB CDC, and a host drains it far slower than
// the canvases are produced (measured: 84 KB/s against 329 KB per frame), so the
// write blocks for ~1.4 s per canvas and anything another task prints while it is
// blocked lands in the middle of the byte stream the host is framing — the fetch
// lines ("http: … tls=…", "fetch usage: ok") are the ones that appear there. The
// poller pause is the lever OTA pulls for the same reason. Released after the last
// wifi frame, so the page walk below still waits for real poll data rather than
// the offline placeholders.
static const uint32_t WIFI_DUMP_PAUSE_MS = 25000;
static bool s_wifiPaused = false;

bool frameDumpWifiTick(uint32_t now) {
  if (s_wifiIdx >= (uint32_t)kWIFI_DUMP_N) return false;
  if (!s_wifiPaused) {
    s_wifiPaused = true;
    pollerPauseAll(now, WIFI_DUMP_PAUSE_MS);
  }
  if (now < s_wifiNext) return true;
  s_wifiNext = now + 1500;

  if (!s_wifiSeeded) {
    s_wifiSeeded = true;
    wifiSetupDebugSeed(6);
    wifiSetupInit(s_dbgWifi, "HomeNet-5G", "hunter2hunter2");
  }

  int step = kWifiDumpSteps[s_wifiIdx];
  s_dbgWifi.step = step;
  s_dbgWifi.open = true;
  s_dbgWifi.netsCount = 6;
  s_dbgWifi.sel = 0;
  s_dbgWifi.pass[0] = 0;
  switch (step) {
    case 0:  // scanning: netsCount != 0 suppresses the scan this pass
      s_dbgWifi.netsCount = 1;
      break;
    case 2:  // password page, with the prefill the saved network would give it
      strncpy(s_dbgWifi.pass, "hunter2hunter2", sizeof(s_dbgWifi.pass) - 1);
      break;
    case 3:  // result page: connDone so it does not associate from the dumper
      s_dbgWifi.connDone = true;
      s_dbgWifi.connOk   = false;
      break;
    default:
      break;
  }

  wifiSetupDraw(s_dbgWifi);
  dumpCanvas(10 + (uint32_t)s_wifiIdx);   // 10+ so a page frame keeps its own name
  s_wifiIdx++;
  // Last wifi frame: let the pollers back in, or the page walk below has nothing
  // to draw but placeholders.
  if (s_wifiIdx >= (uint32_t)kWIFI_DUMP_N) pollerPauseAll(millis(), 0);
  return true;
}

void frameDumpTick(uint32_t now, const UiData& d) {
  // The WiFi setup screens come first, one canvas per tick, from
  // frameDumpWifiTick(); the page walk below keeps its own counter.
  if (s_wifiIdx < (uint32_t)kWIFI_DUMP_N) return;
  if (s_dumpIdx >= (uint32_t)(UI_PAGE_COUNT + UI_WC_COUNT - 1)) return;
  if (now < s_dumpNext) return;
  // Wait for real data (or give up after 60s) so the cards aren't placeholders.
  bool ready = d.usage->valid && d.weather->valid && (!d.hasAir || d.air->valid);
  if (s_dumpIdx == 0 && !ready && now < 60000) return;
  s_dumpNext = now + 1500;

  uint32_t idx = s_dumpIdx;
  if (idx < UI_PAGE_COUNT) {
    pagerSetPage((int)idx);
  } else {
    pagerSetPage(UI_PAGE_WEATHER);
    pagerSetCard((int)(idx - UI_PAGE_COUNT + 1));
  }
  pagerCancelAnim();

  uiRender(d);
  dumpCanvas(idx);
  s_dumpIdx++;
}
#endif
