#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "FanController.h"
#include "ClipOps.h"

using namespace fanlogic;
using holofan::Cmd;
using holofan::CmdType;
using holofan::FanState;
static const char* const BLACK_CLIP_NAME = fanlogic::BLACK_CLIP;

// A scripted MCU: answers get_setting / get_playinfo / s from its fields,
// records every line written (send and transact alike).
class FakeMcu : public McuLink {
 public:
  bool alive = true;
  std::string setting = "setting on all 2 3 164 F-S12\n";
  std::string playinfo = "PlayInfo \"02-Name.bin\" playing 8 24\n";
  std::string list = "02-Name.bin\n03-Name.bin\n04-Name.bin\n05-Name.bin\n";
  bool sendOk = true;
  int wakes = 0;
  std::vector<std::string> lines;
  bool applyVolume = false;   // "m <v>" rewrites `setting` with volume v ...
  int dropVolume = 0;         // ... except the first dropVolume of them (MCU busy)
  int silentSettings = 0;     // the next silentSettings get_setting go unanswered (MCU busy)
  int silentAfterM = 0;       // after each "m", this many get_setting go unanswered
  bool applyBle = true;       // "d" / "c" set the ble field of `setting` off / on (false: the MCU ignores them)
  int dropBle = 0;            // ... except the first dropBle of them (MCU busy)

  void wake() override { wakes++; }
  bool send(const char* line) override {
    lines.push_back(line);
    if (strncmp(line, "m ", 2) == 0 && silentAfterM > 0) silentSettings = silentAfterM;
    bool bleLine = strcmp(line, "d\n") == 0 || strcmp(line, "c\n") == 0;
    if (bleLine && dropBle > 0) { dropBle--; bleLine = false; }
    if (applyBle && bleLine && setting.rfind("setting ", 0) == 0) {
      size_t sp = setting.find(' ', 8);
      if (sp != std::string::npos) setting = std::string("setting ") + (line[0] == 'c' ? "on" : "off") + setting.substr(sp);
    }
    if (applyVolume && strncmp(line, "m ", 2) == 0) {
      if (dropVolume > 0) dropVolume--;
      else setting = std::string("setting on all 2 ") + line[2] + " 164 F-S12\n";
    }
    return sendOk;
  }
  int transact(const char* line, const char* needle, uint32_t, char* reply, size_t size) override {
    lines.push_back(line);
    std::string r;
    if (alive) {
      if (strcmp(line, "get_setting\n") == 0) {
        if (silentSettings > 0) silentSettings--;
        else r = setting;
      }
      else if (strcmp(line, "get_playinfo\n") == 0) r = playinfo;
      else if (strcmp(line, "s\n") == 0) r = list;
    }
    (void)needle;
    snprintf(reply, size, "%s", r.c_str());
    return (int)strlen(reply);
  }
  int expect(const char* line, const char* const*, int, uint32_t) override {
    if (line) lines.push_back(line);
    return -1;
  }
  std::vector<size_t> settles;   // lines.size() at each read-only settle (collect with no buffer)
  int collect(char* buf, size_t, uint32_t, uint32_t) override {
    if (!buf) settles.push_back(lines.size());
    return 0;
  }
  // The MCU was left to settle right after line `i` (0-based) and before the next one.
  bool settledAfter(size_t i) const {
    for (size_t at : settles) if (at == i + 1) return true;
    return false;
  }
  size_t indexOf(const char* line, int nth = 0) const {
    for (size_t i = 0; i < lines.size(); i++) if (lines[i] == line && nth-- == 0) return i;
    return (size_t)-1;
  }
  int count(const char* line) const {
    int n = 0;
    for (const auto& l : lines) if (l == line) n++;
    return n;
  }
  std::string last() const { return lines.empty() ? "" : lines.back(); }
};

static constexpr int CATALOG_MAX_TEST = holofan::CATALOG_MAX;
static FakeMcu* mcu;
static FanController* fan;

void setUp() {
  mcu = new FakeMcu();
  fan = new FanController(*mcu);
}
void tearDown() {
  delete fan;
  delete mcu;
}

static void run(CmdType t, int p = 0, uint32_t now = 1000) { fan->execute(Cmd(t, p), now); }

void test_begin_ready_loads_settings_catalog_and_play() {
  mcu->applyVolume = true;
  fan->begin(0);
  const auto& s = fan->status();
  TEST_ASSERT_EQUAL(1, mcu->wakes);
  TEST_ASSERT_EQUAL((int)FanState::READY, (int)s.state);
  TEST_ASSERT_TRUE(fan->mcuAnswered());
  TEST_ASSERT_EQUAL(2, s.brightness);
  TEST_ASSERT_EQUAL(VOLUME_MAX, s.volume);   // the stored 3 is lowered at connect
  TEST_ASSERT_TRUE(s.loopAll);
  TEST_ASSERT_EQUAL(4, fan->catalog().count);
  TEST_ASSERT_EQUAL_STRING("02-Name.bin", s.fileName);
  TEST_ASSERT_EQUAL(1, s.fileIndex);
  TEST_ASSERT_EQUAL((int)holofan::Play::PLAYING, (int)s.play);
  TEST_ASSERT_EQUAL(8, s.progress);
  TEST_ASSERT_EQUAL(24, s.total);
  TEST_ASSERT_EQUAL(-1, s.switcher);
  TEST_ASSERT_EQUAL_STRING("none", s.err);
}

// A silent MCU must not hang or crash: 21 tries, then NOMCU.
void test_begin_silent_mcu_is_nomcu() {
  mcu->alive = false;
  fan->begin(0);
  TEST_ASSERT_EQUAL((int)FanState::NOMCU, (int)fan->status().state);
  TEST_ASSERT_FALSE(fan->mcuAnswered());
  TEST_ASSERT_EQUAL(SETTING_TRIES, mcu->count("get_setting\n"));
  TEST_ASSERT_EQUAL_STRING("nomcu", fan->status().err);
}

void test_nomcu_drops_actions() {
  mcu->alive = false;
  fan->begin(0);
  size_t before = mcu->lines.size();
  run(CmdType::PAUSE);
  run(CmdType::PLAY_INDEX, 1);
  TEST_ASSERT_EQUAL(before, mcu->lines.size());
  TEST_ASSERT_EQUAL_STRING("nomcu", fan->status().err);
}

// The MCU powers up later than us (or comes back): one retry every 10 s.
void test_nomcu_recovers_on_retry() {
  mcu->alive = false;
  fan->begin(0);
  int tries = mcu->count("get_setting\n");
  fan->poll(NOMCU_RETRY_MS - 1);
  TEST_ASSERT_EQUAL(tries, mcu->count("get_setting\n"));
  fan->poll(NOMCU_RETRY_MS);
  TEST_ASSERT_EQUAL(tries + 1, mcu->count("get_setting\n"));
  mcu->alive = true;
  fan->poll(2 * NOMCU_RETRY_MS);
  TEST_ASSERT_EQUAL((int)FanState::READY, (int)fan->status().state);
  TEST_ASSERT_EQUAL(4, fan->catalog().count);
  TEST_ASSERT_EQUAL_STRING("none", fan->status().err);
}

void test_poll_cadence_and_update() {
  fan->begin(0);
  int polls = mcu->count("get_playinfo\n");
  fan->poll(POLL_MS - 1);
  TEST_ASSERT_EQUAL(polls, mcu->count("get_playinfo\n"));
  mcu->playinfo = "PlayInfo \"04-Name.bin\" pause 19 24\n";
  fan->poll(POLL_MS);
  TEST_ASSERT_EQUAL(polls + 1, mcu->count("get_playinfo\n"));
  TEST_ASSERT_EQUAL(3, fan->status().fileIndex);
  TEST_ASSERT_EQUAL((int)holofan::Play::IDLE, (int)fan->status().play);
  TEST_ASSERT_EQUAL(19, fan->status().progress);
}

void test_ready_goes_nomcu_after_silent_polls() {
  fan->begin(0);
  mcu->alive = false;
  uint32_t t = 0;
  for (int i = 0; i < POLL_MISSES_NOMCU - 1; i++) { t += POLL_MS; fan->poll(t); }
  TEST_ASSERT_EQUAL((int)FanState::READY, (int)fan->status().state);
  t += POLL_MS;
  fan->poll(t);
  TEST_ASSERT_EQUAL((int)FanState::NOMCU, (int)fan->status().state);
  TEST_ASSERT_EQUAL((int)holofan::Play::UNKNOWN, (int)fan->status().play);
}

// Only "t <name>": on the real fan a "t" sent straight after "a" (switcher on)
// is dropped by the MCU (bench 2026-10-06), so #HLFG does not touch the switcher.
void test_play_index_sends_only_the_name() {
  fan->begin(0);
  size_t before = mcu->lines.size();
  run(CmdType::PLAY_INDEX, 3);
  TEST_ASSERT_EQUAL(before + 1, mcu->lines.size());
  TEST_ASSERT_EQUAL_STRING("t 04-Name.bin\n", mcu->last().c_str());
  TEST_ASSERT_EQUAL(-1, fan->status().switcher);
  TEST_ASSERT_EQUAL_STRING("none", fan->status().err);
}

void test_play_index_out_of_range() {
  fan->begin(0);
  size_t before = mcu->lines.size();
  run(CmdType::PLAY_INDEX, 5);
  run(CmdType::PLAY_INDEX, 0);
  TEST_ASSERT_EQUAL(before, mcu->lines.size());
  TEST_ASSERT_EQUAL_STRING("badindex", fan->status().err);
}

// A playback command makes the next poll come soon, so #HLFS catches up.
void test_playback_schedules_quick_poll() {
  fan->begin(0);
  int polls = mcu->count("get_playinfo\n");
  run(CmdType::NEXT, 0, 1000);
  fan->poll(1000 + QUICK_POLL_MS);
  TEST_ASSERT_EQUAL(polls + 1, mcu->count("get_playinfo\n"));
}

void test_simple_actions_map_to_mcu_letters() {
  fan->begin(0);
  run(CmdType::PAUSE);        TEST_ASSERT_EQUAL_STRING("h\n", mcu->last().c_str());
  run(CmdType::NEXT);         TEST_ASSERT_EQUAL_STRING("f\n", mcu->last().c_str());
  run(CmdType::PREV);         TEST_ASSERT_EQUAL_STRING("e\n", mcu->last().c_str());
  run(CmdType::LOOP, 0);      TEST_ASSERT_EQUAL_STRING("i\n", mcu->last().c_str());
  TEST_ASSERT_FALSE(fan->status().loopAll);
  run(CmdType::LOOP, 1);      TEST_ASSERT_EQUAL_STRING("j\n", mcu->last().c_str());
  TEST_ASSERT_TRUE(fan->status().loopAll);
  run(CmdType::SWITCHER, 0);  TEST_ASSERT_EQUAL_STRING("b\n", mcu->last().c_str());
  TEST_ASSERT_EQUAL(0, fan->status().switcher);
  run(CmdType::SWITCHER, 1);  TEST_ASSERT_EQUAL_STRING("a\n", mcu->last().c_str());
  run(CmdType::BRIGHTNESS, 1); TEST_ASSERT_EQUAL_STRING("k 1\n", mcu->last().c_str());
  TEST_ASSERT_EQUAL(1, fan->status().brightness);
  run(CmdType::VOLUME, 2);    TEST_ASSERT_EQUAL_STRING("m 2\n", mcu->last().c_str());
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

// Out-of-range values are ignored, even if a caller skips parseCommand.
void test_levels_out_of_range_ignored() {
  fan->begin(0);
  size_t before = mcu->lines.size();
  run(CmdType::BRIGHTNESS, 4);
  run(CmdType::VOLUME, 0);
  TEST_ASSERT_EQUAL(before, mcu->lines.size());
  TEST_ASSERT_EQUAL(2, fan->status().brightness);
}

void test_mute_and_unmute_restore_volume() {
  fan->begin(0);              // volume 3 from get_setting
  run(CmdType::VOLUME, 2);
  run(CmdType::MUTE);         TEST_ASSERT_EQUAL_STRING("m 1\n", mcu->last().c_str());
  TEST_ASSERT_EQUAL(1, fan->status().volume);
  size_t n = mcu->lines.size();
  run(CmdType::MUTE);         // already muted: nothing sent, the saved volume is kept
  TEST_ASSERT_EQUAL(n, mcu->lines.size());
  run(CmdType::UNMUTE);       TEST_ASSERT_EQUAL_STRING("m 2\n", mcu->last().c_str());
  TEST_ASSERT_EQUAL(2, fan->status().volume);
  n = mcu->lines.size();
  run(CmdType::UNMUTE);       // not muted, volume 2: nothing to undo, nothing sent
  TEST_ASSERT_EQUAL(n, mcu->lines.size());
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

// Volume 1 with no mute of ours (set by the app or #HLFV1): UNMUTE raises it
// to the cap (2), not 3.
void test_unmute_at_floor_volume_sets_max() {
  fan->begin(0);
  run(CmdType::VOLUME, 1);
  run(CmdType::UNMUTE);
  TEST_ASSERT_EQUAL_STRING("m 2\n", mcu->last().c_str());
  TEST_ASSERT_EQUAL(VOLUME_MAX, fan->status().volume);
}

// Volume 3 browns out the fan's ESP and cuts its audio (bench 2026-10-07):
// a volume-3 request sends 2.
void test_volume_3_is_capped_to_2() {
  fan->begin(0);
  run(CmdType::VOLUME, 1);
  run(CmdType::VOLUME, 3);
  TEST_ASSERT_EQUAL_STRING("m 2\n", mcu->last().c_str());
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

// 0.9.11 (#HLFH3, a saved choice; volume 3 ran clean at 8 dBm on the bench): with
// the cap at 3 the MCU's own volume 3 is left alone at connect ...
void test_cap_3_keeps_a_stored_volume_3() {
  fan->setVolumeCap(3);
  mcu->applyVolume = true;
  fan->begin(0);                                   // get_setting says volume 3
  for (const auto& l : mcu->lines) TEST_ASSERT_FALSE(l.rfind("m ", 0) == 0);
  TEST_ASSERT_EQUAL(3, fan->status().volume);
  char out[160];
  TEST_ASSERT_TRUE(fan->formatBoot(out, sizeof(out)) > 0);
  TEST_ASSERT_EQUAL_STRING("[fan] MCU at connect: ble on, loop all, br 2, vol 3, angle 164; playing 02-Name.bin 8/24", out);
}

// ... and at the periodic settings read,
void test_cap_3_survives_the_periodic_read() {
  fan->setVolumeCap(3);
  mcu->applyVolume = true;
  fan->begin(0);
  fan->poll(SETTINGS_POLL_MS);
  for (const auto& l : mcu->lines) TEST_ASSERT_FALSE(l.rfind("m ", 0) == 0);
  char out[UNPROMPTED_MAX];
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));
}

// ... and a volume-3 request sends 3.
void test_cap_3_lets_volume_3_through() {
  fan->setVolumeCap(3);
  fan->begin(0);
  run(CmdType::VOLUME, 1);
  run(CmdType::VOLUME, 3);
  TEST_ASSERT_EQUAL_STRING("m 3\n", mcu->last().c_str());
  TEST_ASSERT_EQUAL(3, fan->status().volume);
}

// Volume 1 with no mute of ours: UNMUTE raises it to the cap, 3 here.
void test_cap_3_unmute_at_floor_sets_3() {
  fan->setVolumeCap(3);
  fan->begin(0);
  run(CmdType::VOLUME, 1);
  run(CmdType::UNMUTE);
  TEST_ASSERT_EQUAL_STRING("m 3\n", mcu->last().c_str());
}

// The way back with no firmware update: #HLFH2 lowers a 3 at once.
void test_cap_command_lowers_a_3_at_once() {
  fan->setVolumeCap(3);
  mcu->applyVolume = true;
  fan->begin(0);
  TEST_ASSERT_EQUAL(3, fan->status().volume);
  run(CmdType::VOLUME_CAP, 2);
  TEST_ASSERT_EQUAL(2, fan->volumeCap());
  TEST_ASSERT_EQUAL(1, mcu->count("m 2\n"));
  TEST_ASSERT_EQUAL(2, fan->status().volume);
  run(CmdType::VOLUME, 3);                         // and volume 3 is capped again
  TEST_ASSERT_EQUAL_STRING("m 2\n", mcu->last().c_str());
}

// #HLFH3 only allows 3: it never raises the volume by itself.
void test_cap_command_raises_nothing() {
  mcu->applyVolume = true;
  fan->begin(0);                                   // stored 3: capped to 2
  size_t n = mcu->lines.size();
  run(CmdType::VOLUME_CAP, 3);
  TEST_ASSERT_EQUAL(3, fan->volumeCap());
  TEST_ASSERT_EQUAL(n, mcu->lines.size());
  TEST_ASSERT_EQUAL(2, fan->status().volume);
  run(CmdType::VOLUME, 3);
  TEST_ASSERT_EQUAL_STRING("m 3\n", mcu->last().c_str());
}

// As Bluetooth: the cap is remembered while the MCU does not answer.
void test_cap_command_is_remembered_while_nomcu() {
  mcu->applyVolume = true;
  mcu->alive = false;
  fan->begin(0);
  run(CmdType::VOLUME_CAP, 3);
  mcu->alive = true;
  fan->poll(NOMCU_RETRY_MS);
  TEST_ASSERT_EQUAL((int)FanState::READY, (int)fan->status().state);
  for (const auto& l : mcu->lines) TEST_ASSERT_FALSE(l.rfind("m ", 0) == 0);
  TEST_ASSERT_EQUAL(3, fan->status().volume);
}

// Anything but 3 is the default cap, 2 (an unset or damaged saved value).
void test_bad_cap_is_the_default() {
  TEST_ASSERT_EQUAL(VOLUME_MAX, fan->volumeCap());
  fan->setVolumeCap(3);
  fan->setVolumeCap(7);
  TEST_ASSERT_EQUAL(VOLUME_MAX, fan->volumeCap());
  fan->setVolumeCap(3);
  fan->setVolumeCap(0);
  TEST_ASSERT_EQUAL(VOLUME_MAX, fan->volumeCap());
}

// The MCU keeps its volume through power cycles (and the IR remote can set
// 3): a stored 3 is lowered to 2 when the link comes up.
void test_connect_lowers_stored_volume_3() {
  mcu->applyVolume = true;
  fan->begin(0);                                   // get_setting says volume 3
  TEST_ASSERT_EQUAL(1, mcu->count("m 2\n"));
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

void test_connect_keeps_stored_volume_2() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  for (const auto& l : mcu->lines) TEST_ASSERT_FALSE(l.rfind("m ", 0) == 0);
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

void test_reconnect_after_nomcu_lowers_stored_volume_3() {
  mcu->applyVolume = true;
  mcu->alive = false;
  fan->begin(0);
  mcu->alive = true;
  fan->poll(NOMCU_RETRY_MS);
  TEST_ASSERT_EQUAL((int)FanState::READY, (int)fan->status().state);
  TEST_ASSERT_EQUAL(1, mcu->count("m 2\n"));
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

// The MCU drops input while busy: the lowered volume is read back and sent
// again if it did not take.
void test_connect_resends_a_dropped_volume_cap() {
  mcu->applyVolume = true;
  mcu->dropVolume = 1;
  fan->begin(0);
  TEST_ASSERT_EQUAL(2, mcu->count("m 2\n"));
  TEST_ASSERT_EQUAL_STRING("setting on all 2 2 164 F-S12\n", mcu->setting.c_str());
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

void test_connect_sends_the_cap_once_when_it_takes() {
  mcu->applyVolume = true;
  fan->begin(0);
  TEST_ASSERT_EQUAL(1, mcu->count("m 2\n"));
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

// READY through PlayInfo alone (get_setting silent, e.g. after the ESP reset
// under a playing MCU): the volume is unknown, so get_setting is retried every
// SETTINGS_RETRY_MS while READY and a stored 3 is lowered once it answers.
void test_unknown_volume_is_read_later_and_capped() {
  mcu->setting = "";
  mcu->applyVolume = true;
  fan->begin(0);
  TEST_ASSERT_EQUAL(0, fan->status().volume);
  mcu->setting = "setting on all 2 3 164 F-S12\n";
  for (uint32_t t = POLL_MS; t < SETTINGS_RETRY_MS; t += POLL_MS) fan->poll(t);   // the 3 s poll cadence
  TEST_ASSERT_EQUAL(0, mcu->count("m 2\n"));
  fan->poll(SETTINGS_RETRY_MS);
  TEST_ASSERT_EQUAL(1, mcu->count("m 2\n"));
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

// 0.9.8: the settings are read every SETTINGS_POLL_MS while READY, so a 3 set
// behind our back (an MCU re-init, the IR remote, the phone app) is capped
// within that time, and changes we did not command are reported.
void test_settings_are_read_every_period() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  int reads = mcu->count("get_setting\n");
  for (uint32_t t = POLL_MS; t < SETTINGS_POLL_MS; t += POLL_MS) fan->poll(t);
  TEST_ASSERT_EQUAL(reads, mcu->count("get_setting\n"));
  fan->poll(SETTINGS_POLL_MS);
  TEST_ASSERT_EQUAL(reads + 1, mcu->count("get_setting\n"));
  char out[120];
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));   // nothing changed
  TEST_ASSERT_EQUAL(0, (int)fan->unprompted());
}

void test_periodic_read_caps_a_hidden_3_and_reports_it() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  mcu->applyVolume = true;
  fan->begin(0);
  mcu->setting = "setting on all 2 3 164 F-S12\n";        // the MCU went to 3 by itself
  int caps = mcu->count("m 2\n");
  fan->poll(SETTINGS_POLL_MS);
  TEST_ASSERT_EQUAL(caps + 1, mcu->count("m 2\n"));
  TEST_ASSERT_EQUAL(2, fan->status().volume);
  char out[120];
  TEST_ASSERT_TRUE(fan->takeUnprompted(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("vol 2->3 (capped)", out);
  TEST_ASSERT_EQUAL(1, (int)fan->unprompted());
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));   // taken once
}

void test_unprompted_loop_ble_brightness_angle_changes_are_reported() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  fan->begin(0);
  mcu->setting = "setting off all 3 2 160 F-S12\n";
  fan->poll(SETTINGS_POLL_MS);
  char out[120];
  TEST_ASSERT_TRUE(fan->takeUnprompted(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ble on->off loop one->all br 2->3 angle 164->160", out);
}

// Our own commands move the baseline: no report.
void test_commanded_changes_are_not_reported() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  mcu->applyVolume = true;
  fan->begin(0);
  run(CmdType::VOLUME, 1);
  run(CmdType::LOOP, 0);
  mcu->setting = "setting on one 2 1 164 F-S12\n";
  fan->poll(SETTINGS_POLL_MS);
  char out[120];
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));
}

// A raw #HLFX line may change anything: the next read sets a new baseline.
void test_raw_lines_rebaseline_silently() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  fan->forgetBaseline();
  mcu->setting = "setting off one 1 1 100 F-S12\n";
  fan->poll(SETTINGS_POLL_MS);
  char out[120];
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));
  mcu->setting = "setting off one 1 2 100 F-S12\n";      // a later change is reported again
  fan->poll(2 * SETTINGS_POLL_MS);
  TEST_ASSERT_TRUE(fan->takeUnprompted(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("vol 1->2", out);
}

// #HLFY holds 3 on purpose: setVolumeChecked(3) allows it until a volume up
// to the cap is set again.
void test_hlfy_hold_is_not_capped() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  mcu->applyVolume = true;
  fan->begin(0);
  TEST_ASSERT_EQUAL(3, fan->setVolumeChecked(3));
  int caps = mcu->count("m 2\n");
  fan->poll(SETTINGS_POLL_MS);
  TEST_ASSERT_EQUAL(caps, mcu->count("m 2\n"));
  char out[120];
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));
  TEST_ASSERT_EQUAL(2, fan->setVolumeChecked(2));
  mcu->setting = "setting on all 2 3 164 F-S12\n";
  fan->poll(2 * SETTINGS_POLL_MS);
  TEST_ASSERT_EQUAL(caps + 2, mcu->count("m 2\n"));      // the restore, then the cap again
}

// After a clip lease (upload, delete, format) the MCU may have re-initialised:
// read the settings at the next poll instead of up to 30 s later.
void test_settings_checked_soon_after_a_lease() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  mcu->applyVolume = true;
  fan->begin(0);
  int reads = mcu->count("get_setting\n");
  fan->checkSettingsSoon();
  mcu->setting = "setting on all 2 3 164 F-S12\n";        // re-initialised: loop all, volume 3
  fan->poll(POLL_MS);
  TEST_ASSERT_EQUAL(reads + 2, mcu->count("get_setting\n"));   // the check, then the cap's read-back
  char out[120];
  TEST_ASSERT_TRUE(fan->takeUnprompted(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("loop one->all vol 2->3 (capped)", out);
}

// In loop one the clip must not change unless we asked.
void test_unprompted_clip_change_in_loop_one() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  fan->begin(0);
  mcu->playinfo = "PlayInfo \"03-Name.bin\" playing 1 24\n";
  fan->poll(POLL_MS);
  char out[120];
  TEST_ASSERT_TRUE(fan->takeUnprompted(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("clip 02-Name.bin->03-Name.bin", out);
}

void test_commanded_clip_change_is_not_reported() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  fan->begin(0);
  run(CmdType::NEXT, 0, 1000);
  mcu->playinfo = "PlayInfo \"03-Name.bin\" playing 1 24\n";
  fan->poll(1000 + QUICK_POLL_MS);
  fan->poll(1000 + QUICK_POLL_MS + POLL_MS);
  char out[120];
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));
}

void test_loop_all_clip_changes_are_normal() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  mcu->playinfo = "PlayInfo \"03-Name.bin\" playing 1 24\n";
  fan->poll(POLL_MS);
  char out[120];
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));
}

// Review of 0.9.8: a read-back lost after our own "m" must not leave a stale
// baseline that later reports our cap as UNPROMPTED.
void test_lost_cap_readback_is_not_reported_later() {
  mcu->applyVolume = true;
  mcu->silentAfterM = VOLUME_READS;                // every read-back after "m" is lost
  fan->begin(0);                                   // stored 3: capped, read-back lost
  mcu->silentAfterM = 0;
  fan->poll(SETTINGS_POLL_MS);                     // reads 2: our own cap took
  char out[120];
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));
}

// A cap made without a baseline (or of a 3 that was already the baseline) is
// still reported.
void test_cap_without_baseline_is_reported() {
  mcu->setting = "";
  fan->begin(0);                                   // READY through PlayInfo only
  mcu->setting = "setting on all 2 3 164 F-S12\n";
  mcu->applyVolume = true;
  fan->poll(SETTINGS_POLL_MS);
  char out[120];
  TEST_ASSERT_TRUE(fan->takeUnprompted(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("vol 3 (capped; no baseline)", out);
  TEST_ASSERT_EQUAL(3, fan->bootVolume());         // the first read fills the boot record (late)
  TEST_ASSERT_TRUE(fan->takeBootLate());
  TEST_ASSERT_FALSE(fan->takeBootLate());
}

// Switching to loop one starts a fresh clip baseline (the MCU may have moved
// on between our last poll and the 'i').
void test_loop_one_starts_a_fresh_clip_baseline() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  mcu->playinfo = "PlayInfo \"03-Name.bin\" playing 1 24\n";   // moved on in loop all
  run(CmdType::LOOP, 0);
  fan->poll(POLL_MS);
  char out[120];
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));
}

// A raw action re-baselines and reads the settings at the next poll.
void test_forget_baseline_reads_settings_soon() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  fan->begin(0);
  int reads = mcu->count("get_setting\n");
  fan->forgetBaseline();
  fan->poll(POLL_MS);
  TEST_ASSERT_EQUAL(reads + 1, mcu->count("get_setting\n"));
}

// Mute follows what the MCU reports: after an outside change away from 1, a
// later #HLFM mutes again.
void test_mute_state_follows_the_mcu() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  run(CmdType::MUTE);
  mcu->setting = "setting on all 2 2 164 F-S12\n";        // someone set 2 again
  fan->poll(SETTINGS_POLL_MS);
  int mutes = mcu->count("m 1\n");
  run(CmdType::MUTE);
  TEST_ASSERT_EQUAL(mutes + 1, mcu->count("m 1\n"));
}

// The check after a clip lease keeps trying for a few polls if the MCU is silent.
void test_post_lease_check_retries() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  mcu->applyVolume = true;
  fan->begin(0);
  fan->checkSettingsSoon();
  mcu->silentSettings = 1;                          // the first post-lease read is lost
  mcu->setting = "setting on all 2 3 164 F-S12\n";
  int caps = mcu->count("m 2\n");
  fan->poll(POLL_MS);
  TEST_ASSERT_EQUAL(caps, mcu->count("m 2\n"));
  fan->poll(2 * POLL_MS);
  TEST_ASSERT_EQUAL(caps + 1, mcu->count("m 2\n"));
}

// 0.9.8: after a clip lease (upload/delete/format) put back what the MCU
// changed by itself: volume, loop mode, brightness, and the clip that was
// playing (an upload pauses it; the MCU may also re-initialise).
static size_t after(const FakeMcu* m, size_t from, const char* line) {
  for (size_t i = from; i < m->lines.size(); i++) if (m->lines[i] == line) return i;
  return (size_t)-1;
}

void test_restore_after_lease_puts_back_the_state() {
  mcu->setting = "setting on one 2 1 164 F-S12\n";        // loop one, volume 1, playing 02-Name.bin
  fan->begin(0);
  fan->beginLease();
  size_t mark = mcu->lines.size();
  mcu->setting = "setting on all 3 3 164 F-S12\n";        // re-initialised: loop all, br 3, volume 3
  mcu->playinfo = "PlayInfo \"04-Name.bin\" playing 1 24\n";
  fan->endLease(true);
  fan->poll(POLL_MS);
  size_t m1 = after(mcu, mark, "m 1\n"), i = after(mcu, mark, "i\n"), k = after(mcu, mark, "k 2\n"),
         t = after(mcu, mark, "t 02-Name.bin\n");
  TEST_ASSERT_TRUE(m1 != (size_t)-1);
  TEST_ASSERT_TRUE(i != (size_t)-1);
  TEST_ASSERT_TRUE(k != (size_t)-1);
  TEST_ASSERT_TRUE(t != (size_t)-1);
  char out[120];
  TEST_ASSERT_TRUE(fan->takeLeaseReport(out, sizeof(out)));
  TEST_ASSERT_NOT_NULL(strstr(out, "loop one->all"));
  TEST_ASSERT_NOT_NULL(strstr(out, "restored"));
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));
  TEST_ASSERT_EQUAL(0, (int)fan->unprompted());          // lease changes are not "unprompted" (chg=)
  TEST_ASSERT_EQUAL(1, (int)fan->leaseChanges());
}

// The upload only paused the clip: it is started again, nothing else is sent.
void test_restore_after_lease_resumes_the_paused_clip() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  fan->begin(0);
  fan->beginLease();
  size_t mark = mcu->lines.size();
  mcu->playinfo = "PlayInfo \"02-Name.bin\" pause 8 24\n";
  fan->endLease(true);
  fan->poll(POLL_MS);
  TEST_ASSERT_TRUE(after(mcu, mark, "t 02-Name.bin\n") != (size_t)-1);
  TEST_ASSERT_EQUAL((size_t)-1, after(mcu, mark, "m 2\n"));
  TEST_ASSERT_EQUAL((size_t)-1, after(mcu, mark, "i\n"));
}

// Paused before the lease: stays paused; nothing changed: nothing is sent.
void test_restore_after_lease_leaves_a_paused_fan_paused() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  mcu->playinfo = "PlayInfo \"02-Name.bin\" pause 8 24\n";
  fan->begin(0);
  fan->beginLease();
  size_t mark = mcu->lines.size();
  fan->endLease(true);
  fan->poll(POLL_MS);
  for (size_t i = mark; i < mcu->lines.size(); i++)
    TEST_ASSERT_TRUE(mcu->lines[i] == "get_setting\n" || mcu->lines[i] == "get_playinfo\n" || mcu->lines[i] == "s\n");
}

// A command after the lease wins over the restore of what it set.
void test_restore_after_lease_respects_later_commands() {
  mcu->setting = "setting on one 2 1 164 F-S12\n";
  fan->begin(0);
  fan->beginLease();
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->endLease(true);
  run(CmdType::LOOP, 1, 1000);                      // the droid asked for loop all
  run(CmdType::PLAY_INDEX, 3, 1000);                // and clip 3
  size_t mark = mcu->lines.size();
  fan->poll(POLL_MS);
  TEST_ASSERT_EQUAL((size_t)-1, after(mcu, mark, "i\n"));
  TEST_ASSERT_EQUAL((size_t)-1, after(mcu, mark, "t 02-Name.bin\n"));
  TEST_ASSERT_TRUE(after(mcu, mark, "m 1\n") != (size_t)-1);   // the volume was not commanded: restored
}

// A clip that is gone (deleted, or a format) is not started again.
void test_restore_after_lease_skips_a_missing_clip() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  fan->begin(0);
  fan->beginLease();
  mcu->list = "03-Name.bin\n04-Name.bin\n";              // 02-Name.bin deleted
  mcu->playinfo = "PlayInfo \"\" idle 0 0\n";
  fan->endLease(true);
  size_t mark = mcu->lines.size();
  fan->poll(POLL_MS);                               // before the queued CATALOG_REFRESH, as in the firmware
  TEST_ASSERT_EQUAL((size_t)-1, after(mcu, mark, "t 02-Name.bin\n"));
}

// Review of 0833ada: a lease that changed nothing (a list, EXISTS, an SPI
// failure: release(false)) arms no restore; in loop all the clip moves on.
void test_read_only_lease_restores_nothing() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  fan->beginLease();
  mcu->playinfo = "PlayInfo \"04-Name.bin\" playing 1 24\n";   // loop all moved on
  fan->endLease(false);
  size_t mark = mcu->lines.size();
  fan->poll(POLL_MS);
  TEST_ASSERT_EQUAL((size_t)-1, after(mcu, mark, "t 02-Name.bin\n"));
}

// Loop all before and after, playing: whatever clip it is on is fine.
void test_loop_all_playing_is_left_alone() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  fan->beginLease();
  mcu->playinfo = "PlayInfo \"01-Other.bin\" playing 1 24\n";
  fan->endLease(true);
  size_t mark = mcu->lines.size();
  fan->poll(POLL_MS);
  TEST_ASSERT_EQUAL((size_t)-1, after(mcu, mark, "t 02-Name.bin\n"));
}

// A second lease before the first restore ran keeps the first snapshot.
void test_back_to_back_leases_keep_the_first_snapshot() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  fan->begin(0);
  fan->beginLease();
  std::string black = std::string("PlayInfo \"") + BLACK_CLIP_NAME + "\" pause 1 40\n";
  mcu->playinfo = black;                           // a dark upload: black clip, paused
  fan->endLease(true);
  mcu->silentSettings = SOON_TRIES - 1;            // the first post-lease reads are lost ...
  fan->poll(POLL_MS);                              // ... but PlayInfo answers (black, paused)
  fan->beginLease();                               // the next lease (e.g. the next upload's list)
  fan->endLease(false);
  size_t mark = mcu->lines.size();
  fan->poll(2 * POLL_MS);
  fan->poll(3 * POLL_MS);
  fan->poll(4 * POLL_MS);
  TEST_ASSERT_TRUE(after(mcu, mark, "t 02-Name.bin\n") != (size_t)-1);
}

// Paused before a dark upload: the right clip is shown again, paused.
void test_paused_before_a_dark_upload_gets_its_clip_back_paused() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  mcu->playinfo = "PlayInfo \"02-Name.bin\" pause 8 24\n";
  fan->begin(0);
  fan->beginLease();
  mcu->playinfo = std::string("PlayInfo \"") + BLACK_CLIP_NAME + "\" pause 1 40\n";
  fan->endLease(true);
  size_t mark = mcu->lines.size();
  fan->poll(POLL_MS);
  size_t t = after(mcu, mark, "t 02-Name.bin\n");
  TEST_ASSERT_TRUE(t != (size_t)-1);
  TEST_ASSERT_TRUE(after(mcu, t, "h\n") != (size_t)-1);
}

// An unanswered PlayInfo during the restore retries the clip part later.
void test_clip_restore_retries_without_playinfo() {
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  fan->begin(0);
  fan->beginLease();
  mcu->playinfo = "";                              // the MCU does not answer PlayInfo for now
  fan->endLease(true);
  size_t mark = mcu->lines.size();
  fan->poll(POLL_MS);
  TEST_ASSERT_EQUAL((size_t)-1, after(mcu, mark, "t 02-Name.bin\n"));
  mcu->playinfo = "PlayInfo \"02-Name.bin\" pause 8 24\n";
  fan->poll(2 * POLL_MS);
  TEST_ASSERT_TRUE(after(mcu, mark, "t 02-Name.bin\n") != (size_t)-1);
}

// No settings read since boot: the loop mode is unknown and not "restored".
void test_unknown_loop_mode_is_not_restored() {
  mcu->setting = "";
  fan->begin(0);                                   // READY through PlayInfo alone (loopAll defaults to all)
  fan->beginLease();
  mcu->setting = "setting on one 2 2 164 F-S12\n";
  fan->endLease(true);
  size_t mark = mcu->lines.size();
  fan->poll(POLL_MS);
  TEST_ASSERT_EQUAL((size_t)-1, after(mcu, mark, "j\n"));
}

// A restore that never got its settings read expires.
void test_restore_expires() {
  mcu->setting = "setting on one 2 1 164 F-S12\n";
  fan->begin(0);
  fan->beginLease();
  mcu->setting = "";                               // silent from here ...
  fan->endLease(true);
  for (uint32_t t = POLL_MS; t <= RESTORE_MAX_MS + POLL_MS; t += POLL_MS) fan->poll(t);
  mcu->setting = "setting on one 2 2 164 F-S12\n";   // ... back after the limit
  size_t mark = mcu->lines.size();
  fan->poll(RESTORE_MAX_MS + SETTINGS_POLL_MS + POLL_MS);
  TEST_ASSERT_EQUAL((size_t)-1, after(mcu, mark, "m 1\n"));
}

// #HLFY's own volume and raw lines win over a pending restore.
void test_hlfy_and_raw_lines_override_the_restore() {
  mcu->setting = "setting on one 2 1 164 F-S12\n";
  mcu->applyVolume = true;
  fan->begin(0);
  fan->beginLease();
  fan->endLease(true);
  fan->setVolumeChecked(2);
  size_t mark = mcu->lines.size();
  fan->poll(POLL_MS);
  TEST_ASSERT_EQUAL((size_t)-1, after(mcu, mark, "m 1\n"));
}

// 0.9.9 (Bluetooth is not needed on the mesh): with wantBluetooth(false) the MCU's
// ble setting is switched off at connect and kept off; reported, not silent.
// The firmware's order: begin() with no preference, then the saved choice
// (FanMesh posts BLUETOOTH once after NVS is up).
void test_bluetooth_switched_off_at_connect() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  TEST_ASSERT_EQUAL(0, mcu->count("d\n"));
  run(CmdType::BLUETOOTH, 0);
  TEST_ASSERT_EQUAL(1, mcu->count("d\n"));
  TEST_ASSERT_TRUE(mcu->settledAfter(mcu->indexOf("d\n")));
  TEST_ASSERT_EQUAL_STRING("setting off all 2 2 164 F-S12\n", mcu->setting.c_str());
  char out[160];
  TEST_ASSERT_TRUE(fan->formatBoot(out, sizeof(out)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(out, "ble on"));         // the boot record shows what the MCU said
}

void test_bluetooth_already_off_sends_nothing() {
  fan->wantBluetooth(false);
  mcu->setting = "setting off all 2 2 164 F-S12\n";
  fan->begin(0);
  TEST_ASSERT_EQUAL(0, mcu->count("d\n"));
}

void test_bluetooth_dont_care_by_default() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  fan->poll(SETTINGS_POLL_MS);
  TEST_ASSERT_EQUAL(0, mcu->count("d\n"));
}

void test_bluetooth_turned_back_on_by_the_mcu_is_switched_off_and_reported() {
  fan->wantBluetooth(false);
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  mcu->setting = "setting on all 2 2 164 F-S12\n";      // on again behind our back (e.g. an MCU re-init)
  fan->poll(SETTINGS_POLL_MS);
  TEST_ASSERT_EQUAL(2, mcu->count("d\n"));
  char out[120];
  TEST_ASSERT_TRUE(fan->takeUnprompted(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ble off->on (switched off)", out);
}

void test_bluetooth_command_switches_and_reads_back() {
  fan->wantBluetooth(false);
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  run(CmdType::BLUETOOTH, 1);
  TEST_ASSERT_EQUAL(1, mcu->count("c\n"));
  TEST_ASSERT_EQUAL_STRING("setting on all 2 2 164 F-S12\n", mcu->setting.c_str());
  fan->poll(SETTINGS_POLL_MS);                          // on is now wanted: left alone
  TEST_ASSERT_EQUAL(1, mcu->count("d\n"));
  run(CmdType::BLUETOOTH, 0);
  TEST_ASSERT_EQUAL(2, mcu->count("d\n"));
}

// Review of 3211124: one dropped "d" is sent again, as a dropped "m" is.
void test_one_dropped_d_still_switches_off() {
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  mcu->dropBle = 1;
  run(CmdType::BLUETOOTH, 0);
  TEST_ASSERT_EQUAL(2, mcu->count("d\n"));
  TEST_ASSERT_EQUAL_STRING("setting off all 2 2 164 F-S12\n", mcu->setting.c_str());
  char out[120];
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));
}

// A "stuck" MCU that later shows the wanted state is watched again.
void test_stuck_bluetooth_rearms_when_it_reads_right() {
  fan->wantBluetooth(false);
  mcu->applyBle = false;
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);                                   // stuck: reported once
  char out[120];
  fan->takeUnprompted(out, sizeof(out));
  mcu->setting = "setting off all 2 2 164 F-S12\n";     // it went off after all (late apply)
  fan->poll(SETTINGS_POLL_MS);
  int ds = mcu->count("d\n");
  mcu->applyBle = true;
  mcu->setting = "setting on all 2 2 164 F-S12\n";      // and on again later
  fan->poll(2 * SETTINGS_POLL_MS);
  TEST_ASSERT_EQUAL(ds + 1, mcu->count("d\n"));
}

// The 30 s read caps a 3 before it switches Bluetooth (the cap is the safety one).
void test_cap_comes_before_bluetooth() {
  fan->wantBluetooth(false);
  mcu->setting = "setting off all 2 2 164 F-S12\n";
  mcu->applyVolume = true;
  fan->begin(0);
  size_t mark = mcu->lines.size();
  mcu->setting = "setting on all 2 3 164 F-S12\n";      // both changed (an MCU re-init)
  fan->poll(SETTINGS_POLL_MS);
  size_t m = (size_t)-1, d = (size_t)-1;
  for (size_t i = mark; i < mcu->lines.size(); i++) {
    if (m == (size_t)-1 && mcu->lines[i] == "m 2\n") m = i;
    if (d == (size_t)-1 && mcu->lines[i] == "d\n") d = i;
  }
  TEST_ASSERT_TRUE(m != (size_t)-1);
  TEST_ASSERT_TRUE(d != (size_t)-1);
  TEST_ASSERT_TRUE(m < d);
}

// An MCU that keeps reporting ble on: one report, no "d" every 30 s.
void test_bluetooth_that_does_not_switch_is_reported_once() {
  fan->wantBluetooth(false);
  mcu->applyBle = false;
  mcu->setting = "setting on all 2 2 164 F-S12\n";
  fan->begin(0);
  int ds = mcu->count("d\n");
  TEST_ASSERT_EQUAL(2, ds);                        // tried twice, as a dropped line would be
  char out[120];
  TEST_ASSERT_TRUE(fan->takeUnprompted(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ble did not switch off (MCU reports on)", out);
  fan->poll(SETTINGS_POLL_MS);
  fan->poll(2 * SETTINGS_POLL_MS);
  TEST_ASSERT_EQUAL(ds, mcu->count("d\n"));
  TEST_ASSERT_FALSE(fan->takeUnprompted(out, sizeof(out)));
}

// What the MCU said at connect, before the cap touched it (0.9.8).
void test_boot_record_keeps_the_settings_before_the_cap() {
  mcu->applyVolume = true;                                // stored 3
  fan->begin(0);
  char out[160];
  TEST_ASSERT_TRUE(fan->formatBoot(out, sizeof(out)) > 0);
  TEST_ASSERT_EQUAL_STRING("[fan] MCU at connect: ble on, loop all, br 2, vol 3 (capped to 2), angle 164; playing 02-Name.bin 8/24",
                           out);
  TEST_ASSERT_EQUAL(3, fan->bootVolume());
}

void test_boot_record_unknown_without_settings() {
  mcu->setting = "";
  fan->begin(0);
  char out[160];
  TEST_ASSERT_TRUE(fan->formatBoot(out, sizeof(out)) > 0);
  TEST_ASSERT_EQUAL_STRING("[fan] MCU at connect: settings not read; playing 02-Name.bin 8/24", out);
  TEST_ASSERT_EQUAL(0, fan->bootVolume());
}

// The wake bytes and a query that got no answer are followed by a settle
// before the next line (0.9.8; the MCU may still be chewing on them).
void test_settle_after_wake_and_after_a_silent_query() {
  mcu->alive = false;
  fan->begin(0);
  TEST_ASSERT_TRUE(mcu->settles.size() > 0);
  TEST_ASSERT_EQUAL(0, (int)mcu->settles[0]);                 // after wake, before the first get_setting
  TEST_ASSERT_TRUE(mcu->settledAfter(0));                     // after the first unanswered get_setting
}

// #HLFY (bench, 0.9.4): set a volume past the cap and read it back, sending
// once more if the MCU dropped the line; returns what the MCU reports.
void test_checked_volume_sets_3_past_the_cap() {
  mcu->applyVolume = true;
  fan->begin(0);                                   // the stored 3 is lowered to 2 first
  TEST_ASSERT_EQUAL(3, fan->setVolumeChecked(3));
  TEST_ASSERT_EQUAL(1, mcu->count("m 3\n"));
  TEST_ASSERT_EQUAL(3, fan->status().volume);
  TEST_ASSERT_EQUAL_STRING("get_setting\n", mcu->last().c_str());
}

void test_checked_volume_resends_a_dropped_line() {
  mcu->applyVolume = true;
  fan->begin(0);
  mcu->dropVolume = 1;
  TEST_ASSERT_EQUAL(3, fan->setVolumeChecked(3));
  TEST_ASSERT_EQUAL(2, mcu->count("m 3\n"));
}

void test_checked_volume_reports_what_the_mcu_kept() {
  mcu->applyVolume = true;
  fan->begin(0);
  mcu->dropVolume = 2;                             // both sends dropped
  TEST_ASSERT_EQUAL(2, fan->setVolumeChecked(3));
  TEST_ASSERT_EQUAL(2, mcu->count("m 3\n"));
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

void test_checked_volume_silent_mcu_reports_0() {
  mcu->applyVolume = true;
  fan->begin(0);
  mcu->alive = false;                              // get_setting no longer answers
  int reads = mcu->count("get_setting\n");
  TEST_ASSERT_EQUAL(0, fan->setVolumeChecked(3));
  TEST_ASSERT_EQUAL(1, mcu->count("m 3\n"));         // silence is not a dropped "m": no resend
  TEST_ASSERT_EQUAL(reads + VOLUME_READS, mcu->count("get_setting\n"));
  TEST_ASSERT_EQUAL(0, fan->status().volume);       // unknown: poll reads it again and caps it
}

// Bench 2026-10-07 (0.9.4): right after "m" the MCU often leaves get_setting
// unanswered for a while. The read is retried before "m" is sent again.
void test_checked_volume_waits_out_a_busy_mcu() {
  mcu->applyVolume = true;
  fan->begin(0);
  int reads = mcu->count("get_setting\n");
  mcu->silentSettings = VOLUME_READS - 1;
  TEST_ASSERT_EQUAL(3, fan->setVolumeChecked(3));
  TEST_ASSERT_EQUAL(1, mcu->count("m 3\n"));
  TEST_ASSERT_EQUAL(reads + VOLUME_READS, mcu->count("get_setting\n"));
}

// No read-back after a restore: the volume is unknown, so the next settings
// read (SETTINGS_RETRY_MS cadence) lowers a 3 that stuck.
void test_unconfirmed_volume_is_read_again_and_capped() {
  mcu->applyVolume = true;
  fan->begin(0);
  fan->setVolumeChecked(3);
  mcu->alive = false;
  TEST_ASSERT_EQUAL(0, fan->setVolumeChecked(2));   // "m 2" sent twice, never confirmed
  mcu->alive = true;
  mcu->dropVolume = 0;
  mcu->setting = "setting on all 2 3 164 F-S12\n";  // it did not take
  int lowered = mcu->count("m 2\n");
  for (uint32_t t = POLL_MS; t <= SETTINGS_RETRY_MS + POLL_MS; t += POLL_MS) fan->poll(t);
  TEST_ASSERT_EQUAL(lowered + 1, mcu->count("m 2\n"));
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

// NOMCU: a lower volume (<= the cap) is still sent, without a read-back; 3 never is.
void test_checked_volume_nomcu_still_lowers() {
  mcu->alive = false;
  fan->begin(0);
  TEST_ASSERT_EQUAL(0, fan->setVolumeChecked(2));
  TEST_ASSERT_EQUAL(1, mcu->count("m 2\n"));
  TEST_ASSERT_EQUAL_STRING("nomcu", fan->status().err);
}

void test_checked_volume_needs_ready_and_a_level() {
  mcu->alive = false;
  fan->begin(0);
  size_t n = mcu->lines.size();
  TEST_ASSERT_EQUAL(0, fan->setVolumeChecked(3));
  TEST_ASSERT_EQUAL(n, mcu->lines.size());         // NOMCU: never raised
  mcu->alive = true;
  fan->poll(NOMCU_RETRY_MS);
  n = mcu->lines.size();
  TEST_ASSERT_EQUAL(0, fan->setVolumeChecked(0));
  TEST_ASSERT_EQUAL(0, fan->setVolumeChecked(4));
  TEST_ASSERT_EQUAL(n, mcu->lines.size());
}

// While the test holds volume 3, the poll leaves it alone (the volume is
// known), and the restore reads back too.
void test_checked_volume_3_survives_polls_and_restores() {
  mcu->applyVolume = true;
  fan->begin(0);
  fan->setVolumeChecked(3);
  int lowered = mcu->count("m 2\n");
  for (uint32_t t = POLL_MS; t <= 3 * SETTINGS_RETRY_MS; t += POLL_MS) fan->poll(t);
  TEST_ASSERT_EQUAL(lowered, mcu->count("m 2\n"));
  TEST_ASSERT_EQUAL(2, fan->setVolumeChecked(2));
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

// Bench 2026-10-07 (0.9.6 trace): "m 3" then get_setting 4 ms later; the MCU,
// still busy with "m", lost the 'g' and ran "et_setting" as 'e' (previous
// clip: mculog:prev.). Every action is followed by a settle (as ClipOps does
// after "h" and CT) before the next line goes out.
void test_actions_let_the_mcu_settle() {
  mcu->applyVolume = true;
  fan->begin(0);                                   // caps the stored 3: "m 2" then get_setting
  size_t cap = mcu->indexOf("m 2\n");
  TEST_ASSERT_TRUE(cap != (size_t)-1);
  TEST_ASSERT_TRUE(mcu->settledAfter(cap));
  TEST_ASSERT_EQUAL(3, fan->setVolumeChecked(3));
  TEST_ASSERT_TRUE(mcu->settledAfter(mcu->indexOf("m 3\n")));
  const struct { CmdType t; int p; const char* line; } acts[] = {
    {CmdType::VOLUME, 1, "m 1\n"}, {CmdType::BRIGHTNESS, 3, "k 3\n"}, {CmdType::PAUSE, 0, "h\n"},
    {CmdType::NEXT, 0, "f\n"}, {CmdType::PREV, 0, "e\n"}, {CmdType::LOOP, 1, "j\n"}, {CmdType::SWITCHER, 0, "b\n"},
    {CmdType::PLAY_INDEX, 2, "t 03-Name.bin\n"},
  };
  for (const auto& a : acts) {
    run(a.t, a.p);
    TEST_ASSERT_EQUAL_STRING(a.line, mcu->lines.back().c_str());
    TEST_ASSERT_TRUE(mcu->settledAfter(mcu->lines.size() - 1));
  }
}

// #HLFT<NN> (0.9.3): the clip whose name starts with NN- / NN_ in the MCU's
// full listing (not the 32-entry catalog), played with "t" only.
void test_play_slot_plays_the_matching_name() {
  fan->begin(0);
  run(CmdType::PLAY_SLOT, 4);
  TEST_ASSERT_EQUAL_STRING("t 04-Name.bin\n", mcu->last().c_str());
  TEST_ASSERT_EQUAL_STRING("none", fan->status().err);
}

void test_play_slot_missing_is_noslot() {
  fan->begin(0);
  size_t n = mcu->lines.size();
  run(CmdType::PLAY_SLOT, 9);
  TEST_ASSERT_EQUAL_STRING("s\n", mcu->last().c_str());   // listed, nothing played
  TEST_ASSERT_EQUAL(n + 1, mcu->lines.size());
  TEST_ASSERT_EQUAL_STRING("noslot", fan->status().err);
}

void test_play_slot_reaches_past_the_catalog() {
  std::string big;
  for (int i = 0; i < 40; i++) {
    char l[24];
    snprintf(l, sizeof(l), "%02d-Clip.bin\n", i);
    big += l;
  }
  mcu->list = big;
  fan->begin(0);
  TEST_ASSERT_EQUAL(CATALOG_MAX_TEST, fan->catalog().count);
  run(CmdType::PLAY_SLOT, 39);
  TEST_ASSERT_EQUAL_STRING("t 39-Clip.bin\n", mcu->last().c_str());
}

void test_play_slot_needs_the_mcu() {
  mcu->alive = false;
  fan->begin(0);
  size_t n = mcu->lines.size();
  run(CmdType::PLAY_SLOT, 4);
  TEST_ASSERT_EQUAL(n, mcu->lines.size());
  TEST_ASSERT_EQUAL_STRING("nomcu", fan->status().err);
}

// Volume never read (get_setting silent): a mute saves the cap, not 3.
void test_unmute_with_unknown_volume_restores_the_cap() {
  mcu->setting = "";
  fan->begin(0);
  run(CmdType::MUTE);
  run(CmdType::UNMUTE);
  TEST_ASSERT_EQUAL_STRING("m 2\n", mcu->last().c_str());
  TEST_ASSERT_EQUAL(2, fan->status().volume);
}

// Any reply carrying "PlayInfo" proves the MCU is alive, parsed or not.
void test_unparseable_playinfo_keeps_ready() {
  fan->begin(0);
  mcu->playinfo = "PlayInfo garbage\n";
  uint32_t t = 0;
  for (int i = 0; i < POLL_MISSES_NOMCU + 3; i++) { t += POLL_MS; fan->poll(t); }
  TEST_ASSERT_EQUAL((int)FanState::READY, (int)fan->status().state);
  TEST_ASSERT_EQUAL_STRING("02-Name.bin", fan->status().fileName);   // not clobbered
}

void test_playinfo_name_sanitized() {
  fan->begin(0);
  mcu->playinfo = "PlayInfo \"a^b\xC3\xA9 c.bin\" playing 1 2\n";
  fan->poll(POLL_MS);
  TEST_ASSERT_EQUAL_STRING("a?b???c.bin", fan->status().fileName);
  char out[256];
  TEST_ASSERT_GREATER_THAN(0, holofan::formatStatus(fan->status(), out, sizeof(out)));
  TEST_ASSERT_NULL(strchr(out, '^'));
  TEST_ASSERT_GREATER_THAN(0, holofan::formatCatalog(fan->catalog(), out, sizeof(out)));
  TEST_ASSERT_NULL(strchr(out, '^'));
}

void test_catalog_never_carries_caret() {
  mcu->list = "02-Name.bin\na^b.bin\n03-Name.bin\n";
  fan->begin(0);
  TEST_ASSERT_EQUAL(2, fan->catalog().count);
  char out[256];
  holofan::formatCatalog(fan->catalog(), out, sizeof(out));
  TEST_ASSERT_NULL(strchr(out, '^'));
}

void test_mark_no_link() {
  fan->markNoLink();
  TEST_ASSERT_EQUAL((int)FanState::NOMCU, (int)fan->status().state);
  TEST_ASSERT_EQUAL_STRING("nouart", fan->status().err);
}

void test_catalog_refresh() {
  fan->begin(0);
  mcu->list = "07-New.bin\n";
  run(CmdType::CATALOG_REFRESH);
  TEST_ASSERT_EQUAL(1, fan->catalog().count);
  TEST_ASSERT_EQUAL_STRING("07-New.bin", fan->catalog().entries[0].name);
  TEST_ASSERT_EQUAL(0, fan->status().fileIndex);   // 02-Name.bin is no longer listed
}

// No answer to "s": keep the catalog we had.
void test_catalog_refresh_timeout_keeps_old() {
  fan->begin(0);
  mcu->list = "";
  run(CmdType::CATALOG_REFRESH);
  TEST_ASSERT_EQUAL(4, fan->catalog().count);
  TEST_ASSERT_EQUAL_STRING("timeout", fan->status().err);
}

// Names come from the MCU; one that cannot be framed safely never reaches the wire.
void test_unsafe_names_never_listed() {
  fan->begin(0);
  mcu->list = "02-Name.bin\nbad name.bin\nv\n";
  run(CmdType::CATALOG_REFRESH);
  TEST_ASSERT_EQUAL(1, fan->catalog().count);
}

void test_send_failure_reported() {
  fan->begin(0);
  mcu->sendOk = false;
  run(CmdType::PAUSE);
  TEST_ASSERT_EQUAL_STRING("txfail", fan->status().err);
  mcu->sendOk = true;
  run(CmdType::PAUSE);
  TEST_ASSERT_EQUAL_STRING("none", fan->status().err);
}

void test_queries_and_power_are_not_actions() {
  fan->begin(0);
  size_t before = mcu->lines.size();
  run(CmdType::STATUS);
  run(CmdType::CATALOG_DUMP);
  run(CmdType::POWER_ON);
  run(CmdType::POWER_OFF);
  TEST_ASSERT_EQUAL(before, mcu->lines.size());
}

// Nothing the controller can be asked to do ever puts a line starting with 'v' on the wire.
void test_never_sends_v() {
  fan->begin(0);
  for (int t = (int)CmdType::NONE; t <= (int)CmdType::STATUS; t++)
    for (int p = -1; p <= 40; p++) run((CmdType)t, p);
  for (const auto& l : mcu->lines) TEST_ASSERT_NOT_EQUAL('v', l[0]);
}

// The MCU is alive and playing but no longer answers get_setting (seen in
// the droid after the C2 rebooted at power-up): a PlayInfo reply is proof
// of life, so the fan must still come up READY.
void test_begin_ready_on_playinfo_only() {
  mcu->setting = "";
  fan->begin(0);
  TEST_ASSERT_EQUAL((int)FanState::READY, (int)fan->status().state);
  TEST_ASSERT_TRUE(fan->mcuAnswered());
  TEST_ASSERT_EQUAL(4, fan->catalog().count);
  TEST_ASSERT_EQUAL_STRING("02-Name.bin", fan->status().fileName);
}

void test_nomcu_recovers_on_playinfo_and_rewakes() {
  mcu->alive = false;
  fan->begin(0);
  int wakes = mcu->wakes;
  mcu->alive = true;
  mcu->setting = "";
  fan->poll(NOMCU_RETRY_MS);
  TEST_ASSERT_EQUAL(wakes + 1, mcu->wakes);        // wake bytes sent again before retrying
  TEST_ASSERT_EQUAL((int)FanState::READY, (int)fan->status().state);
}

void test_tries_counted() {
  mcu->alive = false;
  fan->begin(0);
  TEST_ASSERT_EQUAL(SETTING_TRIES + 1, fan->queryTries());   // 21 get_setting + 1 get_playinfo
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_begin_ready_loads_settings_catalog_and_play);
  RUN_TEST(test_begin_silent_mcu_is_nomcu);
  RUN_TEST(test_nomcu_drops_actions);
  RUN_TEST(test_nomcu_recovers_on_retry);
  RUN_TEST(test_poll_cadence_and_update);
  RUN_TEST(test_ready_goes_nomcu_after_silent_polls);
  RUN_TEST(test_play_index_sends_only_the_name);
  RUN_TEST(test_play_index_out_of_range);
  RUN_TEST(test_playback_schedules_quick_poll);
  RUN_TEST(test_simple_actions_map_to_mcu_letters);
  RUN_TEST(test_levels_out_of_range_ignored);
  RUN_TEST(test_mute_and_unmute_restore_volume);
  RUN_TEST(test_unmute_at_floor_volume_sets_max);
  RUN_TEST(test_volume_3_is_capped_to_2);
  RUN_TEST(test_cap_3_keeps_a_stored_volume_3);
  RUN_TEST(test_cap_3_survives_the_periodic_read);
  RUN_TEST(test_cap_3_lets_volume_3_through);
  RUN_TEST(test_cap_3_unmute_at_floor_sets_3);
  RUN_TEST(test_cap_command_lowers_a_3_at_once);
  RUN_TEST(test_cap_command_raises_nothing);
  RUN_TEST(test_cap_command_is_remembered_while_nomcu);
  RUN_TEST(test_bad_cap_is_the_default);
  RUN_TEST(test_connect_lowers_stored_volume_3);
  RUN_TEST(test_connect_keeps_stored_volume_2);
  RUN_TEST(test_reconnect_after_nomcu_lowers_stored_volume_3);
  RUN_TEST(test_unmute_with_unknown_volume_restores_the_cap);
  RUN_TEST(test_connect_resends_a_dropped_volume_cap);
  RUN_TEST(test_connect_sends_the_cap_once_when_it_takes);
  RUN_TEST(test_unknown_volume_is_read_later_and_capped);
  RUN_TEST(test_settings_are_read_every_period);
  RUN_TEST(test_periodic_read_caps_a_hidden_3_and_reports_it);
  RUN_TEST(test_unprompted_loop_ble_brightness_angle_changes_are_reported);
  RUN_TEST(test_commanded_changes_are_not_reported);
  RUN_TEST(test_raw_lines_rebaseline_silently);
  RUN_TEST(test_hlfy_hold_is_not_capped);
  RUN_TEST(test_settings_checked_soon_after_a_lease);
  RUN_TEST(test_unprompted_clip_change_in_loop_one);
  RUN_TEST(test_commanded_clip_change_is_not_reported);
  RUN_TEST(test_loop_all_clip_changes_are_normal);
  RUN_TEST(test_lost_cap_readback_is_not_reported_later);
  RUN_TEST(test_cap_without_baseline_is_reported);
  RUN_TEST(test_loop_one_starts_a_fresh_clip_baseline);
  RUN_TEST(test_forget_baseline_reads_settings_soon);
  RUN_TEST(test_mute_state_follows_the_mcu);
  RUN_TEST(test_post_lease_check_retries);
  RUN_TEST(test_restore_after_lease_puts_back_the_state);
  RUN_TEST(test_restore_after_lease_resumes_the_paused_clip);
  RUN_TEST(test_restore_after_lease_leaves_a_paused_fan_paused);
  RUN_TEST(test_restore_after_lease_respects_later_commands);
  RUN_TEST(test_restore_after_lease_skips_a_missing_clip);
  RUN_TEST(test_read_only_lease_restores_nothing);
  RUN_TEST(test_loop_all_playing_is_left_alone);
  RUN_TEST(test_back_to_back_leases_keep_the_first_snapshot);
  RUN_TEST(test_paused_before_a_dark_upload_gets_its_clip_back_paused);
  RUN_TEST(test_clip_restore_retries_without_playinfo);
  RUN_TEST(test_unknown_loop_mode_is_not_restored);
  RUN_TEST(test_restore_expires);
  RUN_TEST(test_hlfy_and_raw_lines_override_the_restore);
  RUN_TEST(test_bluetooth_switched_off_at_connect);
  RUN_TEST(test_bluetooth_already_off_sends_nothing);
  RUN_TEST(test_bluetooth_dont_care_by_default);
  RUN_TEST(test_bluetooth_turned_back_on_by_the_mcu_is_switched_off_and_reported);
  RUN_TEST(test_bluetooth_command_switches_and_reads_back);
  RUN_TEST(test_bluetooth_that_does_not_switch_is_reported_once);
  RUN_TEST(test_one_dropped_d_still_switches_off);
  RUN_TEST(test_stuck_bluetooth_rearms_when_it_reads_right);
  RUN_TEST(test_cap_comes_before_bluetooth);
  RUN_TEST(test_boot_record_keeps_the_settings_before_the_cap);
  RUN_TEST(test_boot_record_unknown_without_settings);
  RUN_TEST(test_settle_after_wake_and_after_a_silent_query);
  RUN_TEST(test_checked_volume_sets_3_past_the_cap);
  RUN_TEST(test_checked_volume_resends_a_dropped_line);
  RUN_TEST(test_checked_volume_reports_what_the_mcu_kept);
  RUN_TEST(test_checked_volume_silent_mcu_reports_0);
  RUN_TEST(test_checked_volume_needs_ready_and_a_level);
  RUN_TEST(test_checked_volume_3_survives_polls_and_restores);
  RUN_TEST(test_unconfirmed_volume_is_read_again_and_capped);
  RUN_TEST(test_checked_volume_waits_out_a_busy_mcu);
  RUN_TEST(test_checked_volume_nomcu_still_lowers);
  RUN_TEST(test_actions_let_the_mcu_settle);
  RUN_TEST(test_play_slot_plays_the_matching_name);
  RUN_TEST(test_play_slot_missing_is_noslot);
  RUN_TEST(test_play_slot_reaches_past_the_catalog);
  RUN_TEST(test_play_slot_needs_the_mcu);
  RUN_TEST(test_unparseable_playinfo_keeps_ready);
  RUN_TEST(test_playinfo_name_sanitized);
  RUN_TEST(test_catalog_never_carries_caret);
  RUN_TEST(test_mark_no_link);
  RUN_TEST(test_catalog_refresh);
  RUN_TEST(test_catalog_refresh_timeout_keeps_old);
  RUN_TEST(test_unsafe_names_never_listed);
  RUN_TEST(test_send_failure_reported);
  RUN_TEST(test_queries_and_power_are_not_actions);
  RUN_TEST(test_never_sends_v);
  RUN_TEST(test_begin_ready_on_playinfo_only);
  RUN_TEST(test_nomcu_recovers_on_playinfo_and_rewakes);
  RUN_TEST(test_tries_counted);
  return UNITY_END();
}
