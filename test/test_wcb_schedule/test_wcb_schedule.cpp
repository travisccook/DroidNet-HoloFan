#include <unity.h>
#include "WcbSchedule.h"

using namespace wcb;

void setUp() {}
void tearDown() {}

void test_boot_burst_and_heartbeat() {
  Schedule s;
  s.arm(1000, 16, false);
  TEST_ASSERT_EQUAL_HEX8(ACT_BOOT | ACT_HEARTBEAT, s.due(1000));
  TEST_ASSERT_EQUAL_HEX8(0, s.due(2199));
  TEST_ASSERT_EQUAL_HEX8(ACT_BOOT, s.due(2200));
  TEST_ASSERT_EQUAL_HEX8(ACT_BOOT, s.due(3400));
  TEST_ASSERT_EQUAL_HEX8(0, s.due(4600));          // only three announces
  TEST_ASSERT_EQUAL_HEX8(ACT_HEARTBEAT, s.due(11000));
  TEST_ASSERT_EQUAL_HEX8(ACT_HEARTBEAT, s.due(21000));
}

void test_wdp_burst_then_periodic_staggered_by_id() {
  Schedule s;
  s.arm(0, 15, true);                              // stagger (15 % 16) * 500 = 7500
  s.due(0);
  TEST_ASSERT_EQUAL_HEX8(0, s.due(299) & ACT_WDP);
  TEST_ASSERT_EQUAL_HEX8(ACT_WDP, s.due(300) & ACT_WDP);
  TEST_ASSERT_EQUAL_HEX8(ACT_WDP, s.due(1600) & ACT_WDP);
  TEST_ASSERT_EQUAL_HEX8(ACT_WDP, s.due(2900) & ACT_WDP);
  TEST_ASSERT_EQUAL_HEX8(0, s.due(4200) & ACT_WDP);           // burst was 3 adverts
  TEST_ASSERT_EQUAL_HEX8(0, s.due(67499) & ACT_WDP);
  TEST_ASSERT_EQUAL_HEX8(ACT_WDP, s.due(67500) & ACT_WDP);
  TEST_ASSERT_EQUAL_HEX8(ACT_WDP, s.due(127500) & ACT_WDP);
}

void test_no_wdp_without_identity() {
  Schedule s;
  s.arm(0, 15, false);
  for (uint32_t t = 0; t < 200000; t += 100) TEST_ASSERT_EQUAL_HEX8(0, s.due(t) & ACT_WDP);
  s.solicit(5000, 15);
  TEST_ASSERT_EQUAL_HEX8(0, s.due(300000) & ACT_WDP);
}

// Regression (WCB_Client): a SOLICIT must produce an advert, with id jitter.
void test_solicit_pulls_an_advert_forward() {
  Schedule s;
  s.arm(0, 15, true);
  for (uint32_t t = 0; t <= 5000; t += 100) s.due(t);   // burst finished
  s.solicit(5000, 15);                              // jitter 15 * 40 = 600
  TEST_ASSERT_EQUAL_HEX8(0, s.due(5599) & ACT_WDP);
  TEST_ASSERT_EQUAL_HEX8(ACT_WDP, s.due(5600) & ACT_WDP);
  TEST_ASSERT_EQUAL_HEX8(0, s.due(5700) & ACT_WDP);
}

void test_rearm_restarts_boot_burst() {
  Schedule s;
  s.arm(0, 16, true);
  for (uint32_t t = 0; t <= 20000; t += 100) s.due(t);
  s.arm(30000, 16, true);
  TEST_ASSERT_EQUAL_HEX8(ACT_BOOT | ACT_HEARTBEAT, s.due(30000));
  TEST_ASSERT_EQUAL_HEX8(ACT_WDP, s.due(30300) & ACT_WDP);
}

// Bench knob (HoloFan #HLFK, 0.9.8): another heartbeat period from the next
// beat on; 0 restores HEARTBEAT_MS. Out-of-range periods are clamped.
void test_heartbeat_period_can_be_changed() {
  Schedule s;
  s.arm(0, 16, false);
  TEST_ASSERT_EQUAL_HEX8(ACT_BOOT | ACT_HEARTBEAT, s.due(0));
  TEST_ASSERT_EQUAL(HEARTBEAT_MS, s.heartbeatMs());
  s.setHeartbeatMs(7000);
  TEST_ASSERT_EQUAL_HEX8(ACT_HEARTBEAT, s.due(10000) & ACT_HEARTBEAT);   // the beat already scheduled
  TEST_ASSERT_EQUAL_HEX8(0, s.due(16999) & ACT_HEARTBEAT);
  TEST_ASSERT_EQUAL_HEX8(ACT_HEARTBEAT, s.due(17000) & ACT_HEARTBEAT);
  TEST_ASSERT_EQUAL_HEX8(ACT_HEARTBEAT, s.due(24000) & ACT_HEARTBEAT);
  s.setHeartbeatMs(0);
  TEST_ASSERT_EQUAL(HEARTBEAT_MS, s.heartbeatMs());
  s.setHeartbeatMs(500);
  TEST_ASSERT_EQUAL(HEARTBEAT_MIN_MS, s.heartbeatMs());
  s.setHeartbeatMs(30000);
  TEST_ASSERT_EQUAL(HEARTBEAT_MAX_MS, s.heartbeatMs());
}

// arm() (boot, rejoin) keeps a changed period.
void test_heartbeat_period_survives_rearm() {
  Schedule s;
  s.setHeartbeatMs(13000);
  s.arm(0, 16, false);
  s.due(0);
  TEST_ASSERT_EQUAL_HEX8(0, s.due(12999) & ACT_HEARTBEAT);
  TEST_ASSERT_EQUAL_HEX8(ACT_HEARTBEAT, s.due(13000) & ACT_HEARTBEAT);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_burst_and_heartbeat);
  RUN_TEST(test_wdp_burst_then_periodic_staggered_by_id);
  RUN_TEST(test_no_wdp_without_identity);
  RUN_TEST(test_solicit_pulls_an_advert_forward);
  RUN_TEST(test_rearm_restarts_boot_burst);
  RUN_TEST(test_heartbeat_period_can_be_changed);
  RUN_TEST(test_heartbeat_period_survives_rearm);
  return UNITY_END();
}
