#pragma once
// Background HTTPS fetch task.
//
// Every payload fetch is blocking: DNS + TCP + TLS connect cannot be pumped
// (client.connect() has no callback), so running them from loop() froze input
// and rendering for seconds — most visibly on wake, where screenWake() forces
// a usage fetch exactly when a finger lands on the panel (and after a long
// idle all four pollers fire back to back). The whole fetch now runs on a
// dedicated FreeRTOS task; loop() only schedules jobs and harvests results.
//
// Threading contract:
//   - the fetch task owns the OAuth tokens and all apiFetch* scratch state;
//   - loop() owns every data holder that reaches the UI (s_usage, s_weather,
//     ...) and is the only task that touches the display, I2C or LittleFS;
//   - one job goes out through a 1-slot queue and one result comes back
//     through another, so at most one fetch is ever in flight and no payload
//     is shared between the tasks while it runs.
#include <stdint.h>
#include "config.h"
#include "data_types.h"
#include "token_store.h"

// The caller's current data for a job. Seeds the fetch so a failed poll still
// marks weather/moon/air `stale` against what is actually on screen — the
// fetchers only set that flag when they had data to lose.
struct FetchSeed {
  UsageData   usage;
  WeatherData weather;
  MoonData    moon;
  AirData     air;
};

// One finished job. `ok` false = fetch/parse failed; the payload then carries
// the seed back (plus stale flags) and must not be treated as new data.
struct FetchResult {
  FetchJob   job;
  bool       ok;
  UsageData   usage;
  WeatherData weather;
  MoonData    moon;
  AirData     air;
};

// Create the worker task and initialise the token store. Call once from setup()
// (before WiFi needs it). Logs and continues when the task cannot be created;
// fetchTaskStart() then fails and polling simply stays silent.
void fetchTaskBegin(const AppConfig& cfg);

// Mirror of the worker's token state (a usable refresh token is present).
bool fetchTaskTokensValid();

// True from a successful fetchTaskStart() until that job is harvested.
bool fetchTaskBusy();

// Wall-clock age of the job in flight, 0 when idle. The main loop uses it to
// spot a worker that is stuck inside an SDK call no timeout covers (see
// fetchTaskRestart()).
uint32_t fetchTaskJobAgeMs(uint32_t now);

// Which job is in flight (FETCH_USAGE when idle / unknown).
FetchJob fetchTaskCurrentJob();

// Human-readable job name for logs.
const char* fetchJobName(FetchJob job);

// Kill and recreate a wedged worker, clearing the busy flag. The task's copy of
// the tokens lives in a file-scope static (not on the task stack) and a pending
// token hand-off is preserved, so tokens survive. The abandoned task's socket
// and mbedtls contexts leak (there is no way to interrupt the SDK call), which
// is why the caller must also back the offending host off.
void fetchTaskRestart();

// Hand a job to the worker. False when a job is already in flight or the task
// is unavailable — the caller must then leave the poll timer un-ran so the
// job is retried on a later iteration.
bool fetchTaskStart(FetchJob job, const FetchSeed& seed);

// Take the finished job, if any. The caller copies the payload into its own
// holders and only advances its change key when `ok` is set.
bool fetchTaskHarvest(FetchResult& out);

// TEMP DIAG: queue occupancy for the freeze stall reporter (diag.cpp).
uint32_t fetchTaskPendingJobs();
uint32_t fetchTaskPendingResults();

// Rotated-token hand-off: the worker never writes to LittleFS (loop() reads
// files from it — the 555 artwork — and LittleFS is not thread-safe). Non-null
// = persist this snapshot, then call fetchTaskSaveDone().
const TokenSet* fetchTaskPendingSave();
void fetchTaskSaveDone();
