#pragma once
// ---- TEMPORARY: boot / fetch freeze diagnostics ----------------------------
// Remove once the "boot → fetch → UI frozen" bug is understood.
//
// Two independent tracks, both aimed at failure modes that are otherwise
// invisible from the USB CDC port:
//   * a 1-word breadcrumb + loop heartbeat in RTC no-init memory, so a hang or
//     a silent panic is reported on the *next* boot ("previous run stopped at
//     ..."), and `esp_reset_reason()` distinguishes a crash from a clean boot;
//   * a lock-free stall reporter on the esp_timer task (core 0) that keeps
//     emitting while the main loop is wedged on core 1 — it writes through
//     esp_rom_printf (like the panic handler) so it cannot be blocked by the
//     Arduino Serial mutex the stuck task may be holding.
#include <stdint.h>

enum DiagCode : uint32_t {
  DIAG_NONE = 0,
  DIAG_SETUP,
  DIAG_SETUP_DONE,
  DIAG_LOOP,
  DIAG_UI_RENDER,
  DIAG_UI_PUSH,
  DIAG_FETCH_JOB,
  DIAG_FETCH_DONE,
  DIAG_HTTP_CONNECT,
  DIAG_HTTP_CONNECTED,
  DIAG_HTTP_READ,
  DIAG_HTTP_DONE,
  DIAG_TOKEN_REFRESH,
  DIAG_TOKEN_DONE,
  DIAG_WIFI_CONNECTED,
  DIAG_COUNT,
};

const char* diagCodeName(uint32_t code);

void diagBoot();                              // call first in setup()
void diagMark(DiagCode code, uint32_t arg);   // cheap: two 32-bit RTC stores
void diagMarkHost(const char* host);          // remember which endpoint is in flight
void diagLoopTick(uint32_t now, uint32_t iter);
void diagPushMs(uint32_t ms);                 // render+push duration of one frame
void diagSetNet(bool online, int wifiStatus);

// Where the worker currently is: its own phase breadcrumb (the main loop's
// diagMark calls do not overwrite it) plus the host of the request in flight.
// Printed in the periodic line so a fetch that stops making progress says why.
uint32_t    diagFetchCodeNow();
const char* diagHostNow();
