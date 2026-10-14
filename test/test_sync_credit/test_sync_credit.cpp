#include <unity.h>
#include <deque>
#include <vector>
#include "ClipOps.h"

using namespace fanlogic;

void setUp() {}
void tearDown() {}

// GPIO18 as a script: each sample pops the next level (the last one
// repeats); counts samples, yields and delays.
class ScriptLine : public SyncLine {
 public:
  std::deque<bool> levels{false};
  long samples = 0, yields = 0;
  std::vector<uint32_t> delays;
  bool high() override {
    samples++;
    bool v = levels.front();
    if (levels.size() > 1) levels.pop_front();
    return v;
  }
  void yield() override { yields++; }
  void delayMs(uint32_t ms) override { delays.push_back(ms); }
};

static void script(ScriptLine& l, std::initializer_list<bool> v) { l.levels.assign(v); }

// --- after each window: the LOW-count wait (behaviour since 0.6, unchanged) ---

void test_window_ok_after_100_low_samples() {
  ScriptLine l; SyncCredit c(l);
  TEST_ASSERT_TRUE(c.afterWindow());
  TEST_ASSERT_EQUAL(100, c.lastLow());
  TEST_ASSERT_EQUAL(100, (int)l.samples);
  TEST_ASSERT_EQUAL(1, c.pending());
}

void test_window_counts_low_samples_cumulatively() {
  ScriptLine l; SyncCredit c(l);
  for (int i = 0; i < 300; i++) l.levels.push_back(i % 2 == 0);   // HIGH, LOW, HIGH, ...
  l.levels.pop_front();
  TEST_ASSERT_TRUE(c.afterWindow());
  TEST_ASSERT_EQUAL(100, c.lastLow());
  TEST_ASSERT_EQUAL(200, (int)l.samples);
}

void test_window_never_low_fails_at_the_spin_cap() {
  ScriptLine l; SyncCredit c(l);
  script(l, {true});
  TEST_ASSERT_FALSE(c.afterWindow());
  TEST_ASSERT_EQUAL(0, c.lastLow());
  TEST_ASSERT_EQUAL((long)SYNC_SPIN_CAP, l.samples);
  TEST_ASSERT_EQUAL(SYNC_SPIN_CAP / 1024, (int)l.yields);   // a yield every 1024 samples
}

void test_window_at_the_cap_needs_ten_low() {
  ScriptLine l; SyncCredit c(l);
  l.levels.assign(9, false);
  l.levels.push_back(true);
  TEST_ASSERT_FALSE(c.afterWindow());
  ScriptLine l2; SyncCredit c2(l2);
  l2.levels.assign(10, false);
  l2.levels.push_back(true);
  TEST_ASSERT_TRUE(c2.afterWindow());
  TEST_ASSERT_EQUAL(10, c2.lastLow());
}

// --- before every frame: the stock credit check ---

void test_nothing_pending_samples_nothing() {
  ScriptLine l; SyncCredit c(l);
  TEST_ASSERT_TRUE(c.beforeFrame());
  TEST_ASSERT_EQUAL(0, (int)l.samples);
}

void test_high_sample_retires_a_pending_window() {
  ScriptLine l; SyncCredit c(l);
  c.afterWindow();                        // LOW: pending 1
  long before = l.samples;
  script(l, {true});
  TEST_ASSERT_TRUE(c.beforeFrame());
  TEST_ASSERT_EQUAL(0, c.pending());
  TEST_ASSERT_EQUAL(1, (int)(l.samples - before));
  TEST_ASSERT_TRUE(c.beforeFrame());      // nothing pending: no more samples
  TEST_ASSERT_EQUAL(1, (int)(l.samples - before));
  TEST_ASSERT_EQUAL(0, (int)l.delays.size());
}

void test_one_window_may_stay_outstanding() {
  ScriptLine l; SyncCredit c(l);
  c.afterWindow();
  long before = l.samples;
  TEST_ASSERT_TRUE(c.beforeFrame());      // LOW: still busy, but only one outstanding
  TEST_ASSERT_EQUAL(1, c.pending());
  TEST_ASSERT_EQUAL(1, (int)(l.samples - before));
  TEST_ASSERT_EQUAL(0, c.highWaits());
  TEST_ASSERT_EQUAL(0, (int)l.delays.size());
}

// Two windows outstanding: the next frame waits for HIGH (yielding), then
// rests SYNC_REST_MS, as stock (vTaskDelay(1) at 250 Hz).
void test_second_outstanding_window_blocks_until_high() {
  ScriptLine l; SyncCredit c(l);
  c.afterWindow();
  c.afterWindow();
  TEST_ASSERT_EQUAL(2, c.pending());
  long before = l.samples, yieldsBefore = l.yields;
  l.levels.assign(500, false);            // the MCU is busy for 500 more samples
  l.levels.push_back(true);
  TEST_ASSERT_TRUE(c.beforeFrame());
  TEST_ASSERT_EQUAL(1, c.pending());
  TEST_ASSERT_EQUAL(1, c.highWaits());
  TEST_ASSERT_EQUAL(501, (int)(l.samples - before));
  TEST_ASSERT_TRUE(l.yields - yieldsBefore >= 499);   // yields while LOW
  TEST_ASSERT_EQUAL(1, (int)l.delays.size());
  TEST_ASSERT_EQUAL_UINT32(SYNC_REST_MS, l.delays[0]);
}

// The first sample being HIGH already retires one: no wait.
void test_two_outstanding_high_sample_needs_no_wait() {
  ScriptLine l; SyncCredit c(l);
  c.afterWindow();
  c.afterWindow();
  script(l, {true});
  TEST_ASSERT_TRUE(c.beforeFrame());
  TEST_ASSERT_EQUAL(1, c.pending());
  TEST_ASSERT_EQUAL(0, c.highWaits());
  TEST_ASSERT_EQUAL(0, (int)l.delays.size());
}

// Unlike stock (which waits forever), the HIGH wait is capped: sync error.
void test_high_wait_is_capped() {
  ScriptLine l; SyncCredit c(l);
  c.afterWindow();
  c.afterWindow();
  long before = l.samples;
  TEST_ASSERT_FALSE(c.beforeFrame());     // LOW forever
  TEST_ASSERT_EQUAL(1L + SYNC_SPIN_CAP, l.samples - before);   // the credit sample, then the capped wait
  TEST_ASSERT_EQUAL(1, c.highWaits());
}

void test_reset_clears_pending_and_counts() {
  ScriptLine l; SyncCredit c(l);
  c.afterWindow();
  c.afterWindow();
  l.levels.assign(5, false);
  l.levels.push_back(true);
  c.beforeFrame();
  c.reset();
  TEST_ASSERT_EQUAL(0, c.pending());
  TEST_ASSERT_EQUAL(0, c.highWaits());
  TEST_ASSERT_EQUAL(0, c.lastLow());
}

// A two-buffer MCU that stores each window in `work` ticks and holds GPIO18
// LOW while it has a window to store; a third window arriving while two
// are unstored is an overrun. One tick per sample, `frameCost` per frame,
// 100 per ms of delay.
class TwoBufferMcu : public SyncLine {
 public:
  long work = 5000, frameCost = 50, left = 0;
  int queued = 0, overruns = 0, maxQueued = 0;
  void tick(long n) {
    while (n-- > 0 && queued > 0)
      if (--left <= 0 && --queued > 0) left = work;
  }
  void frame() { tick(frameCost); }
  void windowIn() {
    if (queued == 2) { overruns++; return; }
    if (queued++ == 0) left = work;
    if (queued > maxQueued) maxQueued = queued;
  }
  bool high() override { tick(1); return queued == 0; }
  void yield() override {}
  void delayMs(uint32_t ms) override { tick((long)ms * 100); }
};

static int sendWindows(TwoBufferMcu& m, SyncCredit* c, int windows) {
  for (int w = 0; w < windows; w++) {
    for (int f = 0; f < SPI_WINDOW_FRAMES; f++) {
      if (c && !c->beforeFrame()) return -1;
      m.frame();
    }
    m.windowIn();
    if (c && !c->afterWindow()) return -1;
  }
  return 0;
}

// The MCU needs 5000 ticks per window, the sender 1000 (+ the LOW wait):
// without the credit check a third window lands on a busy MCU; with it the
// sender waits for HIGH instead.
void test_slow_mcu_blocks_the_third_window_until_high() {
  TwoBufferMcu raw;
  for (int w = 0; w < 10; w++) {         // what 0.7.1 did: frames back to back
    for (int f = 0; f < SPI_WINDOW_FRAMES; f++) raw.frame();
    raw.windowIn();
    for (int i = 0; i < 100; i++) raw.high();   // the LOW-count wait
  }
  TEST_ASSERT_TRUE(raw.overruns > 0);

  TwoBufferMcu m;
  SyncCredit c(m);
  TEST_ASSERT_EQUAL(0, sendWindows(m, &c, 10));
  TEST_ASSERT_EQUAL(0, m.overruns);
  TEST_ASSERT_TRUE(m.maxQueued <= 2);
  TEST_ASSERT_TRUE(c.highWaits() > 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_window_ok_after_100_low_samples);
  RUN_TEST(test_window_counts_low_samples_cumulatively);
  RUN_TEST(test_window_never_low_fails_at_the_spin_cap);
  RUN_TEST(test_window_at_the_cap_needs_ten_low);
  RUN_TEST(test_nothing_pending_samples_nothing);
  RUN_TEST(test_high_sample_retires_a_pending_window);
  RUN_TEST(test_one_window_may_stay_outstanding);
  RUN_TEST(test_second_outstanding_window_blocks_until_high);
  RUN_TEST(test_two_outstanding_high_sample_needs_no_wait);
  RUN_TEST(test_high_wait_is_capped);
  RUN_TEST(test_reset_clears_pending_and_counts);
  RUN_TEST(test_slow_mcu_blocks_the_third_window_until_high);
  return UNITY_END();
}
