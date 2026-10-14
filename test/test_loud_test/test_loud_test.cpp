#include <unity.h>
#include <utility>
#include <vector>
#include "LoudTest.h"

using namespace fanlogic;
using A = LoudTest::Action;

void setUp() {}
void tearDown() {}

// The mesh task as the firmware drives it: polls every 5 ms and answers each
// VOLUME_UP / VOLUME_RESTORE with the MCU's read-back `latency` ms later.
struct Sim {
  LoudTest t;
  std::vector<std::pair<A, uint32_t>> acts;
  int upValue = 3;                       // read back after "m 3"
  std::vector<int> restoreValues{2};     // read back after each restore (last one repeats)
  size_t restores = 0;
  uint32_t latency = 200;
  bool silent = false;                   // the MCU task never reports
  bool pending = false;
  uint32_t resultAt = 0;
  int resultVal = 0;
  bool radioDownSeen = false;

  void run(uint32_t from, uint32_t to) {
    for (uint32_t i = 0; i <= to - from; i += 5) {
      uint32_t ms = from + i;
      if (pending && (int32_t)(ms - resultAt) >= 0) { pending = false; t.volumeResult(resultVal, ms); }
      A a = t.poll(ms);
      if (t.radioDown()) radioDownSeen = true;
      if (a == A::NONE) continue;
      acts.push_back({a, ms});
      if (silent) continue;
      if (a == A::VOLUME_UP) { pending = true; resultAt = ms + latency; resultVal = upValue; }
      if (a == A::VOLUME_RESTORE) {
        size_t k = restores < restoreValues.size() ? restores : restoreValues.size() - 1;
        restores++;
        pending = true; resultAt = ms + latency; resultVal = restoreValues[k];
      }
    }
  }
  int count(A a) const {
    int n = 0;
    for (const auto& p : acts) if (p.first == a) n++;
    return n;
  }
  uint32_t at(A a, int nth = 0) const {
    for (const auto& p : acts) if (p.first == a && nth-- == 0) return p.second;
    return 0xFFFFFFFFu;
  }
};

void test_radio_on_sequence() {
  Sim s;
  TEST_ASSERT_TRUE(s.t.start(1060, 2, 1000));
  TEST_ASSERT_TRUE(s.t.active());
  TEST_ASSERT_FALSE(s.t.radioOff());
  TEST_ASSERT_EQUAL(60, s.t.seconds());
  TEST_ASSERT_EQUAL(2, s.t.restoreVolume());
  s.run(1000, 70000);
  TEST_ASSERT_EQUAL(3, (int)s.acts.size());
  TEST_ASSERT_EQUAL((int)A::VOLUME_UP, (int)s.acts[0].first);
  TEST_ASSERT_EQUAL(1000, s.acts[0].second);
  TEST_ASSERT_EQUAL((int)A::VOLUME_RESTORE, (int)s.acts[1].first);
  TEST_ASSERT_EQUAL(1200 + 60000, s.acts[1].second);   // the hold counts from the read-back of 3
  TEST_ASSERT_EQUAL((int)A::REPORT, (int)s.acts[2].first);
  TEST_ASSERT_EQUAL(61400, s.acts[2].second);          // after the restore read back
  TEST_ASSERT_TRUE(s.t.confirmed());
  TEST_ASSERT_EQUAL(3, s.t.upReadBack());
  TEST_ASSERT_EQUAL(2, s.t.restoreReadBack());
  TEST_ASSERT_FALSE(s.radioDownSeen);
  TEST_ASSERT_FALSE(s.t.active());
}

// r=0 (0.9.6): the radio goes off BEFORE the volume step, so the step and
// the whole hold run without it; it comes back after the restore read back.
void test_radio_off_sequence() {
  Sim s;
  s.restoreValues = {1};
  TEST_ASSERT_TRUE(s.t.start(120, 1, 0));
  TEST_ASSERT_TRUE(s.t.radioOff());
  TEST_ASSERT_EQUAL(0, s.t.mode());
  TEST_ASSERT_EQUAL(1, s.t.restoreVolume());
  s.run(0, 130000);
  TEST_ASSERT_EQUAL(5, (int)s.acts.size());
  TEST_ASSERT_EQUAL((int)A::RADIO_OFF, (int)s.acts[0].first);
  TEST_ASSERT_EQUAL(LOUD_RADIO_OFF_MS, s.acts[0].second);          // the start line goes out first
  TEST_ASSERT_EQUAL((int)A::VOLUME_UP, (int)s.acts[1].first);
  TEST_ASSERT_EQUAL(LOUD_RADIO_OFF_MS + LOUD_QUIET_MS, s.acts[1].second);
  uint32_t up = s.acts[1].second;
  TEST_ASSERT_EQUAL((int)A::VOLUME_RESTORE, (int)s.acts[2].first);
  TEST_ASSERT_EQUAL(up + 200 + 120000, s.acts[2].second);          // the hold counts from the read-back
  TEST_ASSERT_EQUAL((int)A::RADIO_ON, (int)s.acts[3].first);
  TEST_ASSERT_EQUAL(up + 200 + 120000 + 200, s.acts[3].second);    // only after the restore read back
  TEST_ASSERT_EQUAL((int)A::REPORT, (int)s.acts[4].first);
  TEST_ASSERT_EQUAL(s.acts[3].second + LOUD_REPORT_MS, s.acts[4].second);
  TEST_ASSERT_TRUE(s.radioDownSeen);
  TEST_ASSERT_FALSE(s.t.radioDown());
  TEST_ASSERT_EQUAL(1, s.t.restoreReadBack());
}

// r=2 (0.9.6): the same sequence as r=0 (the glue keeps Wi-Fi started).
void test_receive_only_sequence() {
  Sim s;
  TEST_ASSERT_TRUE(s.t.start(2010, 2, 0));
  TEST_ASSERT_TRUE(s.t.radioOff());
  TEST_ASSERT_EQUAL(2, s.t.mode());
  s.run(0, 20000);
  TEST_ASSERT_EQUAL((int)A::RADIO_OFF, (int)s.acts[0].first);
  TEST_ASSERT_EQUAL((int)A::VOLUME_UP, (int)s.acts[1].first);
  TEST_ASSERT_EQUAL(1, s.count(A::RADIO_ON));
}

void test_radio_is_down_only_between_off_and_on() {
  Sim s;
  TEST_ASSERT_TRUE(s.t.start(10, 2, 0));
  s.run(0, LOUD_RADIO_OFF_MS - 5);
  TEST_ASSERT_FALSE(s.t.radioDown());
  uint32_t up = LOUD_RADIO_OFF_MS + LOUD_QUIET_MS;
  s.run(LOUD_RADIO_OFF_MS, up + 200 + 10000 + 195);
  TEST_ASSERT_TRUE(s.t.radioDown());                   // restore sent, not read back yet
  s.run(up + 200 + 10000 + 200, up + 200 + 10000 + 200);
  TEST_ASSERT_FALSE(s.t.radioDown());
}

// The MCU did not take "m 3" (read back 2): no radio-off, no hold, restore at once.
void test_volume_3_not_confirmed_stops_the_test() {
  Sim s;
  s.upValue = 2;
  TEST_ASSERT_TRUE(s.t.start(1300, 2, 0));
  s.run(0, 20000);
  TEST_ASSERT_EQUAL(0, s.count(A::RADIO_OFF));
  TEST_ASSERT_EQUAL(0, s.count(A::RADIO_ON));
  TEST_ASSERT_EQUAL(1, s.count(A::VOLUME_RESTORE));
  TEST_ASSERT_EQUAL(200, s.at(A::VOLUME_RESTORE));
  TEST_ASSERT_EQUAL(1, s.count(A::REPORT));
  TEST_ASSERT_FALSE(s.t.confirmed());
  TEST_ASSERT_EQUAL(2, s.t.upReadBack());
  TEST_ASSERT_FALSE(s.t.active());
}

// The MCU task never reports: the test gives up on volume 3 after
// LOUD_RESULT_MS, restores (bounded) and never touches the radio.
void test_silent_mcu_never_turns_the_radio_off() {
  Sim s;
  s.silent = true;
  TEST_ASSERT_TRUE(s.t.start(1300, 2, 0));
  s.run(0, 60000);
  TEST_ASSERT_EQUAL(0, s.count(A::RADIO_OFF));
  TEST_ASSERT_EQUAL(LOUD_RESULT_MS, s.at(A::VOLUME_RESTORE));
  TEST_ASSERT_EQUAL(1, s.count(A::REPORT));
  TEST_ASSERT_TRUE(s.at(A::REPORT) <= LOUD_RESULT_MS + LOUD_RESTORE_MAX_MS + 5);
  TEST_ASSERT_FALSE(s.t.confirmed());
  TEST_ASSERT_EQUAL(0, s.t.upReadBack());
  TEST_ASSERT_FALSE(s.t.active());
}

// r=0 with a silent MCU task: the radio is back within a fixed bound.
void test_silent_mcu_radio_off_comes_back() {
  Sim s;
  s.silent = true;
  TEST_ASSERT_TRUE(s.t.start(300, 2, 0));
  s.run(0, 60000);
  TEST_ASSERT_EQUAL(1, s.count(A::RADIO_OFF));
  TEST_ASSERT_EQUAL(1, s.count(A::RADIO_ON));
  TEST_ASSERT_TRUE(s.at(A::RADIO_ON) <= LOUD_RADIO_OFF_MS + LOUD_QUIET_MS + LOUD_RESULT_MS + LOUD_RESTORE_MAX_MS + 5);
  TEST_ASSERT_FALSE(s.t.confirmed());
}

void test_restore_is_resent_until_it_reads_back() {
  Sim s;
  s.restoreValues = {3, 2};
  TEST_ASSERT_TRUE(s.t.start(10, 2, 0));
  s.run(0, 30000);
  TEST_ASSERT_EQUAL(2, s.count(A::VOLUME_RESTORE));
  TEST_ASSERT_TRUE(s.at(A::RADIO_ON) > s.at(A::VOLUME_RESTORE, 1));
  TEST_ASSERT_EQUAL(2, s.t.restoreReadBack());
}

// The MCU keeps reporting 3: after LOUD_RESTORE_TRIES the radio comes back
// anyway (the fan must not stay off the mesh), and the report says so.
void test_radio_comes_back_when_the_restore_fails() {
  Sim s;
  s.restoreValues = {3};
  TEST_ASSERT_TRUE(s.t.start(10, 2, 0));
  s.run(0, 60000);
  TEST_ASSERT_EQUAL(LOUD_RESTORE_TRIES, s.count(A::VOLUME_RESTORE));
  TEST_ASSERT_EQUAL(1, s.count(A::RADIO_ON));
  TEST_ASSERT_EQUAL(3, s.t.restoreReadBack());
  TEST_ASSERT_FALSE(s.t.active());
}

// The MCU task goes silent during the hold: the radio is back within
// LOUD_RESTORE_MAX_MS of the first restore.
void test_silent_restore_still_brings_the_radio_back() {
  Sim s;
  TEST_ASSERT_TRUE(s.t.start(10, 2, 0));
  s.run(0, 10000);                                     // past the read-back of 3
  s.silent = true;
  s.run(10005, 60000);
  uint32_t first = s.at(A::VOLUME_RESTORE);
  TEST_ASSERT_EQUAL(1, s.count(A::RADIO_ON));
  TEST_ASSERT_TRUE(s.at(A::RADIO_ON) - first <= LOUD_RESTORE_MAX_MS);
  TEST_ASSERT_EQUAL(0, s.t.restoreReadBack());
}

// The phase and timing the firmware keeps in .noinit RAM for the next boot.
void test_phases_and_hold_time() {
  Sim s;
  TEST_ASSERT_EQUAL((int)LoudPhase::IDLE, (int)s.t.phase());
  TEST_ASSERT_TRUE(s.t.start(30, 2, 1000));
  TEST_ASSERT_EQUAL((int)LoudPhase::PRE, (int)s.t.phase());
  s.run(1000, 1000 + LOUD_RADIO_OFF_MS);
  TEST_ASSERT_EQUAL((int)LoudPhase::QUIET, (int)s.t.phase());
  uint32_t up = 1000 + LOUD_RADIO_OFF_MS + LOUD_QUIET_MS;
  s.run(1000 + LOUD_RADIO_OFF_MS + 5, up);
  TEST_ASSERT_EQUAL((int)LoudPhase::STEP, (int)s.t.phase());
  TEST_ASSERT_EQUAL(0u, s.t.holdMs(up));
  s.run(up + 5, up + 200 + 12345);
  TEST_ASSERT_EQUAL((int)LoudPhase::HOLD, (int)s.t.phase());
  TEST_ASSERT_EQUAL(12345u, s.t.holdMs(up + 200 + 12345));
  TEST_ASSERT_EQUAL(up + 200 + 12345 - 1000, s.t.testMs(up + 200 + 12345));
  s.run(up + 200 + 12350, up + 200 + 30000 + 100);
  TEST_ASSERT_EQUAL((int)LoudPhase::RESTORE, (int)s.t.phase());
  s.run(up + 200 + 30000 + 105, up + 200 + 30000 + 200);
  TEST_ASSERT_EQUAL((int)LoudPhase::REJOIN, (int)s.t.phase());
  s.run(up + 200 + 30000 + 205, up + 200 + 30000 + 200 + LOUD_REPORT_MS);
  TEST_ASSERT_EQUAL((int)LoudPhase::DONE, (int)s.t.phase());
}

void test_record_seal_and_format() {
  LoudRecord r = {};
  r.seq = 7; r.param = 300; r.phase = (uint8_t)LoudPhase::HOLD; r.testMs = 45678; r.holdMs = 43210;
  TEST_ASSERT_FALSE(loudRecordValid(r));               // never sealed (power-on garbage)
  loudRecordSeal(r);
  TEST_ASSERT_TRUE(loudRecordValid(r));
  char out[160];
  TEST_ASSERT_TRUE(loudRecordFormat(r, 9, out, sizeof(out)) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "[test] RESET during #HLFY0300 (test 7): phase HOLD, 43.2 s into the hold, 45.6 s into the test; rst=9", out);
  r.holdMs = 1;                                        // changed without a seal
  TEST_ASSERT_FALSE(loudRecordValid(r));
}

void test_restore_volume_defaults_to_the_cap() {
  LoudTest t;
  TEST_ASSERT_TRUE(t.start(1010, 0, 0));               // volume unknown
  TEST_ASSERT_EQUAL(VOLUME_MAX, t.restoreVolume());
  LoudTest u;
  TEST_ASSERT_TRUE(u.start(1010, 3, 0));               // never restore past the cap
  TEST_ASSERT_EQUAL(VOLUME_MAX, u.restoreVolume());
}

// 0.9.11: with the cap at 3 (#HLFH3) the hold goes back to the 3 it found.
void test_restore_volume_follows_the_cap() {
  LoudTest t;
  TEST_ASSERT_TRUE(t.start(1010, 3, 0, 3));
  TEST_ASSERT_EQUAL(3, t.restoreVolume());
  LoudTest u;
  TEST_ASSERT_TRUE(u.start(1010, 0, 0, 3));             // volume unknown: the cap
  TEST_ASSERT_EQUAL(3, u.restoreVolume());
  LoudTest v;
  TEST_ASSERT_TRUE(v.start(1010, 2, 0, 3));
  TEST_ASSERT_EQUAL(2, v.restoreVolume());
}

void test_refuses_bad_param_and_a_second_start() {
  LoudTest t;
  TEST_ASSERT_FALSE(t.start(9, 2, 0));
  TEST_ASSERT_FALSE(t.start(301, 2, 0));
  TEST_ASSERT_FALSE(t.start(3060, 2, 0));
  TEST_ASSERT_FALSE(t.start(-1, 2, 0));
  TEST_ASSERT_FALSE(t.active());
  TEST_ASSERT_TRUE(t.start(10, 2, 0));
  TEST_ASSERT_FALSE(t.start(1010, 2, 5));              // one test at a time
  TEST_ASSERT_TRUE(t.radioOff());                      // the running test is unchanged
}

void test_wraps_with_the_millisecond_clock() {
  Sim s;
  uint32_t t0 = 0xFFFFFFFFu - 4999u;                    // 5 s before uint32 wrap
  TEST_ASSERT_TRUE(s.t.start(1010, 2, t0));
  s.run(t0, t0 + 20000);
  TEST_ASSERT_EQUAL(3, (int)s.acts.size());
  TEST_ASSERT_EQUAL((uint32_t)(t0 + 200 + 10000), s.at(A::VOLUME_RESTORE));
}

// A restore that could not be queued is repeated LOUD_RETRY_MS later and
// does not count as a try.
void test_retry_repeats_the_restore() {
  LoudTest t;
  TEST_ASSERT_TRUE(t.start(1010, 2, 0));
  TEST_ASSERT_EQUAL((int)A::VOLUME_UP, (int)t.poll(0));
  t.volumeResult(3, 100);
  TEST_ASSERT_EQUAL((int)A::VOLUME_RESTORE, (int)t.poll(10100));
  t.retry(10100);
  TEST_ASSERT_EQUAL((int)A::NONE, (int)t.poll(10100 + LOUD_RETRY_MS - 5));
  TEST_ASSERT_EQUAL((int)A::VOLUME_RESTORE, (int)t.poll(10100 + LOUD_RETRY_MS));
  t.volumeResult(2, 10400);
  TEST_ASSERT_EQUAL((int)A::REPORT, (int)t.poll(10400));
}

void test_cancel_before_anything_changed() {
  LoudTest t;
  TEST_ASSERT_TRUE(t.start(1010, 2, 0));
  TEST_ASSERT_EQUAL((int)A::VOLUME_UP, (int)t.poll(0));
  t.cancel();                                          // volume 3 could not be queued
  TEST_ASSERT_FALSE(t.active());
  TEST_ASSERT_EQUAL((int)A::NONE, (int)t.poll(LOUD_RADIO_OFF_MS));
  TEST_ASSERT_TRUE(t.start(1010, 2, 100));             // free for the next test
}

// A read-back that arrives when none is awaited changes nothing.
void test_unexpected_result_is_ignored() {
  LoudTest t;
  t.volumeResult(3, 0);
  TEST_ASSERT_FALSE(t.active());
  TEST_ASSERT_TRUE(t.start(1010, 2, 0));
  t.volumeResult(3, 0);                                // before VOLUME_UP went out
  TEST_ASSERT_EQUAL((int)A::VOLUME_UP, (int)t.poll(0));
  TEST_ASSERT_FALSE(t.confirmed());
}

void test_idle_polls_nothing() {
  LoudTest t;
  TEST_ASSERT_EQUAL((int)A::NONE, (int)t.poll(0));
  TEST_ASSERT_EQUAL((int)A::NONE, (int)t.poll(123456));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_radio_on_sequence);
  RUN_TEST(test_radio_off_sequence);
  RUN_TEST(test_receive_only_sequence);
  RUN_TEST(test_radio_is_down_only_between_off_and_on);
  RUN_TEST(test_volume_3_not_confirmed_stops_the_test);
  RUN_TEST(test_silent_mcu_never_turns_the_radio_off);
  RUN_TEST(test_silent_mcu_radio_off_comes_back);
  RUN_TEST(test_phases_and_hold_time);
  RUN_TEST(test_record_seal_and_format);
  RUN_TEST(test_restore_is_resent_until_it_reads_back);
  RUN_TEST(test_radio_comes_back_when_the_restore_fails);
  RUN_TEST(test_silent_restore_still_brings_the_radio_back);
  RUN_TEST(test_restore_volume_defaults_to_the_cap);
  RUN_TEST(test_restore_volume_follows_the_cap);
  RUN_TEST(test_refuses_bad_param_and_a_second_start);
  RUN_TEST(test_wraps_with_the_millisecond_clock);
  RUN_TEST(test_retry_repeats_the_restore);
  RUN_TEST(test_cancel_before_anything_changed);
  RUN_TEST(test_unexpected_result_is_ignored);
  RUN_TEST(test_idle_polls_nothing);
  return UNITY_END();
}
