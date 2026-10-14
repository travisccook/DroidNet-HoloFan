#include <unity.h>
#include <string.h>
#include <vector>
#include "FrameRing.h"

using namespace fanlogic;

// NOR flash: erase sets 0xFF per 4 KB sector; a write may only turn 1-bits
// into 0-bits, so writing over bytes that are not 0xFF is a bug (flagged).
class FakeFlash : public FlashIo {
 public:
  explicit FakeFlash(size_t size) : mem(size, 0x00) {}
  std::vector<uint8_t> mem;
  int erases = 0;
  bool dirtyWrite = false, failWrite = false;
  size_t size() const override { return mem.size(); }
  bool erase(size_t off, size_t len) override {
    if (off % RING_SECTOR || len % RING_SECTOR || off + len > mem.size()) return false;
    memset(&mem[off], 0xFF, len);
    erases += (int)(len / RING_SECTOR);
    return true;
  }
  bool write(size_t off, const uint8_t* buf, size_t len) override {
    if (failWrite || off + len > mem.size()) return false;
    for (size_t i = 0; i < len; i++) {
      if (mem[off + i] != 0xFF) dirtyWrite = true;
      mem[off + i] &= buf[i];
    }
    return true;
  }
  bool read(size_t off, uint8_t* buf, size_t len) override {
    if (off + len > mem.size()) return false;
    memcpy(buf, &mem[off], len);
    return true;
  }
};

static void frameOf(uint32_t n, uint8_t* f) {
  for (size_t i = 0; i < SPI_FRAME; i++) f[i] = (uint8_t)(n * 31 + i * 7);
}

void setUp() {}
void tearDown() {}

// The ring uses whole multiples of lcm(1440, 4096) = 184320 bytes (128
// frames, 45 sectors), so frames never straddle the end and sectors align.
void test_capacity_is_whole_blocks() {
  FakeFlash fl(983040);                       // the fan's OTA slot (960 KB)
  FrameRing r(fl);
  TEST_ASSERT_TRUE(r.begin());
  TEST_ASSERT_EQUAL_UINT32(640, (uint32_t)r.capacity());
  FakeFlash small(100000);
  FrameRing r2(small);
  TEST_ASSERT_FALSE(r2.begin());              // smaller than one block
}

void test_fifo_order_across_many_laps_never_dirty() {
  FakeFlash fl(184320 * 2);                   // 256 frames
  FrameRing r(fl);
  TEST_ASSERT_TRUE(r.begin());
  uint8_t in[SPI_FRAME], out[SPI_FRAME], want[SPI_FRAME];
  uint32_t pushed = 0, popped = 0;
  // Fill to the limit, then alternate bursts so the ring wraps ~10 times.
  for (int round = 0; round < 40; round++) {
    while (!r.full()) { frameOf(pushed++, in); TEST_ASSERT_TRUE(r.push(in)); }
    int take = 30 + round % 50;
    for (int k = 0; k < take && r.count() > 0; k++) {
      TEST_ASSERT_TRUE(r.pop(out));
      frameOf(popped++, want);
      TEST_ASSERT_EQUAL_MEMORY(want, out, SPI_FRAME);
    }
  }
  while (r.count() > 0) {
    TEST_ASSERT_TRUE(r.pop(out));
    frameOf(popped++, want);
    TEST_ASSERT_EQUAL_MEMORY(want, out, SPI_FRAME);
  }
  TEST_ASSERT_EQUAL_UINT32(pushed, popped);
  TEST_ASSERT_TRUE(pushed > 256 * 5);
  TEST_ASSERT_FALSE(fl.dirtyWrite);
}

// Room is kept so the sector erased ahead of the writer never holds unread
// frames: a push is refused unless a whole sector stays free after it.
void test_full_leaves_a_sector_free_and_push_refuses() {
  FakeFlash fl(184320);
  FrameRing r(fl);
  TEST_ASSERT_TRUE(r.begin());
  uint8_t f[SPI_FRAME] = {0};
  while (!r.full()) TEST_ASSERT_TRUE(r.push(f));
  TEST_ASSERT_TRUE((r.capacity() - r.count()) * SPI_FRAME >= RING_SECTOR);
  TEST_ASSERT_FALSE(r.push(f));
}

void test_pop_empty_and_write_failure() {
  FakeFlash fl(184320);
  FrameRing r(fl);
  TEST_ASSERT_TRUE(r.begin());
  uint8_t f[SPI_FRAME] = {0};
  TEST_ASSERT_FALSE(r.pop(f));
  fl.failWrite = true;
  TEST_ASSERT_FALSE(r.push(f));
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)r.count());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_capacity_is_whole_blocks);
  RUN_TEST(test_fifo_order_across_many_laps_never_dirty);
  RUN_TEST(test_full_leaves_a_sector_free_and_push_refuses);
  RUN_TEST(test_pop_empty_and_write_failure);
  return UNITY_END();
}
