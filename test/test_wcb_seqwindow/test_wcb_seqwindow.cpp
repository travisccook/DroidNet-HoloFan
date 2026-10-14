#include <unity.h>
#include "WcbSeqWindow.h"

using namespace wcb;

void setUp() {}
void tearDown() {}

void test_first_seq_is_accepted_then_dup() {
  SeqWindow w;
  TEST_ASSERT_FALSE(w.isDup(5));
  TEST_ASSERT_TRUE(w.isDup(5));
}

void test_newer_accepted() {
  SeqWindow w;
  w.isDup(5);
  TEST_ASSERT_FALSE(w.isDup(6));
  TEST_ASSERT_FALSE(w.isDup(9));
  TEST_ASSERT_TRUE(w.isDup(6));                    // remembered below the high
  TEST_ASSERT_TRUE(w.isDup(5));
}

void test_out_of_order_inside_window() {
  SeqWindow w;
  w.isDup(10);
  TEST_ASSERT_FALSE(w.isDup(8));                   // older, first time
  TEST_ASSERT_TRUE(w.isDup(8));                    // now a dup
  TEST_ASSERT_FALSE(w.isDup(9));
}

void test_big_jump_clears_mask() {
  SeqWindow w;
  w.isDup(1);
  TEST_ASSERT_FALSE(w.isDup(40));                  // jump >= 32
  TEST_ASSERT_FALSE(w.isDup(20));                  // inside new window, not seen
}

void test_too_old_is_accepted() {
  SeqWindow w;
  w.isDup(100);
  TEST_ASSERT_FALSE(w.isDup(50));                  // 50 behind: cannot remember, accept
}

void test_wraps_from_65535_to_1() {
  SeqWindow w;
  w.isDup(65535);
  TEST_ASSERT_FALSE(w.isDup(1));                   // senders skip 0; 1 is newer
  TEST_ASSERT_TRUE(w.isDup(65535));
}

// Regression (WCB_Client 1.17.1): a rebooted sender restarts at seq 1 and its
// new commands must not be dropped against the pre-reboot window.
void test_reset_accepts_reused_sequence_numbers() {
  SeqWindow w;
  for (uint16_t s = 1; s <= 20; s++) w.isDup(s);
  w.reset();
  TEST_ASSERT_FALSE(w.isDup(3));
  TEST_ASSERT_FALSE(w.isDup(4));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_seq_is_accepted_then_dup);
  RUN_TEST(test_newer_accepted);
  RUN_TEST(test_out_of_order_inside_window);
  RUN_TEST(test_big_jump_clears_mask);
  RUN_TEST(test_too_old_is_accepted);
  RUN_TEST(test_wraps_from_65535_to_1);
  RUN_TEST(test_reset_accepts_reused_sequence_numbers);
  return UNITY_END();
}
