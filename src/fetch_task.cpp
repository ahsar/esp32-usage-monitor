#include "fetch_task.h"
#include "api_client.h"
#include "diag.h"
#include "token_store.h"
#include "weather_client.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

// Stack: the mbedTLS handshake runs deep (cert-chain parsing), so this is well
// above the 4 KB default; 16 KB is cheap next to the framebuffers in PSRAM and
// the high-water mark is printed after the first job so it can be verified on
// hardware. Priority 1 == the Arduino loop task: equal priority means FreeRTOS
// time-slices between them, so even a CPU-bound TLS handshake only ever takes
// ticks away from input scanning/rendering instead of starving them.
static const uint32_t   FETCH_STACK   = 16384;
static const UBaseType_t FETCH_PRIO   = 1;

struct FetchJobMsg {
  FetchJob  job;
  FetchSeed seed;
};

static TokenSet      s_tokens;
static volatile bool s_tokensValid = false;   // mirror for the main task
static char          s_waqi[64];

static QueueHandle_t s_jobs    = nullptr;
static QueueHandle_t s_results = nullptr;
static volatile bool s_busy    = false;
static bool          s_taskOk  = false;   // worker alive (else jobs would wedge)
static TaskHandle_t  s_task    = nullptr;
static volatile uint32_t s_jobStart = 0;  // millis() of the in-flight job (0 = idle)
static FetchJob      s_current = FETCH_USAGE;

// Token hand-off (flash write happens on the main task, see fetchTaskPendingSave).
static TokenSet      s_saveSnap;
static volatile bool s_saveReady   = false;
static bool          s_reportedHWM = false;

const char* fetchJobName(FetchJob j) {
  switch (j) {
    case FETCH_USAGE:   return "usage";
    case FETCH_WEATHER: return "weather";
    case FETCH_MOON:    return "moon";
    case FETCH_AIR:     return "air";
  }
  return "?";
}

// Publish rotated tokens to the main task for the flash write. The snapshot
// must not be refilled while the main task may still be reading it, so wait
// for the previous hand-off to drain first. loop() never blocks on this task,
// so the wait cannot deadlock.
static void handOffTokens() {
  if (!s_tokens.dirty) return;
  while (s_saveReady) vTaskDelay(pdMS_TO_TICKS(5));
  memcpy(&s_saveSnap, &s_tokens, sizeof(TokenSet));
  s_tokens.dirty = false;
  __sync_synchronize();          // snapshot fully written before the flag
  s_saveReady = true;
}

static void runJob(const FetchJobMsg& m) {
  FetchResult r;
  memset(&r, 0, sizeof(r));
  r.job     = m.job;
  r.usage   = m.seed.usage;
  r.weather = m.seed.weather;
  r.moon    = m.seed.moon;
  r.air     = m.seed.air;

  diagMark(DIAG_FETCH_JOB, (uint32_t)m.job);
  uint32_t t0 = millis();
  switch (m.job) {
    case FETCH_USAGE:
      r.ok = apiFetchUsage(r.usage, s_tokens);
      s_tokensValid = s_tokens.valid;
      handOffTokens();
      break;
    case FETCH_WEATHER: r.ok = apiFetchWeather(r.weather); break;
    case FETCH_MOON:    r.ok = apiFetchMoon(r.moon); break;
    case FETCH_AIR:     r.ok = apiFetchAir(r.air, s_waqi); break;
  }
  // The duration makes the old failure mode visible: a fetch that used to
  // stall loop() for this long is now invisible to the UI.
  Serial.printf("fetch %s: %s in %lums (stackHWM=%u)\n", fetchJobName(m.job),
                r.ok ? "ok" : "fail", (unsigned long)(millis() - t0),
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
  if (!s_reportedHWM) {
    s_reportedHWM = true;
  }
  diagMark(DIAG_FETCH_DONE, (uint32_t)m.job);
  xQueueOverwrite(s_results, &r);
}

static void fetchTaskFn(void*) {
  FetchJobMsg m;
  for (;;) {
    if (xQueueReceive(s_jobs, &m, portMAX_DELAY) == pdTRUE) runJob(m);
  }
}

void fetchTaskBegin(const AppConfig& cfg) {
  strlcpy(s_waqi, cfg.waqi_token, sizeof(s_waqi));
  if (!tokenStoreInit(s_tokens, cfg.refresh_token,
                      cfg.org_id[0] ? cfg.org_id : cfg.workspace_id)) {
    Serial.println("No refresh token; run tools/authorize.py to authorise");
  }
  s_tokensValid = s_tokens.valid;

  s_jobs    = xQueueCreate(1, sizeof(FetchJobMsg));
  s_results = xQueueCreate(1, sizeof(FetchResult));
  if (!s_jobs || !s_results) {
    Serial.println("fetch: queue alloc failed; polling disabled");
    return;
  }
  if (xTaskCreate(fetchTaskFn, "fetch", FETCH_STACK, nullptr, FETCH_PRIO,
                  &s_task) != pdPASS) {
    Serial.println("fetch: task create failed; polling disabled");
    return;
  }
  s_taskOk  = true;
  s_jobStart = 0;
}

bool fetchTaskTokensValid() { return s_tokensValid; }
bool fetchTaskBusy()        { return s_busy; }
FetchJob fetchTaskCurrentJob() { return s_current; }

uint32_t fetchTaskJobAgeMs(uint32_t now) {
  uint32_t start = s_jobStart;
  return start ? (uint32_t)(now - start) : 0;
}

bool fetchTaskStart(FetchJob job, const FetchSeed& seed) {
  if (!s_taskOk || s_busy) return false;
  FetchJobMsg m;
  m.job  = job;
  m.seed = seed;
  if (xQueueSend(s_jobs, &m, 0) != pdTRUE) return false;
  s_current  = job;
  s_jobStart = millis();
  s_busy     = true;              // cleared by fetchTaskHarvest()
  return true;
}

bool fetchTaskHarvest(FetchResult& out) {
  if (!s_results || xQueueReceive(s_results, &out, 0) != pdTRUE) return false;
  s_busy     = false;
  s_jobStart = 0;
  return true;
}

// Recreate the worker after it stopped making progress. The in-flight job's
// socket/mbedtls state cannot be reclaimed (the SDK call is uninterruptible),
// so this deliberately leaks it once; the caller backs the offending host off
// so it does not become a loop. Tokens survive: s_tokens is a file-scope static
// and a pending hand-off (s_saveSnap/s_saveReady) is left untouched for
// serviceFetch() to write.
void fetchTaskRestart() {
  if (s_task) {
    vTaskDelete(s_task);
    s_task = nullptr;
  }
  if (s_jobs)    { vQueueDelete(s_jobs);    s_jobs    = nullptr; }
  if (s_results) { vQueueDelete(s_results); s_results = nullptr; }
  s_busy     = false;
  s_jobStart = 0;
  s_taskOk   = false;

  s_jobs    = xQueueCreate(1, sizeof(FetchJobMsg));
  s_results = xQueueCreate(1, sizeof(FetchResult));
  if (!s_jobs || !s_results) {
    Serial.println("fetch: queue alloc failed after restart; polling disabled");
    return;
  }
  if (xTaskCreate(fetchTaskFn, "fetch", FETCH_STACK, nullptr, FETCH_PRIO,
                  &s_task) != pdPASS) {
    Serial.println("fetch: task create failed after restart; polling disabled");
    return;
  }
  s_taskOk = true;
}

const TokenSet* fetchTaskPendingSave() {
  return s_saveReady ? &s_saveSnap : nullptr;
}

void fetchTaskSaveDone() {
  __sync_synchronize();           // flash write finished before a refill
  s_saveReady = false;
}

// TEMP DIAG: queue occupancy for the stall reporter.
uint32_t fetchTaskPendingJobs() {
  return s_jobs ? (uint32_t)uxQueueMessagesWaiting(s_jobs) : 0;
}
uint32_t fetchTaskPendingResults() {
  return s_results ? (uint32_t)uxQueueMessagesWaiting(s_results) : 0;
}
