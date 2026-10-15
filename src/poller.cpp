// Network poll scheduling + failure policy. See poller.h for the contract.
//
// Every bit of state here was lifted verbatim out of main.cpp so the behaviour
// is unchanged: the same `Interval` timers, the same usage → weather → moon →
// air priority, and the same exponential backoff caps.
#include "poller.h"

namespace {

PollerConfig g_cfg;

// Fallback periods, overwritten by pollerInit(). Kept so a timer that somehow
// runs before init() still behaves like the old main.cpp literals.
Interval s_ivUsage(60000);
Interval s_ivWeather(1800000);
Interval s_ivMoon(43200000);
Interval s_ivAir(3600000);

// Indexed by FetchJob (1..FETCH_AIR); slot 0 is unused.
uint32_t s_failStreak[FETCH_AIR + 1];
uint32_t s_cooldownUntil[FETCH_AIR + 1];
uint32_t s_wedgeCount = 0;

Interval* intervalFor(FetchJob job) {
  switch (job) {
    case FETCH_WEATHER: return &s_ivWeather;
    case FETCH_MOON:    return &s_ivMoon;
    case FETCH_AIR:     return &s_ivAir;
    default:            return &s_ivUsage;
  }
}

}  // namespace

void pollerInit(const PollerConfig& cfg) {
  g_cfg = cfg;
  // Also drop every latch, so a fresh init (boot, or a test's setUp) starts from
  // the same state as a freshly constructed timer.
  s_ivUsage   = Interval(cfg.usageActiveMs);
  s_ivWeather = Interval(cfg.weatherMs);
  s_ivMoon    = Interval(cfg.moonMs);
  s_ivAir     = Interval(cfg.airMs);
  for (int j = 1; j <= FETCH_AIR; ++j) {
    s_failStreak[j]    = 0;
    s_cooldownUntil[j] = 0;
  }
  s_wedgeCount = 0;
}

void pollerSetUsageIdle(bool idle) {
  s_ivUsage.periodMs = idle ? g_cfg.usageIdleMs : g_cfg.usageActiveMs;
}

bool pollerCoolingDown(FetchJob job, uint32_t now) {
  if (s_cooldownUntil[job] == 0) return false;
  if ((int32_t)(s_cooldownUntil[job] - now) > 0) return true;
  s_cooldownUntil[job] = 0;   // expired
  return false;
}

bool pollerSelect(uint32_t now, bool tokensValid, bool hasAirToken, FetchJob* outJob) {
  if (tokensValid && s_ivUsage.due(now) && !pollerCoolingDown(FETCH_USAGE, now)) {
    *outJob = FETCH_USAGE;
    return true;
  }
  if (s_ivWeather.due(now) && !pollerCoolingDown(FETCH_WEATHER, now)) {
    *outJob = FETCH_WEATHER;
    return true;
  }
  if (s_ivMoon.due(now) && !pollerCoolingDown(FETCH_MOON, now)) {
    *outJob = FETCH_MOON;
    return true;
  }
  if (hasAirToken && s_ivAir.due(now) && !pollerCoolingDown(FETCH_AIR, now)) {
    *outJob = FETCH_AIR;
    return true;
  }
  return false;
}

void pollerStarted(FetchJob job, uint32_t now) { intervalFor(job)->ran(now); }

PollerOutcome pollerResult(FetchJob job, bool ok, uint32_t now) {
  if (ok) {
    s_failStreak[job]    = 0;
    s_cooldownUntil[job] = 0;
    s_wedgeCount         = 0;
    return { 0, 0 };
  }
  // Exponential backoff from base, ×16 once the streak passes 5, capped per
  // job so a dead endpoint stops taking the single fetch slot every cycle.
  uint32_t streak = ++s_failStreak[job];
  uint32_t cool = g_cfg.backoffBaseMs << (streak > 5 ? 4 : streak - 1);
  uint32_t cap = (job == FETCH_USAGE) ? g_cfg.backoffUsageMs : g_cfg.backoffOtherMs;
  if (cool > cap) cool = cap;
  s_cooldownUntil[job] = now + cool;
  return { cool, streak };
}

void pollerForceAll() {
  s_ivUsage.forcePending();
  s_ivWeather.forcePending();
  s_ivMoon.forcePending();
  s_ivAir.forcePending();
}

void pollerForceUsageIfStale(uint32_t now, uint32_t staleMs) {
  // Only if the meter is not already pending, so a chatty status webhook cannot
  // keep re-arming the same fetch.
  if (!s_ivUsage.pending && (uint32_t)(now - s_ivUsage.lastMs) >= staleMs) {
    s_ivUsage.forcePending();
  }
}

void pollerPauseFor(FetchJob job, uint32_t now, uint32_t ms) {
  s_cooldownUntil[job] = now + ms;
}

bool pollerJobWedged(uint32_t ageMs) { return ageMs >= g_cfg.jobDeadlineMs; }

uint32_t pollerWedgePauseMs() {
  uint32_t pause = (s_wedgeCount == 0) ? g_cfg.wedgePauseMs : g_cfg.wedgePauseMaxMs;
  s_wedgeCount++;
  return pause;
}
