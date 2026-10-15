#include <unity.h>
#include "TxPower.h"

using namespace fanlogic;

void setUp() {}
void tearDown() {}

// A fan that never got #HLFO (or got #HLFO00) transmits at the default, 8 dBm.
void test_unset_is_the_default() {
  TEST_ASSERT_EQUAL(8, TX_DBM_DEFAULT);
  TEST_ASSERT_EQUAL(TX_DBM_DEFAULT, txPowerDbm(0));
}

void test_saved_value_is_used() {
  TEST_ASSERT_EQUAL(2, txPowerDbm(2));
  TEST_ASSERT_EQUAL(8, txPowerDbm(8));
  TEST_ASSERT_EQUAL(20, txPowerDbm(20));
}

// A damaged NVS byte means the default.
void test_out_of_range_saved_value_is_the_default() {
  TEST_ASSERT_EQUAL(TX_DBM_DEFAULT, txPowerDbm(1));
  TEST_ASSERT_EQUAL(TX_DBM_DEFAULT, txPowerDbm(21));
  TEST_ASSERT_EQUAL(TX_DBM_DEFAULT, txPowerDbm(255));
  TEST_ASSERT_EQUAL(TX_DBM_DEFAULT, txPowerDbm(-3));
}

// esp_wifi_set_max_tx_power counts in 0.25 dBm.
void test_quarter_dbm_units() {
  TEST_ASSERT_EQUAL(32, txPowerQuarterDbm(0));
  TEST_ASSERT_EQUAL(8, txPowerQuarterDbm(2));
  TEST_ASSERT_EQUAL(32, txPowerQuarterDbm(8));
  TEST_ASSERT_EQUAL(80, txPowerQuarterDbm(20));
  TEST_ASSERT_EQUAL(32, txPowerQuarterDbm(99));
}

// Maintenance (Wi-Fi uploads and OTA) always transmits at full power,
// whatever #HLFO saved.
void test_maintenance_is_full_power() { TEST_ASSERT_EQUAL(80, maintTxQuarterDbm()); }

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_unset_is_the_default);
  RUN_TEST(test_saved_value_is_used);
  RUN_TEST(test_out_of_range_saved_value_is_the_default);
  RUN_TEST(test_quarter_dbm_units);
  RUN_TEST(test_maintenance_is_full_power);
  return UNITY_END();
}
