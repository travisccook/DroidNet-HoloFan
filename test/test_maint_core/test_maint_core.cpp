#include <unity.h>
#include "MaintFsm.h"
#include "ValidityGate.h"

using namespace maint;

void setUp() {}
void tearDown() {}

void test_refusals() {
  MaintFsm f;
  TEST_ASSERT_EQUAL((int)Refusal::NO_PROFILE, (int)f.request(1, false, true, 0));
  TEST_ASSERT_EQUAL((int)Refusal::NO_PROFILE, (int)f.request(3, true, true, 0));
  TEST_ASSERT_EQUAL((int)Refusal::NOT_SAFE, (int)f.request(1, true, false, 0));
  TEST_ASSERT_EQUAL((int)State::MESH, (int)f.state());
  TEST_ASSERT_EQUAL((int)Refusal::NONE, (int)f.request(2, true, true, 0));
  TEST_ASSERT_EQUAL((int)Refusal::BUSY, (int)f.request(1, true, true, 0));
  TEST_ASSERT_EQUAL(2, f.profile());
}

void test_join_then_serve_then_done() {
  MaintFsm f;
  f.request(1, true, true, 1000);
  TEST_ASSERT_EQUAL((int)Action::LEAVE_MESH_AND_JOIN, (int)f.poll(1000, false));
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(5000, false));
  TEST_ASSERT_EQUAL((int)Action::START_SERVER, (int)f.poll(6000, true));
  TEST_ASSERT_EQUAL((int)State::SERVING, (int)f.state());
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(600000, true));   // no idle timeout
  f.done();
  TEST_ASSERT_EQUAL((int)Action::RETURN_TO_MESH, (int)f.poll(600010, true));
  TEST_ASSERT_EQUAL((int)State::MESH, (int)f.state());
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(600020, true));
}

void test_join_timeout_returns_to_mesh() {
  MaintFsm f;
  f.request(1, true, true, 0);
  f.poll(100, false);                                   // leave + join starts at 100
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(20099, false));
  TEST_ASSERT_EQUAL((int)Action::RETURN_TO_MESH, (int)f.poll(20100, false));
  TEST_ASSERT_EQUAL((int)State::MESH, (int)f.state());
}

void test_link_lost_while_serving_returns_after_20s() {
  MaintFsm f;
  f.request(1, true, true, 0);
  f.poll(0, false);
  f.poll(1000, true);                                   // serving
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(2000, false));     // link drops at 2000
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(10000, true));     // back up: timer cleared
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(11000, false));    // drops again at 11000
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(30999, false));
  TEST_ASSERT_EQUAL((int)Action::RETURN_TO_MESH, (int)f.poll(31000, false));
}

void test_stale_done_does_not_leak_into_next_session() {
  MaintFsm f;
  f.done();                                             // e.g. a late HTTP request
  f.poll(0, false);
  f.request(1, true, true, 0);
  f.poll(0, false);
  TEST_ASSERT_EQUAL((int)Action::START_SERVER, (int)f.poll(10, true));
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(20, true));
}

void test_timeouts_survive_millis_rollover() {
  MaintFsm f;
  f.request(1, true, true, 0xFFFFF000u);
  f.poll(0xFFFFF000u, false);
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(0x00001000u, false));   // 8 s later
  TEST_ASSERT_EQUAL((int)Action::RETURN_TO_MESH, (int)f.poll(0xFFFFF000u + 20000u, false));
}

void test_gate_inactive_when_not_pending() {
  ValidityGate g;
  g.begin(false, true, false, 0);
  TEST_ASSERT_EQUAL((int)Verdict::NONE, (int)g.poll(100000));
}

void test_gate_valid_when_mesh_up() {
  ValidityGate g;
  g.begin(true, true, false, 0);
  TEST_ASSERT_EQUAL((int)Verdict::NONE, (int)g.poll(1000));
  g.meshUp();
  TEST_ASSERT_EQUAL((int)Verdict::VALID, (int)g.poll(2000));
  TEST_ASSERT_EQUAL((int)Verdict::NONE, (int)g.poll(3000));      // decided once
}

void test_gate_invalid_after_30s() {
  ValidityGate g;
  g.begin(true, true, false, 0);
  TEST_ASSERT_EQUAL((int)Verdict::NONE, (int)g.poll(29999));
  TEST_ASSERT_EQUAL((int)Verdict::INVALID, (int)g.poll(30000));
  TEST_ASSERT_EQUAL((int)Verdict::NONE, (int)g.poll(40000));
}

void test_gate_needs_mcu_on_fan() {
  ValidityGate g;
  g.begin(true, true, true, 0);
  g.meshUp();
  TEST_ASSERT_EQUAL((int)Verdict::NONE, (int)g.poll(1000));
  g.mcuOk();
  TEST_ASSERT_EQUAL((int)Verdict::VALID, (int)g.poll(1001));
}

void test_gate_mesh_not_required_when_unconfigured() {
  ValidityGate g;
  g.begin(true, false, false, 0);
  TEST_ASSERT_EQUAL((int)Verdict::VALID, (int)g.poll(1));
}

// Spec 2026-10-07 D8: while an upload session runs, a lost link never sends
// the node back to the mesh; the 20 s count starts when the hold ends.
void test_hold_keeps_serving_through_link_loss() {
  MaintFsm f;
  f.request(1, true, true, 0);
  f.poll(0, false);
  TEST_ASSERT_EQUAL((int)Action::START_SERVER, (int)f.poll(1000, true));
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(2000, false, true));
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(120000, false, true));   // 2 min down, held
  TEST_ASSERT_EQUAL((int)State::SERVING, (int)f.state());
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(130000, false, false));  // hold over: count starts
  TEST_ASSERT_EQUAL((int)Action::NONE, (int)f.poll(149999, false, false));
  TEST_ASSERT_EQUAL((int)Action::RETURN_TO_MESH, (int)f.poll(150000, false, false));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_refusals);
  RUN_TEST(test_join_then_serve_then_done);
  RUN_TEST(test_join_timeout_returns_to_mesh);
  RUN_TEST(test_link_lost_while_serving_returns_after_20s);
  RUN_TEST(test_stale_done_does_not_leak_into_next_session);
  RUN_TEST(test_timeouts_survive_millis_rollover);
  RUN_TEST(test_gate_inactive_when_not_pending);
  RUN_TEST(test_gate_valid_when_mesh_up);
  RUN_TEST(test_gate_invalid_after_30s);
  RUN_TEST(test_gate_needs_mcu_on_fan);
  RUN_TEST(test_gate_mesh_not_required_when_unconfigured);
  RUN_TEST(test_hold_keeps_serving_through_link_loss);
  return UNITY_END();
}
