#include <unity.h>
#include <string.h>
#include <vector>
#include "FrameRing.h"

using namespace fanlogic;

// NOR flash: erase sets 0xFF per 4 KB sector or per 64 KB block (a block
// only at a multiple of its size); a write may only turn 1-bits into
// 0-bits, so writing over bytes that are not 0xFF is a bug (flagged).
class FakeFlash : public FlashIo {
 public:
  explicit FakeFlash(size_t size) : mem(size, 0x00) {}
  std::vector<uint8_t> mem;
  int erases = 0, blockErases = 0;
  bool dirtyWrite = false, failWrite = false;
  size_t size() const override { return mem.size(); }
  bool erase(size_t off, size_t len) override {
    if (off % RING_SECTOR || len % RING_SECTOR || off + len > mem.size()) return false;
    if (len != RING_SECTOR && (len != RING_ERASE_BLOCK || off % RING_ERASE_BLOCK)) return false;
    memset(&mem[off], 0xFF, len);
    erases += (int)(len / RING_SECTOR);
    if (len == RING_ERASE_BLOCK) blockErases++;
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
  TEST_ASSERT_EQUAL_UINT32(594, (uint32_t)r.capacity());   // 640 frames of flash, one erase block kept free
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

// Room is kept so the block erased ahead of the writer never holds unread
// frames: a push is refused unless a whole erase block stays free after it.
void test_full_leaves_an_erase_block_free_and_push_refuses() {
  FakeFlash fl(184320);
  FrameRing r(fl);
  TEST_ASSERT_TRUE(r.begin());
  uint8_t f[SPI_FRAME] = {0};
  while (!r.full()) TEST_ASSERT_TRUE(r.push(f));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)r.capacity(), (uint32_t)r.count());
  TEST_ASSERT_TRUE(184320 - r.count() * SPI_FRAME >= RING_ERASE_BLOCK);
  TEST_ASSERT_FALSE(r.push(f));
}

// 0.9.18: eraseAhead() erases whole 64 KB blocks. Kept full and
// drained a window at a time, with eraseAhead() after each window (the
// upload's steady state), every frame comes back intact, nothing unread is
// ever erased, and after the first lap's sector erases (push() erases by
// sectors until eraseAhead() has reached a block boundary) the erases are
// blocks, plus the one sector at the ring's end per lap.
void test_erase_ahead_erases_blocks_in_the_quiet_moment() {
  FakeFlash fl(983040);
  FrameRing r(fl);
  TEST_ASSERT_TRUE(r.begin());
  TEST_ASSERT_EQUAL_UINT32(594, (uint32_t)r.capacity());
  uint8_t in[SPI_FRAME], out[SPI_FRAME], want[SPI_FRAME];
  uint32_t pushed = 0, popped = 0;
  while (!r.full()) { frameOf(pushed++, in); TEST_ASSERT_TRUE(r.push(in)); }    // prefill: sector erases
  TEST_ASSERT_EQUAL_UINT32(594, (uint32_t)r.count());
  TEST_ASSERT_EQUAL(0, fl.blockErases);
  const int prefillSectors = fl.erases;
  for (int round = 0; round < 150; round++) {                                      // ~4.7 laps
    for (int k = 0; k < 20; k++) {
      TEST_ASSERT_TRUE(r.pop(out));
      frameOf(popped++, want);
      TEST_ASSERT_EQUAL_MEMORY(want, out, SPI_FRAME);
    }
    TEST_ASSERT_TRUE(r.eraseAhead(SPI_FRAME * 21) >= 0);
    while (!r.full()) { frameOf(pushed++, in); TEST_ASSERT_TRUE(r.push(in)); }
  }
  TEST_ASSERT_FALSE(fl.dirtyWrite);
  const int laps = (int)(pushed * SPI_FRAME / 921600);
  TEST_ASSERT_TRUE(laps >= 4);
  TEST_ASSERT_TRUE(fl.blockErases >= (laps - 1) * 14);
  TEST_ASSERT_TRUE(fl.erases - prefillSectors - fl.blockErases * 16 <= 15 + laps + 1);   // to the first boundary, then 1 per lap
}

// eraseAhead() stops where it would reach an unread frame. On an empty ring
// (a session opening) it erases what the clip needs in blocks, and the
// pushes after it erase nothing.
void test_erase_ahead_never_reaches_unread_frames() {
  FakeFlash fl(184320);
  FrameRing r(fl);
  TEST_ASSERT_TRUE(r.begin());
  uint8_t f[SPI_FRAME], out[SPI_FRAME], want[SPI_FRAME];
  uint32_t pushed = 0;
  while (!r.full()) { frameOf(pushed++, f); TEST_ASSERT_TRUE(r.push(f)); }
  for (int i = 0; i < 20; i++) r.eraseAhead(1000000);       // far more than there is room for
  for (uint32_t i = 0; i < pushed; i++) {
    TEST_ASSERT_TRUE(r.pop(out));
    frameOf(i, want);
    TEST_ASSERT_EQUAL_MEMORY(want, out, SPI_FRAME);
  }
  FakeFlash fl2(983040);
  FrameRing open(fl2);
  TEST_ASSERT_TRUE(open.begin());
  TEST_ASSERT_EQUAL(65536 * 5, (int)open.eraseAhead(SPI_FRAME * 200));     // 288,000 bytes wanted: 5 blocks
  TEST_ASSERT_EQUAL(5, fl2.blockErases);
  for (uint32_t i = 0; i < 200; i++) { frameOf(i, f); TEST_ASSERT_TRUE(open.push(f)); }
  TEST_ASSERT_EQUAL(5 * 16, fl2.erases);
  TEST_ASSERT_FALSE(fl2.dirtyWrite);
  FakeFlash fl3(983040);
  FrameRing whole(fl3);
  TEST_ASSERT_TRUE(whole.begin());
  TEST_ASSERT_EQUAL(921600, (int)whole.eraseAhead(100000000));              // a clip larger than the ring: all of it
  TEST_ASSERT_EQUAL(14, fl3.blockErases);
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
  RUN_TEST(test_full_leaves_an_erase_block_free_and_push_refuses);
  RUN_TEST(test_erase_ahead_erases_blocks_in_the_quiet_moment);
  RUN_TEST(test_erase_ahead_never_reaches_unread_frames);
  RUN_TEST(test_pop_empty_and_write_failure);
  return UNITY_END();
}
