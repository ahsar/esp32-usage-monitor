// Host-side unit tests for the network poll scheduler / failure policy
// (src/poller.cpp).
//
//   pio test -e native
//
// This logic used to be a knot of statics inside main.cpp and could only be
// verified by staring at it; extracting it here is what makes it testable.
#include <unity.h>
#include "poller.h"

static PollerConfig testCfg() {
  return PollerConfig{
      /* usageActiveMs    */ 60000,
      /* usageIdleMs      */ 180000,
      /* weatherMs        */ 1800000,
      /* moonMs           */ 43200000,
      /* airMs            */ 3600000,
      /* backoffBaseMs    */ 60000,
      /* backoffUsageMs   */ 120000,
      /* backoffOtherMs   */ 1800000,
      /* jobDeadlineMs    */ 240000,
      /* wedgePauseMs     */ 300000,
      /* wedgePauseMaxMs  */ 1800000,
  };
}

// Every test starts from a clean slate; pollerInit() drops all latches.
void setUp() { pollerInit(testCfg()); }
void tearDown() {}

// Arm every non-usage poller so a test can watch the usage timer on its own.
// (pollerInit() leaves them all pending, exactly like a fresh boot.)
static void parkOthers(uint32_t now) {
  pollerStarted(FETCH_WEATHER, now);
  pollerStarted(FETCH_MOON, now);
  pollerStarted(FETCH_AIR, now);
}

// ---- selection priority + gates -------------------------------------------

void test_usage_is_selected_first(void) {
  FetchJob job = FETCH_WEATHER;
  TEST_ASSERT_TRUE(pollerSelect(0, /*tokensValid=*/true, /*hasAirToken=*/true, &job));
  TEST_ASSERT_EQUAL(FETCH_USAGE, job);
}

void test_usage_gated_on_tokens(void) {
  FetchJob job;
  TEST_ASSERT_TRUE(pollerSelect(0, /*tokensValid=*/false, /*hasAirToken=*/true, &job));
  TEST_ASSERT_EQUAL(FETCH_WEATHER, job);
}

void test_priority_order_and_air_gate(void) {
  FetchJob job;
  TEST_ASSERT_TRUE(pollerSelect(0, true, false, &job)); TEST_ASSERT_EQUAL(FETCH_USAGE, job);
  pollerStarted(job, 0);
  TEST_ASSERT_TRUE(pollerSelect(0, true, false, &job)); TEST_ASSERT_EQUAL(FETCH_WEATHER, job);
  pollerStarted(job, 0);
  TEST_ASSERT_TRUE(pollerSelect(0, true, false, &job)); TEST_ASSERT_EQUAL(FETCH_MOON, job);
  pollerStarted(job, 0);
  // Nothing left: air needs a token.
  TEST_ASSERT_FALSE(pollerSelect(0, true, false, &job));
  TEST_ASSERT_TRUE(pollerSelect(0, true, true, &job));
  TEST_ASSERT_EQUAL(FETCH_AIR, job);
}

// ---- timers ---------------------------------------------------------------

void test_started_arms_timer(void) {
  FetchJob job;
  TEST_ASSERT_TRUE(pollerSelect(0, true, true, &job));
  TEST_ASSERT_EQUAL(FETCH_USAGE, job);
  pollerStarted(job, 0);

  // The other three are still pending, so they drain before usage comes due.
  TEST_ASSERT_TRUE(pollerSelect(30000, true, true, &job)); TEST_ASSERT_EQUAL(FETCH_WEATHER, job);
  pollerStarted(job, 30000);
  TEST_ASSERT_TRUE(pollerSelect(30000, true, true, &job)); TEST_ASSERT_EQUAL(FETCH_MOON, job);
  pollerStarted(job, 30000);
  TEST_ASSERT_TRUE(pollerSelect(30000, true, true, &job)); TEST_ASSERT_EQUAL(FETCH_AIR, job);
  pollerStarted(job, 30000);

  TEST_ASSERT_FALSE(pollerSelect(30000, true, true, &job));
  TEST_ASSERT_TRUE(pollerSelect(60000, true, true, &job));
  TEST_ASSERT_EQUAL(FETCH_USAGE, job);
}

void test_usage_idle_period(void) {
  parkOthers(0);
  FetchJob job;
  TEST_ASSERT_TRUE(pollerSelect(0, true, false, &job));
  pollerStarted(job, 0);

  TEST_ASSERT_TRUE(pollerSelect(60000, true, false, &job));   // active period
  TEST_ASSERT_EQUAL(FETCH_USAGE, job);
  pollerStarted(job, 60000);

  pollerSetUsageIdle(true);
  TEST_ASSERT_FALSE(pollerSelect(120000, true, false, &job));  // not due until 180 s
  TEST_ASSERT_TRUE(pollerSelect(240000, true, false, &job));
  TEST_ASSERT_EQUAL(FETCH_USAGE, job);
}

void test_force_usage_if_stale_is_rate_limited(void) {
  parkOthers(0);
  FetchJob job;
  TEST_ASSERT_TRUE(pollerSelect(0, true, false, &job));
  pollerStarted(job, 0);

  pollerForceUsageIfStale(20000, 30000);   // only 20 s stale → left alone
  TEST_ASSERT_FALSE(pollerSelect(20000, true, false, &job));

  pollerForceUsageIfStale(40000, 30000);   // 40 s stale → forced
  TEST_ASSERT_TRUE(pollerSelect(40000, true, false, &job));
  TEST_ASSERT_EQUAL(FETCH_USAGE, job);
}

// ---- backoff / cooldown ---------------------------------------------------

void test_backoff_usage_cap(void) {
  PollerOutcome oc = pollerResult(FETCH_USAGE, false, 1000);
  TEST_ASSERT_EQUAL_UINT32(60000, oc.cooldownMs);
  TEST_ASSERT_EQUAL_UINT32(1, oc.streak);

  oc = pollerResult(FETCH_USAGE, false, 1000);
  TEST_ASSERT_EQUAL_UINT32(120000, oc.cooldownMs);   // 2× base, at the cap
  oc = pollerResult(FETCH_USAGE, false, 1000);
  TEST_ASSERT_EQUAL_UINT32(120000, oc.cooldownMs);   // stays capped
  TEST_ASSERT_EQUAL_UINT32(3, oc.streak);
}

void test_backoff_other_ladder(void) {
  const uint32_t expect[] = { 60000, 120000, 240000, 480000, 960000, 960000, 960000 };
  for (int i = 0; i < 7; i++) {
    PollerOutcome oc = pollerResult(FETCH_WEATHER, false, 0);
    TEST_ASSERT_EQUAL_UINT32(expect[i], oc.cooldownMs);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(i + 1), oc.streak);
  }
}

void test_cooldown_blocks_then_expires(void) {
  pollerResult(FETCH_USAGE, false, 0);   // backed off until 60000
  TEST_ASSERT_TRUE(pollerCoolingDown(FETCH_USAGE, 0));

  FetchJob job;
  TEST_ASSERT_TRUE(pollerSelect(0, true, true, &job));
  TEST_ASSERT_EQUAL(FETCH_WEATHER, job);        // usage skipped
  TEST_ASSERT_TRUE(pollerSelect(59000, true, true, &job));
  TEST_ASSERT_EQUAL(FETCH_WEATHER, job);
  TEST_ASSERT_TRUE(pollerSelect(60000, true, true, &job));
  TEST_ASSERT_EQUAL(FETCH_USAGE, job);          // expired → back in rotation
}

void test_success_clears_backoff(void) {
  pollerResult(FETCH_WEATHER, false, 0);
  TEST_ASSERT_TRUE(pollerCoolingDown(FETCH_WEATHER, 0));

  PollerOutcome oc = pollerResult(FETCH_WEATHER, true, 0);
  TEST_ASSERT_EQUAL_UINT32(0, oc.cooldownMs);
  TEST_ASSERT_EQUAL_UINT32(0, oc.streak);
  TEST_ASSERT_FALSE(pollerCoolingDown(FETCH_WEATHER, 0));
}

void test_pause_for(void) {
  pollerPauseFor(FETCH_MOON, 1000, 5000);
  TEST_ASSERT_TRUE(pollerCoolingDown(FETCH_MOON, 1000));
  TEST_ASSERT_TRUE(pollerCoolingDown(FETCH_MOON, 5999));
  TEST_ASSERT_FALSE(pollerCoolingDown(FETCH_MOON, 6000));
}

// ---- wedge ladder ---------------------------------------------------------

void test_wedge_deadline(void) {
  TEST_ASSERT_FALSE(pollerJobWedged(239999));
  TEST_ASSERT_TRUE(pollerJobWedged(240000));
}

void test_wedge_ladder_stretches(void) {
  TEST_ASSERT_EQUAL_UINT32(300000, pollerWedgePauseMs());
  TEST_ASSERT_EQUAL_UINT32(1800000, pollerWedgePauseMs());
  TEST_ASSERT_EQUAL_UINT32(1800000, pollerWedgePauseMs());
}

void test_wedge_ladder_resets_on_success(void) {
  TEST_ASSERT_EQUAL_UINT32(300000, pollerWedgePauseMs());
  pollerResult(FETCH_USAGE, true, 0);
  TEST_ASSERT_EQUAL_UINT32(300000, pollerWedgePauseMs());
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_usage_is_selected_first);
  RUN_TEST(test_usage_gated_on_tokens);
  RUN_TEST(test_priority_order_and_air_gate);
  RUN_TEST(test_started_arms_timer);
  RUN_TEST(test_usage_idle_period);
  RUN_TEST(test_force_usage_if_stale_is_rate_limited);
  RUN_TEST(test_backoff_usage_cap);
  RUN_TEST(test_backoff_other_ladder);
  RUN_TEST(test_cooldown_blocks_then_expires);
  RUN_TEST(test_success_clears_backoff);
  RUN_TEST(test_pause_for);
  RUN_TEST(test_wedge_deadline);
  RUN_TEST(test_wedge_ladder_stretches);
  RUN_TEST(test_wedge_ladder_resets_on_success);
  return UNITY_END();
}
