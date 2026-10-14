#include <unity.h>
#include <string.h>
#include "WcbCrc.h"

using namespace wcb;

void setUp() {}
void tearDown() {}

void test_standard_check_value() {
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32("123456789", 9));
  TEST_ASSERT_EQUAL_HEX32(0x00000000u, crc32("", 0));
}

void test_append_uppercase_suffix() {
  char out[64];
  TEST_ASSERT_TRUE(appendCrc("#HLQ", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("#HLQ|CRCADEA62DF", out);
  TEST_ASSERT_TRUE(appendCrc(";S2,#HL:POS 4520", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING(";S2,#HL:POS 4520|CRCBDA30ACC", out);
}

void test_append_refuses_when_it_does_not_fit() {
  char out[16];                                   // "#HLQ" + 12 + NUL = 17
  TEST_ASSERT_FALSE(appendCrc("#HLQ", out, sizeof(out)));
  char exact[17];
  TEST_ASSERT_TRUE(appendCrc("#HLQ", exact, sizeof(exact)));
}

void test_verify_strips_good_suffix() {
  char buf[64] = "#HLU|CRCAA87A6C6";
  TEST_ASSERT_EQUAL((int)CrcCheck::OK, (int)verifyStripCrc(buf));
  TEST_ASSERT_EQUAL_STRING("#HLU", buf);
}

void test_verify_accepts_lowercase_hex() {
  char buf[64] = "#HLU|CRCaa87a6c6";
  TEST_ASSERT_EQUAL((int)CrcCheck::OK, (int)verifyStripCrc(buf));
  TEST_ASSERT_EQUAL_STRING("#HLU", buf);
}

void test_verify_mismatch_leaves_buffer() {
  char buf[64] = "#HLD|CRCAA87A6C6";                // CRC of "#HLU", not "#HLD"
  TEST_ASSERT_EQUAL((int)CrcCheck::MISMATCH, (int)verifyStripCrc(buf));
  TEST_ASSERT_EQUAL_STRING("#HLD|CRCAA87A6C6", buf);
}

void test_verify_missing_cases() {
  char a[64] = "#HLU";                               // no suffix at all
  char b[64] = "x";                                  // shorter than 12
  char c[64] = "#HLU|CRCAA87A6CZ";                   // non-hex digit
  char d[64] = "#HLU|CRCAA87A6C6 ";                  // suffix not at the end
  TEST_ASSERT_EQUAL((int)CrcCheck::MISSING, (int)verifyStripCrc(a));
  TEST_ASSERT_EQUAL((int)CrcCheck::MISSING, (int)verifyStripCrc(b));
  TEST_ASSERT_EQUAL((int)CrcCheck::MISSING, (int)verifyStripCrc(c));
  TEST_ASSERT_EQUAL((int)CrcCheck::MISSING, (int)verifyStripCrc(d));
}

void test_verify_is_anchored_to_the_tail() {
  // A payload that itself contains "|CRC" text: only the trailing tag counts.
  char body[64] = "note|CRC12345678";
  char buf[64];
  TEST_ASSERT_TRUE(appendCrc(body, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL((int)CrcCheck::OK, (int)verifyStripCrc(buf));
  TEST_ASSERT_EQUAL_STRING("note|CRC12345678", buf);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_standard_check_value);
  RUN_TEST(test_append_uppercase_suffix);
  RUN_TEST(test_append_refuses_when_it_does_not_fit);
  RUN_TEST(test_verify_strips_good_suffix);
  RUN_TEST(test_verify_accepts_lowercase_hex);
  RUN_TEST(test_verify_mismatch_leaves_buffer);
  RUN_TEST(test_verify_missing_cases);
  RUN_TEST(test_verify_is_anchored_to_the_tail);
  return UNITY_END();
}
