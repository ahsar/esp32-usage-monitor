// Health page: the device's own state — link quality, uptime, memory, the last
// fetch and its phase timings, firmware, token. Read-only by construction: it
// draws what main.cpp's refreshEnv() collected and starts nothing itself.
//
// This is the page that replaces plugging the board into USB to run
// tools/link_probe.py. Everything here is already printed somewhere in the boot
// / fetch log; the point is that a glance at the panel now answers "is the link
// dead or is the fetch failing" without a cable.
#include "ui/ui_internal.h"
#include "usage_display.h"
#include "ui/theme.h"
#include "util/health_util.h"
#include <cstdio>
#include <cstring>

using namespace ui_theme;

namespace {

// The five jobs, spelled here rather than pulled from fetch_task.h: the UI layer
// has no business including the worker's header (and its OAuth/config deps) for
// four strings.
const char* jobName(int8_t job) {
  switch (job) {
    case FETCH_USAGE:    return "usage";
    case FETCH_WEATHER:  return "weather";
    case FETCH_MOON:     return "moon";
    case FETCH_AIR:      return "air";
    case FETCH_FORECAST: return "forecast";
    default:             return "none";
  }
}

// One label/value row. Values are right-aligned so the column of numbers is
// scannable; the label stays muted.
void row(int y, const char* label, const char* value, uint16_t vcol) {
  textAt(F_BODY, COL_DIM, SAFE_L, y, label);
  textRight(F_BODY, vcol, SAFE_R, y, value);
}

}  // namespace

void uiDrawHealth(const HealthData& h, int ox, int oy) {
  g_ox = ox; g_oy = oy;
  cardBg();
  statusBar("Health");

  if (!h.valid) {
    textCenter(F_HEAD, COL_MUTED, HW_W / 2, 110, "collecting...");
    return;
  }

  char val[40];

  // WiFi: the RSSI describes the downlink only (hw/net.h) — it is what the
  // device hears, not what the AP hears, and the two have already disagreed
  // once in this project's history.
  if (h.online) snprintf(val, sizeof(val), "%d dBm", h.rssi);
  else          snprintf(val, sizeof(val), "down");
  row(60, "WiFi", val, h.online ? COL_GREEN : COL_RED);

  health_fmt::formatUptime(val, sizeof(val), h.uptimeSec);
  row(84, "Uptime", val, COL_TEXT);

  // Internal SRAM and PSRAM are both shown because they fail differently: the
  // internal pool is what mbedTLS and Bluedroid need, and PSRAM being large says
  // nothing about it.
  char ib[16], pb[16];
  health_fmt::formatBytes(ib, sizeof(ib), h.heapInternal);
  health_fmt::formatBytes(pb, sizeof(pb), h.heapPsram);
  snprintf(val, sizeof(val), "%s / %s", ib, pb);
  row(108, "Heap", val, h.heapInternal < 20000u ? COL_YELLOW : COL_TEXT);

  if (h.lastJob == 0) {
    snprintf(val, sizeof(val), "none yet");
  } else {
    char age[16];
    health_fmt::formatAge(age, sizeof(age), h.lastFetchAgeSec);
    snprintf(val, sizeof(val), "%s %s %s", jobName(h.lastJob),
             h.lastOk ? "ok" : "fail", age);
  }
  row(132, "Last", val, h.lastJob == 0 ? COL_MUTED : (h.lastOk ? COL_GREEN : COL_RED));

  // DNS is listed next to TLS because it fails first when the path is dead, and
  // the two are what tools/link_probe.py samples off the serial log.
  char dns[16], tls[16];
  health_fmt::formatMs(dns, sizeof(dns), h.lastDnsMs);
  health_fmt::formatMs(tls, sizeof(tls), h.lastTlsMs);
  snprintf(val, sizeof(val), "%s / %s", dns, tls);
  row(156, "dns/tls", val, COL_TEXT);

  row(180, "FW", h.fwVersion, COL_DIM);

  // A single trailing status line, like the Clock page's "BLE on": the two
  // things that are booleans rather than numbers, so they do not deserve a row
  // each. The token one is the same fact the Usage page's AUTH NEEDED reports,
  // spelled smaller.
  char line[48];
  snprintf(line, sizeof(line), "%s \u2022 %s", h.tokensValid ? "token ok" : "no token",
           h.bleUp ? "BLE on" : "BLE off");
  textCenter(F_SMALL, h.tokensValid ? COL_MUTED : COL_YELLOW, HW_W / 2, 202, line);
}
