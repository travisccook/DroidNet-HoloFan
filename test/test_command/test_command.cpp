#include <unity.h>
#include "command.h"

using namespace holofan;

void setUp() {}
void tearDown() {}

static void expect(const char* line, CmdType t, int p) {
  Cmd c;
  TEST_ASSERT_TRUE(parseCommand(line, c));
  TEST_ASSERT_EQUAL((int)t, (int)c.type);
  TEST_ASSERT_EQUAL(p, c.param);
}

void test_power()      { expect("#HLF1", CmdType::POWER_ON, 0); expect("#HLF0", CmdType::POWER_OFF, 0); }
void test_play_index() { expect("#HLFG3", CmdType::PLAY_INDEX, 3); expect("#HLFG12", CmdType::PLAY_INDEX, 12); }
void test_transport()  { expect("#HLFP", CmdType::PAUSE, 0); expect("#HLFN", CmdType::NEXT, 0); expect("#HLFR", CmdType::PREV, 0); }
void test_levels()     { expect("#HLFB2", CmdType::BRIGHTNESS, 2); expect("#HLFV3", CmdType::VOLUME, 3); }
void test_mute()       { expect("#HLFM", CmdType::MUTE, 0); expect("#HLFU", CmdType::UNMUTE, 0); }
void test_loop_sw()    { expect("#HLFLA", CmdType::LOOP, 1); expect("#HLFLO", CmdType::LOOP, 0);
                         expect("#HLFW1", CmdType::SWITCHER, 1); expect("#HLFW0", CmdType::SWITCHER, 0); }
void test_status()     { expect("#HLFS", CmdType::STATUS, 0); expect("#HLFD", CmdType::CATALOG_DUMP, 0); expect("#HLFC", CmdType::CATALOG_REFRESH, 0); }

void test_diag_query() { Cmd c; TEST_ASSERT_TRUE(parseCommand("#HLFQ", c)); TEST_ASSERT_EQUAL((int)CmdType::DIAG, (int)c.type); }
void test_bad_level_fails()    { Cmd c; TEST_ASSERT_FALSE(parseCommand("#HLFB9", c)); TEST_ASSERT_FALSE(parseCommand("#HLFV0", c)); }
void test_bad_index_fails()    { Cmd c; TEST_ASSERT_FALSE(parseCommand("#HLFG0", c)); }
// #HLFY<r><seconds> (bench, 0.9.4): volume 3 past the cap for 10-300 s with the
// radio on (r=1) or off (r=0); param = r * 1000 + seconds.
void test_loud_test() {
  expect("#HLFY060", CmdType::LOUD_TEST, 60);
  expect("#HLFY1120", CmdType::LOUD_TEST, 1120);
  expect("#HLFY010", CmdType::LOUD_TEST, 10);
  expect("#HLFY1300", CmdType::LOUD_TEST, 1300);
  expect("#HLFY2300", CmdType::LOUD_TEST, 2300);     // r=2 (0.9.6): receiver on, no transmit
}
void test_bad_loud_test_fails() {
  Cmd c;
  TEST_ASSERT_FALSE(parseCommand("#HLFY", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFY0", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFY09", c));      // under 10 s
  TEST_ASSERT_FALSE(parseCommand("#HLFY0301", c));    // over 300 s
  TEST_ASSERT_FALSE(parseCommand("#HLFY360", c));     // r is 0, 1 or 2
  TEST_ASSERT_FALSE(parseCommand("#HLFY060x", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFY01000", c));
}
// #HLFE<lo><hi><seconds> (bench, 0.9.6): sag meter over brownout levels
// lo..hi (3-7 since 0.9.7) for 10-300 s; param = lo * 10000 + hi * 1000 + seconds.
void test_sag_meter() {
  expect("#HLFE36300", CmdType::SAG_METER, 36300);
  expect("#HLFE7710", CmdType::SAG_METER, 77010);
  expect("#HLFE3760", CmdType::SAG_METER, 37060);
}
// #HLFJ<lo><hi><dBm> (bench, 0.9.7): TX dip probe over levels lo..hi (3-7) at
// TX power 2-20 dBm (two digits); param = lo * 10000 + hi * 1000 + dBm.
void test_tx_probe() {
  expect("#HLFJ3720", CmdType::TX_PROBE, 37020);
  expect("#HLFJ5502", CmdType::TX_PROBE, 55002);
  expect("#HLFJ4611", CmdType::TX_PROBE, 46011);
}
void test_bad_tx_probe_fails() {
  Cmd c;
  TEST_ASSERT_FALSE(parseCommand("#HLFJ", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFJ37", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFJ372", c));       // two digits of dBm
  TEST_ASSERT_FALSE(parseCommand("#HLFJ3701", c));      // under 2 dBm
  TEST_ASSERT_FALSE(parseCommand("#HLFJ3721", c));      // over 20 dBm
  TEST_ASSERT_FALSE(parseCommand("#HLFJ2720", c));      // no level 2 (3.27 V: next to the supply)
  TEST_ASSERT_FALSE(parseCommand("#HLFJ7320", c));      // lo above hi
  TEST_ASSERT_FALSE(parseCommand("#HLFJ37200", c));
}
void test_bad_sag_meter_fails() {
  Cmd c;
  TEST_ASSERT_FALSE(parseCommand("#HLFE", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFE3", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFE36", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFE3609", c));     // under 10 s
  TEST_ASSERT_FALSE(parseCommand("#HLFE36301", c));    // over 300 s
  TEST_ASSERT_FALSE(parseCommand("#HLFE16060", c));    // no level 1
  TEST_ASSERT_FALSE(parseCommand("#HLFE27060", c));    // no level 2 (0.9.7)
  TEST_ASSERT_FALSE(parseCommand("#HLFE38060", c));    // no level 8
  TEST_ASSERT_FALSE(parseCommand("#HLFE63060", c));    // lo above hi
  TEST_ASSERT_FALSE(parseCommand("#HLFE3660x", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFE361000", c));
}
// #HLFK<ss> (bench, 0.9.8): mesh heartbeat period ss seconds (02-50), 00 = the default.
void test_heartbeat_knob() {
  expect("#HLFK07", CmdType::HEARTBEAT, 7);
  expect("#HLFK25", CmdType::HEARTBEAT, 25);
  expect("#HLFK00", CmdType::HEARTBEAT, 0);
}
void test_bad_heartbeat_knob_fails() {
  Cmd c;
  TEST_ASSERT_FALSE(parseCommand("#HLFK", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFK7", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFK01", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFK26", c));      // review: one lost beat must fit a WCB's offline window
  TEST_ASSERT_FALSE(parseCommand("#HLFK070", c));
}
// #HLFA<0|1> (0.9.9, Bluetooth is not needed on the mesh): the MCU's Bluetooth off/on, saved.
void test_bluetooth() {
  expect("#HLFA0", CmdType::BLUETOOTH, 0);
  expect("#HLFA1", CmdType::BLUETOOTH, 1);
}
void test_bad_bluetooth_fails() {
  Cmd c;
  TEST_ASSERT_FALSE(parseCommand("#HLFA", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFA2", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFA10", c));
}
// #HLFO<dd> (0.9.10): mesh TX power dd dBm (02-20, two digits), 00 = the default; saved.
void test_tx_power() {
  expect("#HLFO02", CmdType::TX_POWER, 2);
  expect("#HLFO08", CmdType::TX_POWER, 8);
  expect("#HLFO20", CmdType::TX_POWER, 20);
  expect("#HLFO00", CmdType::TX_POWER, 0);
}
void test_bad_tx_power_fails() {
  Cmd c;
  TEST_ASSERT_FALSE(parseCommand("#HLFO", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFO8", c));        // two digits, as #HLFK
  TEST_ASSERT_FALSE(parseCommand("#HLFO01", c));       // the radio's floor is 2 dBm
  TEST_ASSERT_FALSE(parseCommand("#HLFO21", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFO080", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFO-8", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFO1x", c));
}
// #HLFH<2|3>: the highest volume the fan allows (0.9.11; saved).
void test_volume_cap() {
  expect("#HLFH2", CmdType::VOLUME_CAP, 2);
  expect("#HLFH3", CmdType::VOLUME_CAP, 3);
}
void test_bad_volume_cap_fails() {
  Cmd c;
  TEST_ASSERT_FALSE(parseCommand("#HLFH", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFH0", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFH1", c));        // 1 is a volume, not a cap worth saving
  TEST_ASSERT_FALSE(parseCommand("#HLFH4", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFH23", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFH3x", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFHx", c));
}
// #HLFT<NN>: play the clip whose name starts with NN- or NN_ (one or two digits, 0-99).
void test_play_slot() {
  expect("#HLFT03", CmdType::PLAY_SLOT, 3);
  expect("#HLFT7", CmdType::PLAY_SLOT, 7);
  expect("#HLFT00", CmdType::PLAY_SLOT, 0);
  expect("#HLFT99", CmdType::PLAY_SLOT, 99);
}
void test_bad_slot_fails() {
  Cmd c;
  TEST_ASSERT_FALSE(parseCommand("#HLFT", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFT100", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFTx", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFT1a", c));
  TEST_ASSERT_FALSE(parseCommand("#HLFT-1", c));
}
void test_bad_loop_fails()     { Cmd c; TEST_ASSERT_FALSE(parseCommand("#HLFLX", c)); }
void test_wrong_prefix_fails() { Cmd c; TEST_ASSERT_FALSE(parseCommand("#HLU", c)); TEST_ASSERT_FALSE(parseCommand("garbage", c)); TEST_ASSERT_FALSE(parseCommand("#HLF", c)); }

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_diag_query);
  RUN_TEST(test_power);
  RUN_TEST(test_play_index);
  RUN_TEST(test_transport);
  RUN_TEST(test_levels);
  RUN_TEST(test_mute);
  RUN_TEST(test_loop_sw);
  RUN_TEST(test_status);
  RUN_TEST(test_bad_level_fails);
  RUN_TEST(test_bad_index_fails);
  RUN_TEST(test_play_slot);
  RUN_TEST(test_loud_test);
  RUN_TEST(test_bad_loud_test_fails);
  RUN_TEST(test_sag_meter);
  RUN_TEST(test_bad_sag_meter_fails);
  RUN_TEST(test_tx_probe);
  RUN_TEST(test_bad_tx_probe_fails);
  RUN_TEST(test_heartbeat_knob);
  RUN_TEST(test_bluetooth);
  RUN_TEST(test_bad_bluetooth_fails);
  RUN_TEST(test_bad_heartbeat_knob_fails);
  RUN_TEST(test_tx_power);
  RUN_TEST(test_bad_tx_power_fails);
  RUN_TEST(test_volume_cap);
  RUN_TEST(test_bad_volume_cap_fails);
  RUN_TEST(test_bad_slot_fails);
  RUN_TEST(test_bad_loop_fails);
  RUN_TEST(test_wrong_prefix_fails);
  return UNITY_END();
}
