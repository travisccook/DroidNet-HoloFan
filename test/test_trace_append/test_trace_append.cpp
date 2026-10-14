#include <unity.h>
#include <string.h>
#include <vector>
#include "McuProto.h"

using namespace fanlogic;

void setUp() {}
void tearDown() {}

void test_appends_and_terminates() {
  char buf[16];
  size_t len = traceAppend(buf, sizeof(buf), 0, "abc", 3);
  len = traceAppend(buf, sizeof(buf), len, "de", 2);
  TEST_ASSERT_EQUAL(5, (int)len);
  TEST_ASSERT_EQUAL_STRING("abcde", buf);
}

// Full: the oldest half goes, the latest bytes stay.
void test_full_drops_the_oldest_half() {
  char buf[16];
  size_t len = traceAppend(buf, sizeof(buf), 0, "0123456789ABCDE", 15);
  len = traceAppend(buf, sizeof(buf), len, "xy", 2);
  TEST_ASSERT_EQUAL_STRING("789ABCDExy", buf);
  TEST_ASSERT_EQUAL(10, (int)len);
}

// An append longer than half the buffer keeps only its last size / 2 bytes.
void test_long_append_keeps_its_tail() {
  char buf[16];
  size_t len = traceAppend(buf, sizeof(buf), 0, "abcdefghijklmnopqrst", 20);
  TEST_ASSERT_EQUAL_STRING("mnopqrst", buf);
  TEST_ASSERT_EQUAL(8, (int)len);
}

// The fan's 3072-byte trace with 3071 bytes used and a 1536-byte append:
// dropping half left 1536 + 1536 = 3072 bytes and the NUL went to [3072].
void test_never_writes_past_the_buffer() {
  const size_t size = 3072;
  std::vector<char> mem(size + 8, 'Z');      // canary after the buffer
  std::vector<char> fill(size - 1, 'a'), add(1536, 'b');
  size_t len = traceAppend(mem.data(), size, 0, fill.data(), 1536);
  len = traceAppend(mem.data(), size, len, fill.data(), 1535);
  TEST_ASSERT_EQUAL(3071, (int)len);
  len = traceAppend(mem.data(), size, len, add.data(), add.size());
  for (size_t i = size; i < mem.size(); i++) TEST_ASSERT_EQUAL_CHAR('Z', mem[i]);
  TEST_ASSERT_TRUE(len < size);
  TEST_ASSERT_EQUAL_CHAR('\0', mem[len]);
  TEST_ASSERT_EQUAL_MEMORY(add.data(), mem.data() + len - add.size(), add.size());
}

void test_zero_size_writes_nothing() {
  char c = 'Z';
  TEST_ASSERT_EQUAL(0, (int)traceAppend(&c, 0, 0, "abc", 3));
  TEST_ASSERT_EQUAL_CHAR('Z', c);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_appends_and_terminates);
  RUN_TEST(test_full_drops_the_oldest_half);
  RUN_TEST(test_long_append_keeps_its_tail);
  RUN_TEST(test_never_writes_past_the_buffer);
  RUN_TEST(test_zero_size_writes_nothing);
  return UNITY_END();
}
