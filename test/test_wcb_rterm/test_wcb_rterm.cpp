#include <unity.h>
#include <stddef.h>
#include <string.h>
#include "WcbRterm.h"
#include "WcbPacket.h"

using namespace wcb;

void setUp() {}
void tearDown() {}

void test_layout() {
  TEST_ASSERT_EQUAL(204, sizeof(RtermPacket));
  TEST_ASSERT_EQUAL(40, offsetof(RtermPacket, packetType));
  TEST_ASSERT_EQUAL(41, offsetof(RtermPacket, sourceId));
  TEST_ASSERT_EQUAL(42, offsetof(RtermPacket, destId));
  TEST_ASSERT_EQUAL(43, offsetof(RtermPacket, textLen));
  TEST_ASSERT_EQUAL(44, offsetof(RtermPacket, text));
}

void test_fields() {
  RtermPacket p;
  size_t n = buildRterm(p, "pw", 15, 2, "hello", 5);
  TEST_ASSERT_EQUAL(5, n);
  TEST_ASSERT_EQUAL_STRING("pw", p.password);
  TEST_ASSERT_EQUAL_UINT8(PKT_RTERM, p.packetType);
  TEST_ASSERT_EQUAL_UINT8(15, p.sourceId);
  TEST_ASSERT_EQUAL_UINT8(2, p.destId);
  TEST_ASSERT_EQUAL_UINT8(5, p.textLen);
  TEST_ASSERT_EQUAL_MEMORY("hello", p.text, 5);
  TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)p.text[5]);
}

void test_truncates_at_160() {
  char line[300];
  memset(line, 'a', sizeof(line));
  RtermPacket p;
  TEST_ASSERT_EQUAL(160, buildRterm(p, "pw", 15, 2, line, sizeof(line)));
  TEST_ASSERT_EQUAL_UINT8(160, p.textLen);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_layout);
  RUN_TEST(test_fields);
  RUN_TEST(test_truncates_at_160);
  return UNITY_END();
}
