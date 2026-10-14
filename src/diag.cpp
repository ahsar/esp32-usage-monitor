// TEMPORARY diagnostics for the boot/fetch freeze. See diag.h.
#include "diag.h"
#include "fetch_task.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct DiagState {
  uint32_t magic;
  uint32_t boots;
  uint32_t code;          // last breadcrumb
  uint32_t arg;
  uint32_t loopMs;        // loop heartbeat (millis of last loop entry)
  uint32_t loopIter;
  uint32_t stallMax;      // longest observed loop stall, ms
  uint32_t stallCnt;      // number of distinct stalls
  uint32_t pushMax;       // longest render+push, ms
  uint32_t resetReason;
  uint32_t online;
  uint32_t wifiStatus;
  char     host[12];      // endpoint of the in-flight HTTP request
};

static RTC_NOINIT_ATTR DiagState s_d;
static const uint32_t DIAG_MAGIC = 0x0C1AD1A5u;

static esp_timer_handle_t s_timer    = nullptr;
static uint32_t s_lastTick    = 0;    // previous loop heartbeat
static uint32_t s_lastReport  = 0;
static uint32_t s_periodic    = 0;
static uint32_t s_iterMaxGap  = 0;
static bool     s_inStall     = false;
static bool     s_bootPrinted = false;   // previous-run summary repeated later
static char     s_prevSummary[192];
static uint32_t s_prevResetReason = 0;
static uint32_t s_prevBoots = 0;
static volatile uint32_t s_fetchCode = DIAG_NONE;   // worker-only breadcrumb

// Codes only the fetch path writes (everything else comes from loop()).
static bool isFetchCode(uint32_t c) {
  switch ((DiagCode)c) {
    case DIAG_FETCH_JOB:
    case DIAG_FETCH_DONE:
    case DIAG_HTTP_CONNECT:
    case DIAG_HTTP_CONNECTED:
    case DIAG_HTTP_READ:
    case DIAG_HTTP_DONE:
    case DIAG_TOKEN_REFRESH:
    case DIAG_TOKEN_DONE:
      return true;
    default:
      return false;
  }
}

const char* diagCodeName(uint32_t c) {
  switch (c) {
    case DIAG_NONE:           return "none";
    case DIAG_SETUP:          return "setup";
    case DIAG_SETUP_DONE:     return "setup-done";
    case DIAG_LOOP:           return "loop";
    case DIAG_UI_RENDER:      return "ui-render";
    case DIAG_UI_PUSH:        return "ui-push";
    case DIAG_FETCH_JOB:      return "fetch-job";
    case DIAG_FETCH_DONE:     return "fetch-done";
    case DIAG_HTTP_CONNECT:   return "http-connect";
    case DIAG_HTTP_CONNECTED: return "http-connected";
    case DIAG_HTTP_READ:      return "http-read";
    case DIAG_HTTP_DONE:      return "http-done";
    case DIAG_TOKEN_REFRESH:  return "token-refresh";
    case DIAG_TOKEN_DONE:     return "token-done";
    case DIAG_WIFI_CONNECTED: return "wifi-connected";
    default:                  return "?";
  }
}

static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    default:                return "unknown";
  }
}

// Lock-free emit: the panic handler's own route (ROM console → UART0 + the
// USB-Serial-JTAG secondary console). Safe even if the Arduino Serial mutex is
// held by the task we are reporting on.
static void emit(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  esp_rom_printf("%s", buf);
}

void diagMark(DiagCode code, uint32_t arg) {
  s_d.code = (uint32_t)code;
  s_d.arg  = arg;
  if (isFetchCode((uint32_t)code)) s_fetchCode = (uint32_t)code;
}

// The worker's phase, kept apart from s_d.code: the main loop rewrites the
// breadcrumb through diagLoopTick()/diagPushMs() on every render, which would
// otherwise make "the fetch is stuck in …" point at the UI instead.
uint32_t diagFetchCodeNow() { return s_fetchCode; }

void diagMarkHost(const char* host) {
  if (!host) { s_d.host[0] = 0; return; }
  strlcpy(s_d.host, host, sizeof(s_d.host));
}

const char* diagHostNow() { return s_d.host[0] ? s_d.host : "-"; }

void diagPushMs(uint32_t ms) {
  if (ms > s_d.pushMax) s_d.pushMax = ms;
}

void diagSetNet(bool online, int wifiStatus) {
  s_d.online     = online ? 1u : 0u;
  s_d.wifiStatus = (uint32_t)wifiStatus;
}

static void stallCb(void*) {
  uint32_t now = millis();
  uint32_t gap = (uint32_t)(now - s_d.loopMs);
  if (gap < 1500) { s_inStall = false; return; }
  if (s_d.loopIter == 0) return;      // loop() has not started yet (boot)
  if (s_inStall && (uint32_t)(now - s_lastReport) < 1000) return;
  if (!s_inStall) { s_inStall = true; s_d.stallCnt++; }
  if (gap > s_d.stallMax) s_d.stallMax = gap;
  s_lastReport = now;

  emit("diag: STALL %ums bc=%s(%u) host=%s iter=%u busy=%d jobsQ=%u resQ=%u "
       "online=%u wifi=%u heap=%u psram=%u\n",
       (unsigned)gap, diagCodeName(s_d.code), (unsigned)s_d.arg,
       s_d.host[0] ? s_d.host : "-", (unsigned)s_d.loopIter,
       fetchTaskBusy() ? 1 : 0, (unsigned)fetchTaskPendingJobs(),
       (unsigned)fetchTaskPendingResults(), (unsigned)s_d.online,
       (unsigned)s_d.wifiStatus, (unsigned)esp_get_free_heap_size(),
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void diagBoot() {
  bool valid = (s_d.magic == DIAG_MAGIC);
  uint32_t prevBoots = valid ? s_d.boots : 0;
  esp_reset_reason_t rr = esp_reset_reason();

  s_prevResetReason = (uint32_t)rr;
  s_prevBoots = prevBoots + 1;
  if (valid) {
    snprintf(s_prevSummary, sizeof(s_prevSummary),
             "prev stopped at %s(%u) host=%s iters=%u stallMax=%ums x%u "
             "pushMax=%ums online=%u wifi=%u",
             diagCodeName(s_d.code), (unsigned)s_d.arg,
             s_d.host[0] ? s_d.host : "-", (unsigned)s_d.loopIter,
             (unsigned)s_d.stallMax, (unsigned)s_d.stallCnt,
             (unsigned)s_d.pushMax, (unsigned)s_d.online,
             (unsigned)s_d.wifiStatus);
  } else {
    snprintf(s_prevSummary, sizeof(s_prevSummary),
             "no retained state (cold power-on or RTC domain lost)");
  }
  Serial.printf("\ndiag: boot #%u reset=%d(%s)\n", prevBoots + 1, (int)rr,
                resetReasonName(rr));
  Serial.printf("diag: %s\n", s_prevSummary);

  memset(&s_d, 0, sizeof(s_d));
  s_d.magic       = DIAG_MAGIC;
  s_d.boots       = prevBoots + 1;
  s_d.resetReason = (uint32_t)rr;
  s_d.code        = DIAG_SETUP;
  s_fetchCode     = DIAG_NONE;

  esp_timer_create_args_t args = {};
  args.callback = &stallCb;
  args.name     = "diag";
  esp_timer_create(&args, &s_timer);
  if (s_timer) esp_timer_start_periodic(s_timer, 250 * 1000);
}

void diagLoopTick(uint32_t now, uint32_t iter) {
  uint32_t gap = (uint32_t)(now - s_lastTick);
  s_lastTick = now;
  s_d.loopMs   = now;
  s_d.loopIter = iter;

  if (iter > 1) {
    if (gap > s_iterMaxGap) s_iterMaxGap = gap;
    if (gap > 250) {
      emit("diag: slow loop iter %ums bc=%s(%u) host=%s\n", (unsigned)gap,
           diagCodeName(s_d.code), (unsigned)s_d.arg,
           s_d.host[0] ? s_d.host : "-");
    }
  }
  // Repeat the previous-run summary once the USB port has (re)settled — the
  // line printed in diagBoot() runs before the host can reopen the CDC port
  // after a reset, so it is otherwise lost.
  if (!s_bootPrinted && now >= 3000) {
    s_bootPrinted = true;
    Serial.printf("diag: boot #%u reset=%u(%s) — %s\n", (unsigned)s_prevBoots,
                  (unsigned)s_prevResetReason,
                  resetReasonName((esp_reset_reason_t)s_prevResetReason),
                  s_prevSummary);
  }
  if ((uint32_t)(now - s_periodic) >= 10000) {
    s_periodic = now;
    Serial.printf("diag: t=%us iter=%u maxIterGap=%ums pushMax=%ums busy=%d "
                  "job=%s age=%lums fbc=%s host=%s jobsQ=%u resQ=%u heap=%u "
                  "psram=%u online=%u wifi=%u\n",
                  (unsigned)(now / 1000), (unsigned)iter,
                  (unsigned)s_iterMaxGap, (unsigned)s_d.pushMax,
                  fetchTaskBusy() ? 1 : 0, fetchJobName(fetchTaskCurrentJob()),
                  (unsigned long)fetchTaskJobAgeMs(now),
                  diagCodeName(s_fetchCode), s_d.host[0] ? s_d.host : "-",
                  (unsigned)fetchTaskPendingJobs(),
                  (unsigned)fetchTaskPendingResults(),
                  (unsigned)esp_get_free_heap_size(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)s_d.online, (unsigned)s_d.wifiStatus);
    s_iterMaxGap = 0;
  }
}
