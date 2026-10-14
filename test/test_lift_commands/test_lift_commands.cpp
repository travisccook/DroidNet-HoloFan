#include <unity.h>
#include <string.h>
#include "LiftCommands.h"

using namespace lift;

void setUp() {}
void tearDown() {}

static void expect(const char* line, Cmd c, int32_t v = 0) {
  LiftCmd out;
  TEST_ASSERT_TRUE_MESSAGE(parseLift(line, out), line);
  TEST_ASSERT_EQUAL_MESSAGE((int)c, (int)out.cmd, line);
  TEST_ASSERT_EQUAL_MESSAGE(v, out.value, line);
}
static void reject(const char* line) {
  LiftCmd out;
  TEST_ASSERT_FALSE_MESSAGE(parseLift(line, out), line);
}

void test_motion_commands() {
  expect("#HLU", Cmd::UP); expect("#HLD", Cmd::DOWN); expect("#HLS", Cmd::STOP);
  expect("#HLH", Cmd::HOME); expect("#HLX", Cmd::CLEAR);
  expect("#HLG5000", Cmd::GOTO, 5000); expect("#HLG0", Cmd::GOTO, 0); expect("#HLG-120", Cmd::GOTO, -120);
}

void test_config_commands() {
  expect("#HLV128", Cmd::SPEED, 128); expect("#HLV0", Cmd::SPEED, 0); expect("#HLV255", Cmd::SPEED, 255);
  expect("#HLM20", Cmd::MARGIN, 20);
  expect("#HLON", Cmd::MOTOR_ON); expect("#HLOF", Cmd::MOTOR_OFF);
  expect("#HLSAVE", Cmd::SAVE); expect("#HLLOAD", Cmd::LOAD); expect("#HLCFGRST", Cmd::CFG_RESET);
  expect("#HLR", Cmd::REBOOT);
  expect("#HLF1", Cmd::FAN_ON); expect("#HLF0", Cmd::FAN_OFF);
}

void test_queries() {
  expect("#HLQ", Cmd::Q_STATUS); expect("#HLP", Cmd::Q_POS); expect("#HLL", Cmd::Q_LIMITS);
  expect("#HLE", Cmd::Q_ENC); expect("#HLI", Cmd::Q_INFO); expect("#HL?", Cmd::Q_HELP);
  TEST_ASSERT_TRUE(isQuery(Cmd::Q_STATUS));
  TEST_ASSERT_FALSE(isQuery(Cmd::UP));
}

// #HLSAVE must not be read as #HLS (stop) plus junk, and so on.
void test_longest_words_win() {
  expect("#HLSAVE", Cmd::SAVE);
  expect("#HLS", Cmd::STOP);
  reject("#HLSA");
  reject("#HLSTOP");
}

void test_rejects_bad_input() {
  reject(""); reject("#HL"); reject("#hlu"); reject("HLU"); reject("#HLU1"); reject("#HLUX");
  reject("#HLG"); reject("#HLG12a"); reject("#HLG1.5"); reject("#HLG-"); reject("#HLG99999999999");
  reject("#HLV256"); reject("#HLV-1"); reject("#HLV"); reject("#HLM-5");
  reject("#HLC");                                   // documented calibrate: not implemented
  reject("#HLFG1"); reject("#HLFP"); reject("#HLF2"); // playback is the fan's
  reject("#HLZQ");                                  // node family, parsed elsewhere
}

void test_normalize_strips_soh_and_terminators() {
  char a[] = "\x01#HLU\r";
  TEST_ASSERT_EQUAL_STRING("#HLU", normalizeLine(a));
  char b[] = "#HLG100\r\n";
  TEST_ASSERT_EQUAL_STRING("#HLG100", normalizeLine(b));
  char c[] = "";
  TEST_ASSERT_EQUAL_STRING("", normalizeLine(c));
}

void test_is_motion_start() {
  TEST_ASSERT_TRUE(isMotionStart(Cmd::UP));
  TEST_ASSERT_TRUE(isMotionStart(Cmd::DOWN));
  TEST_ASSERT_TRUE(isMotionStart(Cmd::HOME));
  TEST_ASSERT_TRUE(isMotionStart(Cmd::GOTO));
  TEST_ASSERT_FALSE(isMotionStart(Cmd::STOP));
  TEST_ASSERT_FALSE(isMotionStart(Cmd::CLEAR));
  TEST_ASSERT_FALSE(isMotionStart(Cmd::SPEED));
  TEST_ASSERT_FALSE(isMotionStart(Cmd::Q_STATUS));
  TEST_ASSERT_FALSE(isMotionStart(Cmd::FAN_ON));
  TEST_ASSERT_FALSE(isMotionStart(Cmd::MOTOR_ON));
}

void test_is_stop_line() {
  TEST_ASSERT_TRUE(isStopLine("#HLS"));
  TEST_ASSERT_TRUE(isStopLine("#HLS\r"));
  TEST_ASSERT_TRUE(isStopLine("\x01#HLS\r\n"));
  TEST_ASSERT_FALSE(isStopLine("#HLSAVE"));
  TEST_ASSERT_FALSE(isStopLine("#HLU"));
  TEST_ASSERT_FALSE(isStopLine("#HL"));
}

static Status sample() {
  Status s;
  s.state = State::MOVING; s.pos = 4520; s.target = 5000; s.homed = true; s.speed = 128;
  s.upper = false; s.lower = true; s.fan = true; s.enc = 4521; s.fw = "2.0.0"; s.slot = "app0";
  return s;
}

void test_status_reply() {
  char out[200];
  int n = formatReply(Cmd::Q_STATUS, sample(), out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("#HL:STATUS state=MOVING pos=4520 target=5000 homed=1 speed=128 limits=0,1 fan=1", out);
  TEST_ASSERT_EQUAL((int)strlen(out), n);
}

void test_status_reply_with_error_is_one_token() {
  Status s = sample();
  s.state = State::ERROR;
  s.error = "Stall detected, motor^stopped";
  char out[200];
  formatReply(Cmd::Q_STATUS, s, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("#HL:STATUS state=ERROR pos=4520 target=5000 homed=1 speed=128 limits=0,1 fan=1 error=Stall_detected_motorstopped", out);
}

void test_value_replies() {
  char out[200];
  formatReply(Cmd::Q_POS, sample(), out, sizeof(out));    TEST_ASSERT_EQUAL_STRING("#HL:POS 4520", out);
  formatReply(Cmd::Q_LIMITS, sample(), out, sizeof(out)); TEST_ASSERT_EQUAL_STRING("#HL:LIMITS upper=0 lower=1", out);
  formatReply(Cmd::Q_ENC, sample(), out, sizeof(out));    TEST_ASSERT_EQUAL_STRING("#HL:ENC 4521", out);
  formatReply(Cmd::Q_INFO, sample(), out, sizeof(out));   TEST_ASSERT_EQUAL_STRING("#HL:INFO ver=2.0.0 board=ESP32ThingPlus slot=app0", out);
  TEST_ASSERT_TRUE(formatReply(Cmd::Q_HELP, sample(), out, sizeof(out)) > 0);
}

// Replies travel as ";S<port>,<reply>" inside one 187-char WCB command.
void test_every_reply_fits_one_mesh_command() {
  Status s = sample();
  s.state = State::ERROR;
  s.pos = -10000000; s.target = -10000000; s.enc = -9999999999LL;
  s.error = "an extremely long error message that goes on and on and on and on and on and on";
  s.fw = "10.20.30-rc1"; s.slot = "app1";
  const Cmd qs[] = {Cmd::Q_STATUS, Cmd::Q_POS, Cmd::Q_LIMITS, Cmd::Q_ENC, Cmd::Q_INFO, Cmd::Q_HELP};
  for (Cmd q : qs) {
    char out[200];
    int n = formatReply(q, s, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE_MESSAGE(n + 4 <= 187, out);    // ";S5," prefix
    TEST_ASSERT_NULL(strchr(out, '^'));
    TEST_ASSERT_NOT_EQUAL('{', out[0]);
  }
}

void test_non_query_and_small_buffer_fail() {
  char out[8];
  TEST_ASSERT_EQUAL(-1, formatReply(Cmd::UP, sample(), out, sizeof(out)));
  TEST_ASSERT_EQUAL(-1, formatReply(Cmd::Q_STATUS, sample(), out, sizeof(out)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_motion_commands);
  RUN_TEST(test_config_commands);
  RUN_TEST(test_queries);
  RUN_TEST(test_longest_words_win);
  RUN_TEST(test_rejects_bad_input);
  RUN_TEST(test_normalize_strips_soh_and_terminators);
  RUN_TEST(test_is_stop_line);
  RUN_TEST(test_is_motion_start);
  RUN_TEST(test_status_reply);
  RUN_TEST(test_status_reply_with_error_is_one_token);
  RUN_TEST(test_value_replies);
  RUN_TEST(test_every_reply_fits_one_mesh_command);
  RUN_TEST(test_non_query_and_small_buffer_fail);
  return UNITY_END();
}
