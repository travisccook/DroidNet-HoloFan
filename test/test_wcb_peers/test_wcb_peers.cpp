#include <unity.h>
#include "WcbPeers.h"

using namespace wcb;

void setUp() {}
void tearDown() {}

void test_seen_goes_online_then_offline_after_50s() {
  PeerTable t;
  TEST_ASSERT_FALSE(t.isOnline(2));
  TEST_ASSERT_FALSE(t.everSeen(2));
  t.markSeen(2, 1000);
  TEST_ASSERT_TRUE(t.isOnline(2));
  t.sweep(1000 + 50000);
  TEST_ASSERT_TRUE(t.isOnline(2));                 // exactly 50 s: still online
  t.sweep(1000 + 50001);
  TEST_ASSERT_FALSE(t.isOnline(2));
  TEST_ASSERT_TRUE(t.everSeen(2));
}

// Regression (WCB_Client 1.17.1): every node is swept, so a node above the
// floor that went quiet can never stay "online" forever.
void test_every_id_is_swept() {
  PeerTable t;
  t.markSeen(19, 0);
  t.sweep(60000);
  TEST_ASSERT_FALSE(t.isOnline(19));
}

void test_sweep_handles_millis_rollover() {
  PeerTable t;
  t.markSeen(3, 0xFFFFFF00u);
  t.sweep(0x00000100u);                            // 512 ms later, across the wrap
  TEST_ASSERT_TRUE(t.isOnline(3));
}

void test_dupe_window_per_sender_and_seq0_never_dup() {
  PeerTable t;
  TEST_ASSERT_FALSE(t.isDupCommand(2, 7));
  TEST_ASSERT_TRUE(t.isDupCommand(2, 7));
  TEST_ASSERT_FALSE(t.isDupCommand(3, 7));         // different sender
  TEST_ASSERT_FALSE(t.isDupCommand(2, 0));
  TEST_ASSERT_FALSE(t.isDupCommand(2, 0));
}

void test_boot_clears_window_once_per_hold() {
  PeerTable t;
  t.isDupCommand(2, 5);
  t.onBoot(2, 10000);                              // first announce: clear
  TEST_ASSERT_FALSE(t.isDupCommand(2, 5));
  t.onBoot(2, 11200);                              // second announce of the same boot: keep
  TEST_ASSERT_TRUE(t.isDupCommand(2, 5));          // a retried command stays deduped
  t.onBoot(2, 14001);                              // > 4 s after the first: a new boot
  TEST_ASSERT_FALSE(t.isDupCommand(2, 5));
}

void test_registration_flags_and_reset() {
  PeerTable t;
  t.setRegistered(4, true);
  t.markSeen(4, 1);
  TEST_ASSERT_TRUE(t.registered(4));
  t.reset();
  TEST_ASSERT_FALSE(t.registered(4));
  TEST_ASSERT_FALSE(t.isOnline(4));
  TEST_ASSERT_FALSE(t.everSeen(4));
}

void test_out_of_range_ids_are_ignored() {
  PeerTable t;
  t.markSeen(0, 1);
  t.markSeen(21, 1);
  t.setRegistered(21, true);
  t.onBoot(0, 1);
  TEST_ASSERT_FALSE(t.isOnline(0));
  TEST_ASSERT_FALSE(t.isOnline(21));
  TEST_ASSERT_FALSE(t.registered(21));
  TEST_ASSERT_FALSE(t.isDupCommand(21, 5));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_seen_goes_online_then_offline_after_50s);
  RUN_TEST(test_every_id_is_swept);
  RUN_TEST(test_sweep_handles_millis_rollover);
  RUN_TEST(test_dupe_window_per_sender_and_seq0_never_dup);
  RUN_TEST(test_boot_clears_window_once_per_hold);
  RUN_TEST(test_registration_flags_and_reset);
  RUN_TEST(test_out_of_range_ids_are_ignored);
  return UNITY_END();
}
