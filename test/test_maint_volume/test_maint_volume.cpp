#include <unity.h>
#include <string.h>
#include "MaintVolume.h"

using namespace fanlogic;

void setUp() {}
void tearDown() {}

// .noinit after a power cycle: whatever was in RAM.
static MaintVolumeRecord garbage() {
  MaintVolumeRecord r;
  memset(&r, 0xA5, sizeof(r));
  return r;
}

void test_enter_at_3_sets_1_and_leave_puts_3_back() {
  MaintVolumeRecord r = garbage();
  TEST_ASSERT_EQUAL(MAINT_VOLUME, maintVolumeEnter(r, 3));
  TEST_ASSERT_TRUE(maintVolumeValid(r));
  TEST_ASSERT_EQUAL(3, maintVolumeLeave(r));
  TEST_ASSERT_FALSE(maintVolumeValid(r));
  TEST_ASSERT_EQUAL(0, maintVolumeLeave(r));           // once
}

void test_enter_at_2_puts_2_back() {
  MaintVolumeRecord r = garbage();
  TEST_ASSERT_EQUAL(MAINT_VOLUME, maintVolumeEnter(r, 2));
  TEST_ASSERT_EQUAL(2, maintVolumeLeave(r));
}

void test_enter_at_1_changes_nothing() {
  MaintVolumeRecord r = garbage();
  TEST_ASSERT_EQUAL(0, maintVolumeEnter(r, 1));        // it is at the maintenance volume already
  TEST_ASSERT_EQUAL(0, maintVolumeLeave(r));
  TEST_ASSERT_FALSE(maintVolumeValid(r));
}

// The MCU has not answered yet (maintenance from three short power-ons): set
// 1 anyway; afterwards an unmute puts the top volume back.
void test_unknown_volume_goes_back_by_unmute() {
  MaintVolumeRecord r = garbage();
  TEST_ASSERT_EQUAL(MAINT_VOLUME, maintVolumeEnter(r, 0));
  TEST_ASSERT_EQUAL(MAINT_RESTORE_UNKNOWN, maintVolumeLeave(r));
  MaintVolumeRecord s = garbage();
  TEST_ASSERT_EQUAL(MAINT_VOLUME, maintVolumeEnter(s, 7));   // out of range is unknown too
  TEST_ASSERT_EQUAL(MAINT_RESTORE_UNKNOWN, maintVolumeLeave(s));
}

void test_leave_without_a_record_does_nothing() {
  MaintVolumeRecord r = garbage();
  TEST_ASSERT_FALSE(maintVolumeValid(r));
  TEST_ASSERT_EQUAL(0, maintVolumeLeave(r));
}

// Maintenance again before the volume was put back (the restore waits a few
// seconds after the return): the first volume is the one to put back.
void test_reentry_keeps_the_first_volume() {
  MaintVolumeRecord r = garbage();
  TEST_ASSERT_EQUAL(MAINT_VOLUME, maintVolumeEnter(r, 3));
  TEST_ASSERT_EQUAL(0, maintVolumeEnter(r, 1));        // still at 1: nothing to set
  TEST_ASSERT_EQUAL(3, maintVolumeLeave(r));
}

// The record is what survives the reboot after an OTA: a copy stays valid,
// one flipped bit does not.
void test_record_survives_a_copy_and_detects_damage() {
  MaintVolumeRecord r = garbage();
  maintVolumeEnter(r, 3);
  MaintVolumeRecord copy;
  memcpy(&copy, &r, sizeof(r));
  TEST_ASSERT_TRUE(maintVolumeValid(copy));
  copy.volume ^= 1;
  TEST_ASSERT_FALSE(maintVolumeValid(copy));
  TEST_ASSERT_EQUAL(0, maintVolumeLeave(copy));
}

void test_clear() {
  MaintVolumeRecord r = garbage();
  maintVolumeEnter(r, 3);
  maintVolumeClear(r);
  TEST_ASSERT_FALSE(maintVolumeValid(r));
  TEST_ASSERT_EQUAL(0, maintVolumeLeave(r));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_enter_at_3_sets_1_and_leave_puts_3_back);
  RUN_TEST(test_enter_at_2_puts_2_back);
  RUN_TEST(test_enter_at_1_changes_nothing);
  RUN_TEST(test_unknown_volume_goes_back_by_unmute);
  RUN_TEST(test_leave_without_a_record_does_nothing);
  RUN_TEST(test_reentry_keeps_the_first_volume);
  RUN_TEST(test_record_survives_a_copy_and_detects_damage);
  RUN_TEST(test_clear);
  return UNITY_END();
}
