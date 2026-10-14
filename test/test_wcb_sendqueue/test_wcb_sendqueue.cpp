#include <unity.h>
#include <string.h>
#include "WcbSendQueue.h"

using namespace wcb;

struct Rec { int n = 0; uint8_t target = 0; uint16_t seq = 0; char text[200] = {0}; };
static Rec rec;
static void onRetx(void* ctx, uint8_t target, uint16_t seq, const char* text) {
  Rec* r = (Rec*)ctx;
  r->n++; r->target = target; r->seq = seq;
  strncpy(r->text, text, sizeof(r->text) - 1);
}

void setUp() { rec = Rec(); }
void tearDown() {}

void test_ack_frees_slot() {
  SendQueue q; PeerTable peers;
  q.add(2, 7, "#HL:POS 1", true, 0);
  TEST_ASSERT_EQUAL(1, q.activeCount());
  TEST_ASSERT_FALSE(q.onAck(3, 7));                // wrong sender
  TEST_ASSERT_FALSE(q.onAck(2, 8));                // wrong seq
  TEST_ASSERT_TRUE(q.onAck(2, 7));
  TEST_ASSERT_EQUAL(0, q.activeCount());
  q.tick(5000, peers, onRetx, &rec);
  TEST_ASSERT_EQUAL(0, rec.n);
}

void test_ensured_retries_then_gives_up() {
  SendQueue q; PeerTable peers;
  q.add(2, 7, "#HL:POS 1", true, 0);
  TEST_ASSERT_EQUAL(0, q.tick(499, peers, onRetx, &rec).retries);
  TEST_ASSERT_EQUAL(1, q.tick(500, peers, onRetx, &rec).retries);
  TEST_ASSERT_EQUAL(2, rec.target);
  TEST_ASSERT_EQUAL(7, rec.seq);                   // same seq on every retry
  TEST_ASSERT_EQUAL_STRING("#HL:POS 1", rec.text);
  q.tick(1000, peers, onRetx, &rec);
  q.tick(1500, peers, onRetx, &rec);
  TEST_ASSERT_EQUAL(3, rec.n);
  SendQueue::TickResult r = q.tick(2000, peers, onRetx, &rec);
  TEST_ASSERT_EQUAL(1, r.gaveUp);
  TEST_ASSERT_EQUAL(3, rec.n);
  TEST_ASSERT_EQUAL(0, q.activeCount());
}

void test_departed_target_gives_up_without_retry() {
  SendQueue q; PeerTable peers;
  peers.markSeen(2, 0);
  peers.sweep(60000);                              // seen once, now offline
  q.add(2, 7, "x", true, 60000);
  SendQueue::TickResult r = q.tick(60500, peers, onRetx, &rec);
  TEST_ASSERT_EQUAL(1, r.gaveUp);
  TEST_ASSERT_EQUAL(0, rec.n);
}

void test_never_seen_target_still_gets_retries() {
  SendQueue q; PeerTable peers;                    // target 7 never heard from
  q.add(7, 1, "x", true, 0);
  q.tick(500, peers, onRetx, &rec);
  TEST_ASSERT_EQUAL(1, rec.n);
}

void test_best_effort_freed_without_retry() {
  SendQueue q; PeerTable peers;
  q.add(2, 7, "x", false, 0);
  q.tick(1000, peers, onRetx, &rec);
  TEST_ASSERT_EQUAL(1, q.activeCount());
  q.tick(1001, peers, onRetx, &rec);
  TEST_ASSERT_EQUAL(0, q.activeCount());
  TEST_ASSERT_EQUAL(0, rec.n);
}

void test_full_of_ensured_returns_minus_one() {
  SendQueue q;
  for (int i = 0; i < (int)PENDING_MAX; i++) TEST_ASSERT_TRUE(q.add(2, (uint16_t)(i + 1), "x", true, 0) >= 0);
  TEST_ASSERT_EQUAL(-1, q.add(2, 99, "x", true, 0));
}

void test_full_reuses_oldest_best_effort() {
  SendQueue q;
  q.add(2, 1, "old", false, 0);
  q.add(2, 2, "newer", false, 10);
  for (int i = 2; i < (int)PENDING_MAX; i++) q.add(2, (uint16_t)(i + 1), "x", true, 20);
  int slot = q.add(2, 50, "y", true, 30);
  TEST_ASSERT_TRUE(slot >= 0);
  TEST_ASSERT_FALSE(q.onAck(2, 1));                // the oldest best-effort was evicted
  TEST_ASSERT_TRUE(q.onAck(2, 2));
}

void test_retry_timing_across_millis_rollover() {
  SendQueue q; PeerTable peers;
  q.add(2, 7, "x", true, 0xFFFFFF00u);
  TEST_ASSERT_EQUAL(0, q.tick(0xFFFFFFFFu, peers, onRetx, &rec).retries);   // 255 ms
  TEST_ASSERT_EQUAL(1, q.tick(0x000000F4u, peers, onRetx, &rec).retries);   // 500 ms, wrapped
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ack_frees_slot);
  RUN_TEST(test_ensured_retries_then_gives_up);
  RUN_TEST(test_departed_target_gives_up_without_retry);
  RUN_TEST(test_never_seen_target_still_gets_retries);
  RUN_TEST(test_best_effort_freed_without_retry);
  RUN_TEST(test_full_of_ensured_returns_minus_one);
  RUN_TEST(test_full_reuses_oldest_best_effort);
  RUN_TEST(test_retry_timing_across_millis_rollover);
  return UNITY_END();
}
