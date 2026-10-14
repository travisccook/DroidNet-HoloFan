#include <unity.h>
#include <string.h>
#include "Crc32.h"

using namespace fanlogic;

void setUp() {}
void tearDown() {}

// The CRC-32/ISO-HDLC check value (zlib.crc32(b"123456789")).
void test_check_value() {
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32Update(0, "123456789", 9));
}

void test_empty_is_zero() {
  TEST_ASSERT_EQUAL_HEX32(0, crc32Update(0, "", 0));
}

// Incremental, as zlib: feeding the previous result continues the CRC.
void test_incremental_equals_one_shot() {
  uint32_t crc = crc32Update(0, "1234", 4);
  crc = crc32Update(crc, "", 0);
  crc = crc32Update(crc, "56789", 5);
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc);
}

// One zero frame (1440 bytes): zlib.crc32(bytes(1440)).
void test_zero_frame() {
  static uint8_t zeros[1440];
  TEST_ASSERT_EQUAL_HEX32(0xAA11169Au, crc32Update(0, zeros, sizeof(zeros)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_check_value);
  RUN_TEST(test_empty_is_zero);
  RUN_TEST(test_incremental_equals_one_shot);
  RUN_TEST(test_zero_frame);
  return UNITY_END();
}
