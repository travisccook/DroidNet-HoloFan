#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "SagLog.h"

using namespace fanlogic;

void setUp() {}
void tearDown() {}

void test_begin_checks_its_range() {
  SagLog g;
  TEST_ASSERT_FALSE(g.begin(1, 6, 60));              // level 1 does not exist on the C2
  TEST_ASSERT_FALSE(g.begin(2, 6, 60));              // 3.27 V: next to the supply; a stale 2 after a reset could loop the bootloader
  TEST_ASSERT_FALSE(g.begin(3, 8, 60));
  TEST_ASSERT_FALSE(g.begin(5, 4, 60));              // lo above hi
  TEST_ASSERT_FALSE(g.begin(3, 6, 9));
  TEST_ASSERT_FALSE(g.begin(3, 6, SAG_MAX_S + 1));
  TEST_ASSERT_TRUE(g.begin(3, 6, 60));
  TEST_ASSERT_TRUE(g.begin(7, 7, 10));
}

// 0.9.7: the level changes every SAG_DWELL_MS in a pseudo-random order (a
// fixed rotation phase-locked onto the 10 s heartbeat in 0.9.6), stays put
// within a dwell, and covers every level about equally.
void test_levels_are_shuffled_evenly() {
  SagLog g;
  TEST_ASSERT_TRUE(g.begin(3, 7, 300));
  int count[8] = {0};
  bool changesOrder = false;
  int prev = -1, prevPrev = -1;
  for (uint32_t slot = 0; slot < 10000; slot++) {
    uint32_t ms = slot * SAG_DWELL_MS;
    int l = g.levelAt(ms);
    TEST_ASSERT_TRUE(l >= 3 && l <= 7);
    TEST_ASSERT_EQUAL(l, g.levelAt(ms + SAG_DWELL_MS - 1));
    count[l]++;
    if (slot >= 2 && !(prev == prevPrev + 1 || (prevPrev == 7 && prev == 3))) changesOrder = true;
    prevPrev = prev;
    prev = l;
  }
  for (int l = 3; l <= 7; l++) TEST_ASSERT_INT_WITHIN(200, 2000, count[l]);
  TEST_ASSERT_TRUE(changesOrder);                    // not a plain rotation
  SagLog one;
  TEST_ASSERT_TRUE(one.begin(6, 6, 60));
  TEST_ASSERT_EQUAL(6, one.levelAt(12345));
}

// A periodic event (10 s heartbeat) lands on different levels over time.
void test_periodic_events_see_every_level() {
  SagLog g;
  TEST_ASSERT_TRUE(g.begin(3, 7, 300));
  bool seen[8] = {false};
  for (uint32_t t = 4000; t < 300000; t += 10000) seen[g.levelAt(t)] = true;
  int n = 0;
  for (int l = 3; l <= 7; l++) n += seen[l] ? 1 : 0;
  TEST_ASSERT_TRUE(n >= 4);
}

void test_done_after_the_window() {
  SagLog g;
  TEST_ASSERT_TRUE(g.begin(3, 6, 10));
  TEST_ASSERT_FALSE(g.done(9999));
  TEST_ASSERT_TRUE(g.done(10000));
}

void test_summary_counts_per_level() {
  SagLog g;
  TEST_ASSERT_TRUE(g.begin(3, 4, 10));
  uint32_t watched[8] = {0}, hits3 = 0;
  for (uint32_t ms = 0; ms < 10000; ms++) {
    int l = g.levelAt(ms);
    bool hit = l == 3 && ms % 10 == 0;                 // level 3 crossed every 10 ms, level 4 never
    watched[l]++;
    if (hit) hits3++;
    g.sample(ms, l, hit, false);
  }
  char want[200], out[200];
  snprintf(want, sizeof(want), "[sag] 10 s, ms crossed/watched: L3(3.10V)=%lu/%lu L4(2.92V)=0/%lu; det 0",
           (unsigned long)hits3, (unsigned long)watched[3], (unsigned long)watched[4]);
  TEST_ASSERT_TRUE(g.formatSummary(out, sizeof(out)) > 0);
  TEST_ASSERT_EQUAL_STRING(want, out);
  TEST_ASSERT_EQUAL(3, g.deepest());
}

// The comparator's DET bit (semantics undocumented) is counted apart from
// the latched interrupt bit and does not mark the timeline.
void test_det_is_counted_apart() {
  SagLog g;
  TEST_ASSERT_TRUE(g.begin(5, 5, 10));
  g.sample(100, 5, false, true);
  g.sample(101, 5, false, true);
  g.sample(102, 5, true, false);
  char out[200];
  g.formatSummary(out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("[sag] 10 s, ms crossed/watched: L5(2.76V)=1/3; det 2", out);
  TEST_ASSERT_EQUAL(1, g.timelineLines());
  g.formatTimeline(0, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("[sag] s 0-9: 5---------", out);
}

// One character per second: the deepest level crossed in it ('-' none).
void test_timeline_marks_the_deepest_level_each_second() {
  SagLog g;
  TEST_ASSERT_TRUE(g.begin(3, 6, 12));
  g.sample(500, 3, true, false);
  g.sample(600, 5, true, false);
  g.sample(700, 4, true, false);                       // second 0: deepest 5
  g.sample(2100, 6, true, false);                      // second 2: 6
  g.sample(2200, 6, false, false);
  g.sample(11999, 3, true, false);                     // second 11: 3
  g.sample(12000, 7, true, false);                     // past the window: ignored
  char out[200];
  TEST_ASSERT_EQUAL(1, g.timelineLines());
  TEST_ASSERT_TRUE(g.formatTimeline(0, out, sizeof(out)) > 0);
  TEST_ASSERT_EQUAL_STRING("[sag] s 0-11: 5-6--------3", out);
  TEST_ASSERT_EQUAL(6, g.deepest());
}

void test_timeline_splits_long_windows() {
  SagLog g;
  TEST_ASSERT_TRUE(g.begin(3, 3, 250));
  g.sample(149500, 3, true, false);                    // second 149
  TEST_ASSERT_EQUAL(3, g.timelineLines());             // 100 s per line
  char out[200];
  TEST_ASSERT_TRUE(g.formatTimeline(1, out, sizeof(out)) > 0);
  TEST_ASSERT_EQUAL(0, strncmp(out, "[sag] s 100-199: ", 17));
  TEST_ASSERT_EQUAL('3', out[17 + 49]);
  TEST_ASSERT_EQUAL(17 + 100, (int)strlen(out));
  TEST_ASSERT_TRUE(g.formatTimeline(2, out, sizeof(out)) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "[sag] s 200-249: --------------------------------------------------", out);
  TEST_ASSERT_EQUAL(-1, g.formatTimeline(3, out, sizeof(out)));
}

void test_quiet_supply_reports_none() {
  SagLog g;
  TEST_ASSERT_TRUE(g.begin(6, 7, 10));
  uint32_t w6 = 0, w7 = 0;
  for (uint32_t ms = 0; ms < 10000; ms += 1) {
    int l = g.levelAt(ms);
    (l == 6 ? w6 : w7)++;
    g.sample(ms, l, false, false);
  }
  TEST_ASSERT_EQUAL(0, g.deepest());
  char want[200], out[200];
  snprintf(want, sizeof(want), "[sag] 10 s, ms crossed/watched: L6(2.64V)=0/%lu L7(2.51V)=0/%lu; det 0",
           (unsigned long)w6, (unsigned long)w7);
  g.formatSummary(out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING(want, out);
}

// #HLFJ (0.9.7): per level, how many probe frames crossed it, and how many
// equal idle windows (no probe) crossed it (the background).
void test_probe_log() {
  ProbeLog p;
  TEST_ASSERT_FALSE(p.begin(2, 7, 20));
  TEST_ASSERT_FALSE(p.begin(3, 7, 1));
  TEST_ASSERT_FALSE(p.begin(3, 7, 21));
  TEST_ASSERT_TRUE(p.begin(3, 5, 20));
  TEST_ASSERT_EQUAL(3, p.levels());
  TEST_ASSERT_EQUAL(3, p.levelOf(0));
  TEST_ASSERT_EQUAL(5, p.levelOf(2));
  for (int i = 0; i < PROBE_FRAMES; i++) {
    p.frame(3, true, i == 0);                          // every probe crosses 3.10 V; one idle window too
    p.frame(4, i < 5, false);
    p.frame(5, false, false);
  }
  char out[200];
  TEST_ASSERT_TRUE(p.format(out, sizeof(out)) > 0);
  TEST_ASSERT_EQUAL_STRING("[probe] 20 dBm, 20 frames/level, crossed (idle): L3(3.10V)=20 (1) L4(2.92V)=5 (0) L5(2.76V)=0 (0)",
                           out);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_begin_checks_its_range);
  RUN_TEST(test_levels_are_shuffled_evenly);
  RUN_TEST(test_periodic_events_see_every_level);
  RUN_TEST(test_done_after_the_window);
  RUN_TEST(test_summary_counts_per_level);
  RUN_TEST(test_det_is_counted_apart);
  RUN_TEST(test_timeline_marks_the_deepest_level_each_second);
  RUN_TEST(test_timeline_splits_long_windows);
  RUN_TEST(test_quiet_supply_reports_none);
  RUN_TEST(test_probe_log);
  return UNITY_END();
}
