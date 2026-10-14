#include <unity.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "ChunkFeed.h"

using namespace fanlogic;

void setUp() {}
void tearDown() {}

class FakeClock : public Clock {
 public:
  uint32_t ms = 0;
  uint32_t nowMs() override { return ms; }
};

class RamFlash : public FlashIo {
 public:
  explicit RamFlash(size_t n) : mem(n, 0xFF) {}
  std::vector<uint8_t> mem;
  bool failWrites = false;
  size_t size() const override { return mem.size(); }
  bool erase(size_t off, size_t len) override { memset(&mem[off], 0xFF, len); return true; }
  bool write(size_t off, const uint8_t* b, size_t len) override {
    if (failWrites) return false;
    memcpy(&mem[off], b, len);
    return true;
  }
  bool read(size_t off, uint8_t* b, size_t len) override { memcpy(b, &mem[off], len); return true; }
};

// A feed over a RAM ring: RING_BLOCK * 5 = 640 frames (as the fan), or
// RING_BLOCK = 128 frames (full at 125: a push needs a free sector after it).
struct Rig {
  RamFlash  flash;
  FrameRing ring;
  FakeClock clk;
  ChunkFeed feed;
  uint8_t   asmBuf[SPI_FRAME];
  explicit Rig(size_t ringBytes = RING_BLOCK * 5) : flash(ringBytes), ring(flash), feed(ring, clk) { ring.begin(); }
};

static std::vector<uint8_t> pattern(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = (uint8_t)(i * 7 + 1);
  return v;
}

void test_feed_stages_writes_into_frames() {
  Rig r;
  auto d = pattern(3000);
  r.feed.begin(d.size(), r.asmBuf);
  TEST_ASSERT_EQUAL(1000, r.feed.write(0, d.data(), 1000));
  TEST_ASSERT_EQUAL(1000, r.feed.staged());
  TEST_ASSERT_EQUAL(0, r.feed.count());                     // a partial frame waits in the assembly buffer
  TEST_ASSERT_EQUAL(BodyState::ARRIVING, r.feed.state());
  TEST_ASSERT_EQUAL(2000, r.feed.write(1000, d.data() + 1000, 2000));
  TEST_ASSERT_EQUAL(3000, r.feed.staged());
  TEST_ASSERT_EQUAL(3, r.feed.count());                     // 1440 + 1440 + 120
  TEST_ASSERT_EQUAL(BodyState::COMPLETE, r.feed.state());
  uint8_t f[SPI_FRAME];
  TEST_ASSERT_TRUE(r.feed.pop(f));
  TEST_ASSERT_EQUAL_MEMORY(d.data(), f, SPI_FRAME);
  TEST_ASSERT_TRUE(r.feed.pop(f));
  TEST_ASSERT_EQUAL_MEMORY(d.data() + SPI_FRAME, f, SPI_FRAME);
  TEST_ASSERT_TRUE(r.feed.pop(f));
  TEST_ASSERT_EQUAL_MEMORY(d.data() + 2 * SPI_FRAME, f, 120);
  for (size_t i = 120; i < SPI_FRAME; i++) TEST_ASSERT_EQUAL_UINT8(0, f[i]);   // the last frame is zero-padded
  TEST_ASSERT_FALSE(r.feed.pop(f));
}

void test_feed_drops_overlap_and_counts_dup() {
  Rig r;
  auto d = pattern(5000);
  r.feed.begin(d.size(), r.asmBuf);
  r.feed.write(0, d.data(), 2000);
  TEST_ASSERT_EQUAL(1000, r.feed.write(1500, d.data() + 1500, 1000));
  TEST_ASSERT_EQUAL(2500, r.feed.staged());
  TEST_ASSERT_EQUAL(500, r.feed.dupBytes());
  r.feed.write(2500, d.data() + 2500, 2500);
  uint8_t f[SPI_FRAME];
  std::vector<uint8_t> out;
  while (r.feed.pop(f)) out.insert(out.end(), f, f + SPI_FRAME);
  TEST_ASSERT_EQUAL_MEMORY(d.data(), out.data(), d.size());
}

// Review focus 2: a reply was lost after the fan staged the whole chunk;
// the client resends it from its old offset.
void test_feed_whole_overlap_is_all_dup() {
  Rig r;
  auto d = pattern(4000);
  r.feed.begin(d.size(), r.asmBuf);
  r.feed.write(0, d.data(), 2000);
  TEST_ASSERT_EQUAL(1000, r.feed.write(0, d.data(), 1000));
  TEST_ASSERT_EQUAL(2000, r.feed.staged());
  TEST_ASSERT_EQUAL(1000, r.feed.dupBytes());
}

void test_feed_refuses_a_gap() {
  Rig r;
  auto d = pattern(4000);
  r.feed.begin(d.size(), r.asmBuf);
  r.feed.write(0, d.data(), 1000);
  TEST_ASSERT_EQUAL(0, r.feed.write(1200, d.data() + 1200, 100));
  TEST_ASSERT_EQUAL(1000, r.feed.staged());
}

void test_feed_never_stages_past_size() {
  Rig r;
  auto d = pattern(2000);
  r.feed.begin(1500, r.asmBuf);
  TEST_ASSERT_EQUAL(1500, r.feed.write(0, d.data(), 2000));
  TEST_ASSERT_EQUAL(1500, r.feed.staged());
  TEST_ASSERT_EQUAL(BodyState::COMPLETE, r.feed.state());
  TEST_ASSERT_EQUAL(2, r.feed.count());
}

// 0.8.2: prefill target = capacity - one window (20 frames; 620 of 640, 108 of
// 128) or the whole body.
void test_feed_prefilled_at_capacity_minus_window_or_whole_body() {
  Rig big(RING_BLOCK);
  auto d = pattern(SPI_FRAME * 200);
  big.feed.begin(d.size(), big.asmBuf);
  big.feed.write(0, d.data(), SPI_FRAME * 107);
  TEST_ASSERT_FALSE(big.feed.prefilled());
  big.feed.write(SPI_FRAME * 107, d.data() + SPI_FRAME * 107, SPI_FRAME);
  TEST_ASSERT_TRUE(big.feed.prefilled());

  Rig fan;                                                  // 640 frames: 620
  auto e = pattern(SPI_FRAME * 700);
  fan.feed.begin(e.size(), fan.asmBuf);
  fan.feed.write(0, e.data(), SPI_FRAME * 619);
  TEST_ASSERT_FALSE(fan.feed.prefilled());
  fan.feed.write(SPI_FRAME * 619, e.data() + SPI_FRAME * 619, SPI_FRAME);
  TEST_ASSERT_TRUE(fan.feed.prefilled());

  Rig small;
  auto t = pattern(100);                                    // less than one frame
  small.feed.begin(t.size(), small.asmBuf);
  small.feed.write(0, t.data(), 99);
  TEST_ASSERT_FALSE(small.feed.prefilled());
  small.feed.write(99, t.data() + 99, 1);
  TEST_ASSERT_TRUE(small.feed.prefilled());
  TEST_ASSERT_EQUAL(1, small.feed.count());
}

void test_feed_write_stops_when_ring_full_and_resumes_after_pop() {
  Rig r(RING_BLOCK);
  auto d = pattern(SPI_FRAME * 200);
  r.feed.begin(d.size(), r.asmBuf);
  size_t used = r.feed.write(0, d.data(), d.size());
  TEST_ASSERT_TRUE(r.feed.full());
  TEST_ASSERT_EQUAL(125, r.feed.count());
  TEST_ASSERT_EQUAL(SPI_FRAME * 125, used);
  TEST_ASSERT_EQUAL(used, r.feed.staged());
  uint8_t f[SPI_FRAME];
  TEST_ASSERT_TRUE(r.feed.pop(f));
  TEST_ASSERT_EQUAL(SPI_FRAME, r.feed.write(used, d.data() + used, d.size() - used));
}

void test_feed_abort() {
  Rig r;
  auto d = pattern(5000);
  r.feed.begin(d.size(), r.asmBuf);
  r.feed.write(0, d.data(), 2000);
  r.feed.abort();
  TEST_ASSERT_EQUAL(BodyState::FAILED, r.feed.state());
  TEST_ASSERT_EQUAL(ClipErr::ABORT, r.feed.failErr());
  TEST_ASSERT_EQUAL('a', r.feed.bodyEnd());
}

// Review focus 1: once the session has ended the feed takes no more bytes
// (on the fan its assembly buffer is freed then).
void test_feed_write_after_abort_is_refused() {
  Rig r;
  auto d = pattern(5000);
  r.feed.begin(d.size(), r.asmBuf);
  r.feed.write(0, d.data(), 1000);
  r.feed.abort();
  TEST_ASSERT_EQUAL(0, r.feed.write(1000, d.data() + 1000, 1000));
  TEST_ASSERT_EQUAL(0, r.feed.write(0, d.data(), 1000));
  TEST_ASSERT_EQUAL(1000, r.feed.staged());
}

void test_feed_flash_failure() {
  Rig r;
  auto d = pattern(5000);
  r.feed.begin(d.size(), r.asmBuf);
  r.flash.failWrites = true;
  r.feed.write(0, d.data(), 2000);
  TEST_ASSERT_EQUAL(BodyState::FAILED, r.feed.state());
  TEST_ASSERT_EQUAL(ClipErr::FLASH, r.feed.failErr());
  TEST_ASSERT_EQUAL('f', r.feed.bodyEnd());
}

void test_feed_prefill_idle_fails_after_ten_minutes() {
  Rig r;
  auto d = pattern(SPI_FRAME * 400);
  r.clk.ms = 1000;
  r.feed.begin(d.size(), r.asmBuf);
  r.feed.write(0, d.data(), 5000);                          // last byte at 1000 ms
  r.clk.ms = 1000 + PREFILL_IDLE_MS - 1;
  TEST_ASSERT_EQUAL(BodyState::ARRIVING, r.feed.state());
  r.clk.ms = 1000 + PREFILL_IDLE_MS;
  TEST_ASSERT_EQUAL(BodyState::FAILED, r.feed.state());
  TEST_ASSERT_EQUAL(ClipErr::RECV, r.feed.failErr());
  TEST_ASSERT_EQUAL('s', r.feed.bodyEnd());
}

void test_feed_dry_after_the_first_pop() {
  Rig r;
  auto d = pattern(SPI_FRAME * 10);
  r.feed.begin(d.size(), r.asmBuf);
  r.feed.write(0, d.data(), SPI_FRAME * 3);
  r.clk.ms = 3000;                                          // empty ring before any pop: no dry rule
  TEST_ASSERT_EQUAL(BodyState::ARRIVING, r.feed.state());
  uint8_t f[SPI_FRAME];
  r.feed.pop(f);                                            // at 3000 ms
  r.clk.ms = 3000 + DRY_MS - 1;                             // two frames left, popped recently: not dry
  TEST_ASSERT_EQUAL(BodyState::ARRIVING, r.feed.state());
  r.feed.pop(f);
  r.clk.ms = 9000;
  r.feed.pop(f);                                            // empty at 9000 ms
  r.clk.ms = 9000 + DRY_MS - 1;
  TEST_ASSERT_EQUAL(BodyState::ARRIVING, r.feed.state());
  r.clk.ms = 9000 + DRY_MS;
  TEST_ASSERT_EQUAL(BodyState::FAILED, r.feed.state());
  TEST_ASSERT_EQUAL(ClipErr::RECV, r.feed.failErr());
  TEST_ASSERT_EQUAL('d', r.feed.bodyEnd());
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(9000 + DRY_MS, r.feed.longestQuietMs());   // the silence up to the failure counts
}

void test_feed_dry_with_a_partial_window_buffered() {
  Rig r;
  auto d = pattern(SPI_FRAME * 40);
  r.feed.begin(d.size(), r.asmBuf);
  r.feed.write(0, d.data(), SPI_FRAME * 10);
  uint8_t f[SPI_FRAME];
  r.feed.pop(f);                                            // 9 frames stay buffered, no more pops
  r.clk.ms = DRY_MS - 1;
  TEST_ASSERT_EQUAL(BodyState::ARRIVING, r.feed.state());
  r.clk.ms = DRY_MS;
  TEST_ASSERT_EQUAL(BodyState::FAILED, r.feed.state());
  TEST_ASSERT_EQUAL('d', r.feed.bodyEnd());
}

void test_feed_dry_limit_is_per_session() {
  Rig r;
  auto d = pattern(SPI_FRAME * 40);
  r.feed.begin(d.size(), r.asmBuf, 0, 30000);
  r.feed.write(0, d.data(), SPI_FRAME * 10);
  uint8_t f[SPI_FRAME];
  r.feed.pop(f);
  r.clk.ms = 29999;
  TEST_ASSERT_EQUAL(BodyState::ARRIVING, r.feed.state());
  r.clk.ms = 30000;
  TEST_ASSERT_EQUAL(BodyState::FAILED, r.feed.state());
  TEST_ASSERT_EQUAL('d', r.feed.bodyEnd());
}

void test_feed_prefill_override_and_clamp() {
  Rig r;                                                    // 640 frames: default prefill 620
  auto d = pattern(SPI_FRAME * 700);
  r.feed.begin(d.size(), r.asmBuf, 20);
  r.feed.write(0, d.data(), SPI_FRAME * 19);
  TEST_ASSERT_FALSE(r.feed.prefilled());
  r.feed.write(SPI_FRAME * 19, d.data() + SPI_FRAME * 19, SPI_FRAME);
  TEST_ASSERT_TRUE(r.feed.prefilled());
  Rig a;
  a.feed.begin(d.size(), a.asmBuf, 1000);                   // clamped to capacity - 20
  a.feed.write(0, d.data(), SPI_FRAME * 619);
  TEST_ASSERT_FALSE(a.feed.prefilled());
  a.feed.write(SPI_FRAME * 619, d.data() + SPI_FRAME * 619, SPI_FRAME);
  TEST_ASSERT_TRUE(a.feed.prefilled());
  Rig b;
  b.feed.begin(d.size(), b.asmBuf, 0);                      // 0 = capacity - 20
  b.feed.write(0, d.data(), SPI_FRAME * 619);
  TEST_ASSERT_FALSE(b.feed.prefilled());
}

// A ring of 20 frames or fewer: the target never drops below 1.
void test_feed_prefill_default_never_below_one() {
  RamFlash fl(RING_BLOCK);
  FrameRing ring(fl);
  FakeClock clk;
  ChunkFeed feed(ring, clk);
  ring.begin();
  uint8_t asmBuf[SPI_FRAME];
  auto d = pattern(SPI_FRAME * 4);
  feed.begin(d.size(), asmBuf, 0);
  TEST_ASSERT_TRUE(ring.capacity() > 20);                   // sanity: the real ring is bigger
  feed.write(0, d.data(), SPI_FRAME);
  TEST_ASSERT_FALSE(feed.prefilled());                      // 107 wanted, whole body not in
}

// stg: bytes staged by the first pop / ms from begin() to it.
void test_feed_staging_rate_at_first_pop() {
  Rig r;
  auto d = pattern(SPI_FRAME * 100);
  r.clk.ms = 1000;
  r.feed.begin(d.size(), r.asmBuf, 20);
  TEST_ASSERT_EQUAL_UINT32(0, r.feed.stagingRate());        // nothing popped yet
  r.clk.ms = 3000;
  r.feed.write(0, d.data(), SPI_FRAME * 30);
  r.clk.ms = 5000;                                          // 4 s after begin
  uint8_t f[SPI_FRAME];
  r.feed.pop(f);
  r.clk.ms = 9000;
  r.feed.write(SPI_FRAME * 30, d.data() + SPI_FRAME * 30, SPI_FRAME * 10);
  r.feed.pop(f);                                            // later pops change nothing
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(SPI_FRAME * 30 / 4), r.feed.stagingRate());   // B/s
  r.feed.begin(d.size(), r.asmBuf, 20);                     // a new session resets it
  TEST_ASSERT_EQUAL_UINT32(0, r.feed.stagingRate());
}

void test_feed_complete_is_never_dry() {
  Rig r;
  auto d = pattern(SPI_FRAME * 2);
  r.feed.begin(d.size(), r.asmBuf);
  r.feed.write(0, d.data(), d.size());
  uint8_t f[SPI_FRAME];
  r.feed.pop(f);
  r.feed.pop(f);
  r.clk.ms = 60000;
  TEST_ASSERT_EQUAL(BodyState::COMPLETE, r.feed.state());
}

void test_feed_stats_quiet_and_ringmin() {
  Rig r;
  auto d = pattern(SPI_FRAME * 20);
  r.feed.begin(d.size(), r.asmBuf);
  TEST_ASSERT_EQUAL(SIZE_MAX, r.feed.ringMin());
  r.feed.write(0, d.data(), SPI_FRAME * 5);                 // at 0 ms
  r.clk.ms = 5000;
  r.feed.write(SPI_FRAME * 5, d.data() + SPI_FRAME * 5, SPI_FRAME * 5);
  r.clk.ms = 6000;
  r.feed.write(SPI_FRAME * 10, d.data() + SPI_FRAME * 10, SPI_FRAME);
  TEST_ASSERT_EQUAL_UINT32(5000, r.feed.longestQuietMs());
  uint8_t f[SPI_FRAME];
  for (int i = 0; i < 4; i++) r.feed.pop(f);
  TEST_ASSERT_EQUAL(7, r.feed.ringMin());
  TEST_ASSERT_TRUE(r.feed.started());
  r.feed.write(SPI_FRAME * 11, d.data() + SPI_FRAME * 11, SPI_FRAME * 9);   // complete
  while (r.feed.pop(f)) {}
  TEST_ASSERT_EQUAL(7, r.feed.ringMin());                   // the final drain does not count
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_feed_stages_writes_into_frames);
  RUN_TEST(test_feed_drops_overlap_and_counts_dup);
  RUN_TEST(test_feed_whole_overlap_is_all_dup);
  RUN_TEST(test_feed_refuses_a_gap);
  RUN_TEST(test_feed_never_stages_past_size);
  RUN_TEST(test_feed_prefilled_at_capacity_minus_window_or_whole_body);
  RUN_TEST(test_feed_write_stops_when_ring_full_and_resumes_after_pop);
  RUN_TEST(test_feed_abort);
  RUN_TEST(test_feed_write_after_abort_is_refused);
  RUN_TEST(test_feed_flash_failure);
  RUN_TEST(test_feed_prefill_idle_fails_after_ten_minutes);
  RUN_TEST(test_feed_dry_after_the_first_pop);
  RUN_TEST(test_feed_dry_with_a_partial_window_buffered);
  RUN_TEST(test_feed_dry_limit_is_per_session);
  RUN_TEST(test_feed_prefill_override_and_clamp);
  RUN_TEST(test_feed_prefill_default_never_below_one);
  RUN_TEST(test_feed_staging_rate_at_first_pop);
  RUN_TEST(test_feed_complete_is_never_dry);
  RUN_TEST(test_feed_stats_quiet_and_ringmin);
  return UNITY_END();
}
