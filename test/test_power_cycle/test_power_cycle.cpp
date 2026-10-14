#include <unity.h>
#include "PowerCycleTrigger.h"

using namespace maint;

void setUp() {}
void tearDown() {}

// One boot as the firmware drives it: power-on reset or not, stored count in,
// polled at the given uptimes. Returns the actions seen; `stored` is updated.
struct Boot {
  PowerCycleTrigger t;
  uint8_t stored;
  bool enter = false, cleared = false, counted = false;
  Boot(bool powerOn, uint8_t st) : stored(st) { t.begin(powerOn); }
  void at(uint32_t ms) {
    switch (t.poll(ms, stored)) {
      case PowerCycleTrigger::Action::COUNT: stored = t.newCount(); counted = true; break;
      case PowerCycleTrigger::Action::ENTER_MAINT: stored = 0; enter = true; break;
      case PowerCycleTrigger::Action::CLEAR: stored = 0; cleared = true; break;
      case PowerCycleTrigger::Action::NONE: break;
    }
  }
};

void test_three_short_power_cycles_enter_maintenance() {
  uint8_t stored = 0;
  for (int i = 1; i <= 2; i++) {
    Boot b(true, stored);
    b.at(1000); b.at(2000); b.at(5000);      // switched off at about 5 s
    TEST_ASSERT_TRUE(b.counted);
    TEST_ASSERT_FALSE(b.enter);
    stored = b.stored;
    TEST_ASSERT_EQUAL(i, stored);
  }
  Boot b(true, stored);
  b.at(2000);
  TEST_ASSERT_TRUE(b.enter);
  TEST_ASSERT_EQUAL(0, b.stored);            // cleared so the next boot is normal
}

// A boot that stays up 10 s is a normal boot: the count starts over.
void test_long_boot_clears_count() {
  Boot c(true, 1);
  c.at(2000);
  TEST_ASSERT_EQUAL(2, c.stored);
  c.at(9999);
  TEST_ASSERT_FALSE(c.cleared);
  c.at(10000);
  TEST_ASSERT_TRUE(c.cleared);
  TEST_ASSERT_EQUAL(0, c.stored);
}

// The power-up brown-out resets the chip within milliseconds: such boots
// never reach 2 s and must not count.
void test_boot_shorter_than_two_seconds_does_not_count() {
  Boot b(true, 1);
  b.at(500); b.at(1999);
  TEST_ASSERT_FALSE(b.counted);
  TEST_ASSERT_EQUAL(1, b.stored);
}

// Software resets (OTA reboot, mesh restart, crash) and brown-outs are not
// power cycles: they neither count nor clear.
void test_non_power_on_reset_ignored() {
  Boot b(false, 2);
  b.at(2000); b.at(10000); b.at(60000);
  TEST_ASSERT_FALSE(b.counted);
  TEST_ASSERT_FALSE(b.enter);
  TEST_ASSERT_FALSE(b.cleared);
  TEST_ASSERT_EQUAL(2, b.stored);
}

// Acts once per boot.
void test_acts_once() {
  Boot b(true, 0);
  b.at(2000); b.at(2001); b.at(3000);
  TEST_ASSERT_EQUAL(1, b.stored);
  b.at(10000); b.at(20000);
  TEST_ASSERT_EQUAL(0, b.stored);
}

// A garbage stored value (corrupt NVS) cannot enter maintenance by itself.
void test_garbage_count_restarts() {
  Boot b(true, 200);
  b.at(2000);
  TEST_ASSERT_FALSE(b.enter);
  TEST_ASSERT_EQUAL(1, b.stored);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_three_short_power_cycles_enter_maintenance);
  RUN_TEST(test_long_boot_clears_count);
  RUN_TEST(test_boot_shorter_than_two_seconds_does_not_count);
  RUN_TEST(test_non_power_on_reset_ignored);
  RUN_TEST(test_acts_once);
  RUN_TEST(test_garbage_count_restarts);
  return UNITY_END();
}
