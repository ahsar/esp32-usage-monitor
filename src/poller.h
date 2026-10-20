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
  uint32_t forecastMs;
  uint32_t moonMs;
  uint32_t airMs;
  uint32_t backoffBaseMs;   // first failure: this, then ×2 per further streak
  uint32_t backoffUsageMs;  // cooldown cap for the usage meter
  uint32_t backoffOtherMs;  // cooldown cap for weather/moon/air
  uint32_t jobDeadlineMs;   // wedged-worker threshold
  uint32_t wedgePauseMs;    // pause after the first wedge
  uint32_t wedgePauseMaxMs; // …and after any later one
  uint32_t roamFails;       // consecutive transport failures → re-select the AP
  uint32_t roamCooldownMs;  // minimum gap between AP re-selections
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

// Apply the failure/backoff policy to a harvested job. `transportOk` is false
// when the fetch died on the path (DNS/TCP/TLS/read — see httpTransportFailed())
// rather than getting an answer it didn't like; it feeds the roaming trigger
// below and never the backoff, which does not care why the job failed.
//
// A failure also re-arms the job's timer, so the cooldown — not the (much
// longer) interval — paces the retry: one blip costs a minute of weather, not
// half an hour. The cooldown is unchanged and still gates the retry.
PollerOutcome pollerResult(FetchJob job, bool ok, bool transportOk, uint32_t now);

// AP re-selection ("roaming"), asked for when the path has been dead for
// `roamFails` consecutive fetches with no success in between. An AP that
// associates and then carries nothing is the failure this exists for, and it is
// deliberately *not* rssi-based: in the 2026-10-15 sample the dead AP and the
// good one sat in the same rssi band (-66…-73 against -59…-69), so a threshold
// would have kept the dead one. A success — any answer from a server — clears
// the streak, because it proves the AP is carrying traffic. `now` must come from
// the same clock as the cooldown; loop() calls pollerRoamAttempted() whether or
// not the actor managed to move the station, so a hopeless scan cannot turn into
// a scan per loop iteration.
bool pollerRoamDue(uint32_t now);
void pollerRoamAttempted(uint32_t now);

// Force every poller due now (used right after WiFi associates).
void pollerForceAll();

// Force one job due now *and* clear its backoff, for a manual refresh (a tap on
// the status bar). The cooldown exists to protect a dead host from the
// scheduler, not from a person who has just decided the screen is stale — but
// the caller rate-limits it so a tap cannot become a poll storm.
void pollerForce(FetchJob job, uint32_t now);

// screenWake(): force a usage poll when the meter has gone stale, rate-limited
// so a chatty status webhook cannot spam fetches.
void pollerForceUsageIfStale(uint32_t now, uint32_t staleMs);

// Keep a poller out of rotation for `ms` (wedge recovery / backoff).
void pollerPauseFor(FetchJob job, uint32_t now, uint32_t ms);

// Keep *every* poller out of rotation for `ms`; `ms == 0` clears the pause.
// Used while an OTA owns the radio (see ota.h): it goes through the same
// cooldown gate as a backoff, so it also covers the forced paths
// (pollerForceAll(), pollerForceUsageIfStale()) without the callers knowing.
void pollerPauseAll(uint32_t now, uint32_t ms);

// True when a job this old is stuck inside an SDK call no timeout covers.
bool pollerJobWedged(uint32_t ageMs);

// Apply the wedge recovery ladder: wedgePauseMs for the first wedge,
// wedgePauseMaxMs if it recurs. Bumps the ladder and returns the pause the
// caller should apply to the offending poller.
uint32_t pollerWedgePauseMs();

// True while `job` is still backed off at `now` (and clears an expired one).
bool pollerCoolingDown(FetchJob job, uint32_t now);
