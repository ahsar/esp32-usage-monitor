#pragma once
// Network poll scheduling + failure policy.
//
// Pure logic: no hardware, no FreeRTOS, no HTTP, no config type. loop() asks
// which job is due and hands it to the fetch task; the fetch task's own
// liveness (a wedged worker, restarts) stays in main.cpp, which is the only
// place that talks to fetchTask*. Keeping the policy here is what makes it
// host-testable — it used to be a knot of statics inside main.cpp.
#include <stdint.h>
#include "data_types.h"    // FetchJob
#include "util/interval.h" // Interval

struct PollerConfig {
  uint32_t usageActiveMs;   // poll_usage_s
  uint32_t usageIdleMs;     // poll_usage_idle_s
  uint32_t weatherMs;
  uint32_t moonMs;
  uint32_t airMs;
  uint32_t backoffBaseMs;   // first failure: this, then ×2 per further streak
  uint32_t backoffUsageMs;  // cooldown cap for the usage meter
  uint32_t backoffOtherMs;  // cooldown cap for weather/moon/air
  uint32_t jobDeadlineMs;   // wedged-worker threshold
  uint32_t wedgePauseMs;    // pause after the first wedge
  uint32_t wedgePauseMaxMs; // …and after any later one
};

// Apply the periods + policy numbers. Call once from setup().
void pollerInit(const PollerConfig& cfg);

// Switch the usage meter between the active and idle period. loop() calls this
// every iteration; any activity puts it back on the active period.
void pollerSetUsageIdle(bool idle);

// Most overdue poller that is not cooling down, or false when nothing is due.
// The gate flags are the caller's preconditions (a usable OAuth token for
// usage, a configured waqi token for air) so the policy never reads config.
// Pure: it marks nothing run — call pollerStarted() once the fetch task has
// actually accepted the job.
bool pollerSelect(uint32_t now, bool tokensValid, bool hasAirToken, FetchJob* outJob);

// Mark a job's timer as run. Call only after fetchTaskStart() returned true,
// so a busy worker retries the same job on a later iteration.
void pollerStarted(FetchJob job, uint32_t now);

struct PollerOutcome {
  uint32_t cooldownMs;   // 0 on success
  uint32_t streak;       // consecutive failures (0 on success)
};

// Apply the failure/backoff policy to a harvested job.
PollerOutcome pollerResult(FetchJob job, bool ok, uint32_t now);

// Force every poller due now (used right after WiFi associates).
void pollerForceAll();

// screenWake(): force a usage poll when the meter has gone stale, rate-limited
// so a chatty status webhook cannot spam fetches.
void pollerForceUsageIfStale(uint32_t now, uint32_t staleMs);

// Keep a poller out of rotation for `ms` (wedge recovery / backoff).
void pollerPauseFor(FetchJob job, uint32_t now, uint32_t ms);

// True when a job this old is stuck inside an SDK call no timeout covers.
bool pollerJobWedged(uint32_t ageMs);

// Apply the wedge recovery ladder: wedgePauseMs for the first wedge,
// wedgePauseMaxMs if it recurs. Bumps the ladder and returns the pause the
// caller should apply to the offending poller.
uint32_t pollerWedgePauseMs();

// True while `job` is still backed off at `now` (and clears an expired one).
bool pollerCoolingDown(FetchJob job, uint32_t now);
