#include <unity.h>
#include "TxPower.h"

using namespace fanlogic;

void setUp() {}
void tearDown() {}

// A fan that never got #HLFO (or got #HLFO00) transmits as before 0.9.10.
void test_unset_is_full_power() { TEST_ASSERT_EQUAL(20, txPowerDbm(0)); }

void test_saved_value_is_used() {
  TEST_ASSERT_EQUAL(2, txPowerDbm(2));
  TEST_ASSERT_EQUAL(8, txPowerDbm(8));
  TEST_ASSERT_EQUAL(20, txPowerDbm(20));
}

// A damaged NVS byte must not leave the fan too quiet for the WCBs to hear.
void test_out_of_range_saved_value_is_full_power() {
  TEST_ASSERT_EQUAL(20, txPowerDbm(1));
  TEST_ASSERT_EQUAL(20, txPowerDbm(21));
  TEST_ASSERT_EQUAL(20, txPowerDbm(255));
  TEST_ASSERT_EQUAL(20, txPowerDbm(-3));
}

// esp_wifi_set_max_tx_power counts in 0.25 dBm.
void test_quarter_dbm_units() {
  TEST_ASSERT_EQUAL(80, txPowerQuarterDbm(0));
  TEST_ASSERT_EQUAL(8, txPowerQuarterDbm(2));
  TEST_ASSERT_EQUAL(32, txPowerQuarterDbm(8));
  TEST_ASSERT_EQUAL(80, txPowerQuarterDbm(20));
  TEST_ASSERT_EQUAL(80, txPowerQuarterDbm(99));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_unset_is_full_power);
  RUN_TEST(test_saved_value_is_used);
  RUN_TEST(test_out_of_range_saved_value_is_full_power);
  RUN_TEST(test_quarter_dbm_units);
  return UNITY_END();
}
