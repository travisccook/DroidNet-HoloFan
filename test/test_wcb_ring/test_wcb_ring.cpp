#include <unity.h>
#include <string.h>
#include "WcbRing.h"

using namespace wcb;

void setUp() {}
void tearDown() {}

static const uint8_t MAC[6] = {0x02, 0xD3, 0x3B, 0, 0, 2};

void test_fifo_order_and_contents() {
  PacketRing<4> r;
  uint8_t a[3] = {1, 2, 3}, b[1] = {9};
  TEST_ASSERT_TRUE(r.push(MAC, a, 3));
  TEST_ASSERT_TRUE(r.push(MAC, b, 1));
  PacketRing<4>::Slot s;
  TEST_ASSERT_TRUE(r.pop(s));
  TEST_ASSERT_EQUAL(3, s.len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(a, s.data, 3);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(MAC, s.mac, 6);
  TEST_ASSERT_TRUE(r.pop(s));
  TEST_ASSERT_EQUAL_UINT8(9, s.data[0]);
  TEST_ASSERT_FALSE(r.pop(s));
}

void test_full_rejects() {
  PacketRing<4> r;
  uint8_t x = 0;
  for (int i = 0; i < 4; i++) TEST_ASSERT_TRUE(r.push(MAC, &x, 1));
  TEST_ASSERT_FALSE(r.push(MAC, &x, 1));
}

void test_oversize_rejected() {
  PacketRing<4> r;
  static uint8_t big[253];
  TEST_ASSERT_FALSE(r.push(MAC, big, sizeof(big)));
  TEST_ASSERT_TRUE(r.push(MAC, big, 252));
}

void test_wraps_many_times() {
  PacketRing<4> r;
  PacketRing<4>::Slot s;
  for (int i = 0; i < 1000; i++) {
    uint8_t v = (uint8_t)i;
    TEST_ASSERT_TRUE(r.push(MAC, &v, 1));
    TEST_ASSERT_TRUE(r.pop(s));
    TEST_ASSERT_EQUAL_UINT8(v, s.data[0]);
  }
}

void test_clear() {
  PacketRing<4> r;
  uint8_t x = 1;
  r.push(MAC, &x, 1);
  r.push(MAC, &x, 1);
  r.clear();
  PacketRing<4>::Slot s;
  TEST_ASSERT_FALSE(r.pop(s));
  TEST_ASSERT_TRUE(r.push(MAC, &x, 1));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fifo_order_and_contents);
  RUN_TEST(test_full_rejects);
  RUN_TEST(test_oversize_rejected);
  RUN_TEST(test_wraps_many_times);
  RUN_TEST(test_clear);
  return UNITY_END();
}
