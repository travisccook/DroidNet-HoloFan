#include <unity.h>
#include <string>
#include <stdio.h>
#include <string.h>
#include "McuProto.h"

using namespace fanlogic;

void setUp() {}
void tearDown() {}

static int build(const char* cmd, const char* a0 = nullptr, const char* a1 = nullptr, char* out = nullptr) {
  static char buf[MCU_LINE_MAX];
  const char* args[2] = {a0, a1};
  int argc = a1 ? 2 : (a0 ? 1 : 0);
  return buildLine(cmd, args, argc, out ? out : buf, MCU_LINE_MAX);
}

void test_build_plain_and_with_args() {
  char out[MCU_LINE_MAX];
  TEST_ASSERT_EQUAL(12, build("get_setting", nullptr, nullptr, out));
  TEST_ASSERT_EQUAL_STRING("get_setting\n", out);
  TEST_ASSERT_EQUAL(14, build("t", "02-Name.bin", nullptr, out));
  TEST_ASSERT_EQUAL_STRING("t 02-Name.bin\n", out);
  TEST_ASSERT_EQUAL(4, build("k", "2", nullptr, out));
  TEST_ASSERT_EQUAL_STRING("k 2\n", out);
  build("w", "99_test.bin", "28800", out);
  TEST_ASSERT_EQUAL_STRING("w 99_test.bin 28800\n", out);
}

// The MCU's SD-card format command is 'v'. It must never be built, whatever follows.
void test_v_is_never_built() {
  TEST_ASSERT_EQUAL(-1, build("v"));
  TEST_ASSERT_EQUAL(-1, build("v", "x"));
  TEST_ASSERT_EQUAL(-1, build("vol"));
}

// An argument can never start a second line or split the framing.
void test_args_cannot_break_framing() {
  TEST_ASSERT_EQUAL(-1, build("t", "a\nv"));
  TEST_ASSERT_EQUAL(-1, build("t", "a\rb"));
  TEST_ASSERT_EQUAL(-1, build("t", "a b.bin"));
  TEST_ASSERT_EQUAL(-1, build("t", ""));
  TEST_ASSERT_EQUAL(-1, build("t", "\x01" "x.bin"));
  TEST_ASSERT_EQUAL(-1, build("t", "caf\xC3\xA9.bin"));   // non-ASCII
  TEST_ASSERT_EQUAL(-1, build("t", "ok.bin", "\n"));
}

// md5 (read-only) is on the #HLFX allowlist: digits after the first letter.
void test_build_command_with_digits() {
  char out[MCU_LINE_MAX];
  TEST_ASSERT_EQUAL(16, build("md5", "02-Name.bin", nullptr, out));
  TEST_ASSERT_EQUAL_STRING("md5 02-Name.bin\n", out);
  TEST_ASSERT_EQUAL(-1, build("5md"));
  TEST_ASSERT_EQUAL(-1, build("v5"));
}

void test_bad_commands_refused() {
  TEST_ASSERT_EQUAL(-1, build(""));
  TEST_ASSERT_EQUAL(-1, build("G"));          // upper case
  TEST_ASSERT_EQUAL(-1, build("t\n"));
  TEST_ASSERT_EQUAL(-1, build("get setting"));
  char out[MCU_LINE_MAX];
  const char* args[1] = {"x.bin"};
  TEST_ASSERT_EQUAL(-1, buildLine("t", args, -1, out, sizeof(out)));
}

void test_too_long_refused() {
  char longName[80];
  memset(longName, 'a', 75);
  strcpy(longName + 75, ".bin");                          // 79 chars + "t " + "\n" = 82
  char out[MCU_LINE_MAX];
  const char* args[1] = {longName};
  TEST_ASSERT_EQUAL(82, buildLine("t", args, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL(-1, buildLine("t", args, 1, out, 82));  // no room for the NUL
  char small[4];
  TEST_ASSERT_EQUAL(-1, buildLine("get_setting", nullptr, 0, small, sizeof(small)));
}

void test_clip_names() {
  TEST_ASSERT_TRUE(isClipName("02-Name.bin"));
  TEST_ASSERT_TRUE(isClipName("x.bin"));
  TEST_ASSERT_FALSE(isClipName(".bin"));
  TEST_ASSERT_FALSE(isClipName("02-Name.BIN"));
  TEST_ASSERT_FALSE(isClipName("02 Name.bin"));
  TEST_ASSERT_FALSE(isClipName("mculog:pause."));
  TEST_ASSERT_FALSE(isClipName(""));
  TEST_ASSERT_FALSE(isClipName("a^b.bin"));    // '^' is the mesh reply delimiter
  char n64[70];
  memset(n64, 'a', 60); strcpy(n64 + 60, ".bin");          // 64 chars: one too many
  TEST_ASSERT_FALSE(isClipName(n64));
  n64[59] = '\0'; strcat(n64, ".bin");                     // 63 chars
  TEST_ASSERT_TRUE(isClipName(n64));
}

// Captured on hardware: "setting on all 2 3 164 F-S12".
void test_parse_setting() {
  Settings s;
  TEST_ASSERT_TRUE(parseSetting("mculog:boot\nsetting on all 2 3 164 F-S12\n", s));
  TEST_ASSERT_TRUE(s.ble);
  TEST_ASSERT_TRUE(s.loopAll);
  TEST_ASSERT_EQUAL(2, s.brightness);
  TEST_ASSERT_EQUAL(3, s.volume);
  TEST_ASSERT_EQUAL(164, s.angle);
  TEST_ASSERT_EQUAL_STRING("F-S12", s.model);
  TEST_ASSERT_TRUE(parseSetting("setting off one 1 1 0 F-S12\r\n", s));
  TEST_ASSERT_FALSE(s.ble);
  TEST_ASSERT_FALSE(s.loopAll);
}

void test_parse_setting_rejects_partial() {
  Settings s;
  s.brightness = 9;
  TEST_ASSERT_FALSE(parseSetting("", s));
  TEST_ASSERT_FALSE(parseSetting(nullptr, s));
  TEST_ASSERT_FALSE(parseSetting("setting on all 2 3", s));
  TEST_ASSERT_FALSE(parseSetting("setting on all two 3 164 F-S12", s));
  TEST_ASSERT_FALSE(parseSetting("setting on all 2 3 164\nF-S12", s));   // fields split across lines
  TEST_ASSERT_EQUAL(9, s.brightness);                                   // untouched on failure
}

// Captured on hardware: PlayInfo "02-Name.bin" playing 8 24.
void test_parse_playinfo() {
  PlayInfo p;
  TEST_ASSERT_TRUE(parsePlayInfo("PlayInfo \"02-Name.bin\" playing 8 24\n", p));
  TEST_ASSERT_EQUAL_STRING("02-Name.bin", p.name);
  TEST_ASSERT_TRUE(p.playing);
  TEST_ASSERT_EQUAL(8, p.progress);
  TEST_ASSERT_EQUAL(24, p.total);
  TEST_ASSERT_TRUE(parsePlayInfo("mculog:pause.\nPlayInfo 03-Name.bin pause 19 24", p));
  TEST_ASSERT_EQUAL_STRING("03-Name.bin", p.name);
  TEST_ASSERT_FALSE(p.playing);
}

void test_parse_playinfo_quoted_name_with_space() {
  PlayInfo p;
  TEST_ASSERT_TRUE(parsePlayInfo("PlayInfo \"My clip.bin\" playing 8 24\n", p));
  TEST_ASSERT_EQUAL_STRING("My clip.bin", p.name);
  TEST_ASSERT_TRUE(p.playing);
  TEST_ASSERT_EQUAL(8, p.progress);
  TEST_ASSERT_EQUAL(24, p.total);
}

// Idle: the MCU reports an empty quoted name.
void test_parse_playinfo_empty_name() {
  PlayInfo p;
  TEST_ASSERT_TRUE(parsePlayInfo("PlayInfo \"\" stop 0 0\n", p));
  TEST_ASSERT_EQUAL_STRING("", p.name);
  TEST_ASSERT_FALSE(p.playing);
  TEST_ASSERT_EQUAL(0, p.total);
}

void test_parse_playinfo_rejects_partial() {
  PlayInfo p;
  TEST_ASSERT_FALSE(parsePlayInfo("PlayInfo \"02-Name.bin\" playing 8", p));
  TEST_ASSERT_FALSE(parsePlayInfo("mculog:pause.", p));
  TEST_ASSERT_FALSE(parsePlayInfo("PlayInfo", p));
}

// Captured on hardware: four names, one per line.
void test_parse_list() {
  holofan::Catalog c;
  TEST_ASSERT_EQUAL(4, parseList("02-Name.bin\n03-Name.bin\n04-Name.bin\n05-Name.bin", c));
  TEST_ASSERT_EQUAL_STRING("02-Name.bin", c.entries[0].name);
  TEST_ASSERT_EQUAL_STRING("05-Name.bin", c.entries[3].name);
  TEST_ASSERT_EQUAL(2, parseList("mculog:list\r\n\r\n02-Name.bin\r\nDCIM recv file list\r\n03-Name.bin\r\n", c));
  TEST_ASSERT_EQUAL_STRING("03-Name.bin", c.entries[1].name);
  TEST_ASSERT_EQUAL(0, parseList("", c));
  TEST_ASSERT_EQUAL(0, c.count);
}

void test_parse_list_caps_at_catalog_max() {
  char buf[2048] = {0};
  for (int i = 0; i < 40; i++) {
    char line[24];
    snprintf(line, sizeof(line), "%02d-Clip.bin\n", i);
    strcat(buf, line);
  }
  holofan::Catalog c;
  TEST_ASSERT_EQUAL(holofan::CATALOG_MAX, parseList(buf, c));
  TEST_ASSERT_EQUAL_STRING("31-Clip.bin", c.entries[31].name);
}

void test_raw_mesh_allowlist() {
  TEST_ASSERT_TRUE(rawMeshAllowed("t"));
  TEST_ASSERT_TRUE(rawMeshAllowed("get_playinfo"));
  TEST_ASSERT_TRUE(rawMeshAllowed("g"));
  TEST_ASSERT_FALSE(rawMeshAllowed("v"));     // SD format
  TEST_ASSERT_FALSE(rawMeshAllowed("u"));     // delete
  TEST_ASSERT_FALSE(rawMeshAllowed("w"));     // upload/create
  TEST_ASSERT_FALSE(rawMeshAllowed("y"));     // set clock
  TEST_ASSERT_FALSE(rawMeshAllowed(""));
  TEST_ASSERT_FALSE(rawMeshAllowed(nullptr));
  TEST_ASSERT_FALSE(rawMeshAllowed("tt"));
}

// The whole #HLFX line: the volume command only with a level the MCU knows, 1-3.
void test_raw_mesh_line_volume_levels() {
  TEST_ASSERT_TRUE(rawMeshLineAllowed("m 1"));
  TEST_ASSERT_TRUE(rawMeshLineAllowed("m 2"));
  TEST_ASSERT_TRUE(rawMeshLineAllowed("m 3"));
  TEST_ASSERT_FALSE(rawMeshLineAllowed("m 9"));
  TEST_ASSERT_FALSE(rawMeshLineAllowed("m 0"));
  TEST_ASSERT_FALSE(rawMeshLineAllowed("m"));
  TEST_ASSERT_FALSE(rawMeshLineAllowed("m x"));
  TEST_ASSERT_FALSE(rawMeshLineAllowed("m 2 3"));
  TEST_ASSERT_FALSE(rawMeshLineAllowed("m 2x"));
  TEST_ASSERT_TRUE(rawMeshLineAllowed("  m 2"));
}

void test_raw_mesh_line_other_commands() {
  TEST_ASSERT_TRUE(rawMeshLineAllowed("t 03-Rings.bin"));
  TEST_ASSERT_TRUE(rawMeshLineAllowed("get_playinfo"));
  TEST_ASSERT_TRUE(rawMeshLineAllowed("k 3"));
  TEST_ASSERT_FALSE(rawMeshLineAllowed("v"));
  TEST_ASSERT_FALSE(rawMeshLineAllowed("u 03-Rings.bin"));
  TEST_ASSERT_FALSE(rawMeshLineAllowed("mm 1"));
  TEST_ASSERT_FALSE(rawMeshLineAllowed(""));
  TEST_ASSERT_FALSE(rawMeshLineAllowed(nullptr));
}

// Unsolicited MCU output (0.9.2): only complete "mculog:" lines are kept,
// printable, in arrival order, at most McuLogLines::QUEUE waiting.
static std::string takeLog(McuLogLines& m) {
  char out[McuLogLines::MAX_CHARS + 1];
  return m.take(out, sizeof(out)) ? std::string(out) : std::string("<none>");
}
static void feedStr(McuLogLines& m, const char* s) { m.feed((const uint8_t*)s, strlen(s)); }

void test_mcu_log_keeps_mculog_lines() {
  McuLogLines m;
  feedStr(m, "mculog:play file.\n");
  TEST_ASSERT_EQUAL_STRING("mculog:play file.", takeLog(m).c_str());
  TEST_ASSERT_EQUAL_STRING("<none>", takeLog(m).c_str());
}

void test_mcu_log_line_split_across_reads() {
  McuLogLines m;
  feedStr(m, "mcu");
  TEST_ASSERT_EQUAL_STRING("<none>", takeLog(m).c_str());
  feedStr(m, "log:abc\r\n");
  TEST_ASSERT_EQUAL_STRING("mculog:abc", takeLog(m).c_str());
}

void test_mcu_log_drops_replies_and_noise() {
  McuLogLines m;
  feedStr(m, "PlayInfo \"04-Beeps.bin\" playing 1 2\nsetting on all 2 2 164 F-S12\n03-Rings.bin\n\x18\xc0\x95\n");
  TEST_ASSERT_EQUAL_STRING("<none>", takeLog(m).c_str());
}

void test_mcu_log_sanitized_and_cut() {
  McuLogLines m;
  feedStr(m, "mculog:a\x01" "b^c\n");
  TEST_ASSERT_EQUAL_STRING("mculog:a.b.c", takeLog(m).c_str());
  std::string longLine = "mculog:" + std::string(100, 'x') + "\nmculog:next\n";
  feedStr(m, longLine.c_str());
  std::string first = takeLog(m);
  TEST_ASSERT_EQUAL(McuLogLines::MAX_CHARS, first.size());
  TEST_ASSERT_EQUAL_STRING("mculog:next", takeLog(m).c_str());
}

void test_mcu_log_queue_keeps_oldest_and_counts_drops() {
  McuLogLines m;
  for (int i = 0; i < McuLogLines::QUEUE + 2; i++) {
    std::string l = "mculog:" + std::to_string(i) + "\n";
    feedStr(m, l.c_str());
  }
  TEST_ASSERT_EQUAL_UINT32(2, m.dropped());
  for (int i = 0; i < McuLogLines::QUEUE; i++)
    TEST_ASSERT_EQUAL_STRING(("mculog:" + std::to_string(i)).c_str(), takeLog(m).c_str());
  TEST_ASSERT_EQUAL_STRING("<none>", takeLog(m).c_str());
}

// #HLFT<NN> (0.9.3): the first listed clip name starting with NN- or NN_,
// found while the MCU's listing streams in.
static std::string slotOf(int nn, std::initializer_list<const char*> pieces, bool finish = true) {
  SlotFinder f(nn);
  for (const char* p : pieces) f.feed(p, strlen(p));
  if (finish) f.finish();
  return f.found() ? std::string(f.name()) : std::string("<none>");
}

void test_slot_finds_dash_and_underscore() {
  TEST_ASSERT_EQUAL_STRING("03-Rings.bin", slotOf(3, {"DCIM recv file list\n01-Orient.bin\n02-SW130.bin\n03-Rings.bin\n04-Beeps.bin\n"}).c_str());
  TEST_ASSERT_EQUAL_STRING("07_Vader.bin", slotOf(7, {"06-A.bin\n07_Vader.bin\n"}).c_str());
  TEST_ASSERT_EQUAL_STRING("00-Zero.bin", slotOf(0, {"00-Zero.bin\n"}).c_str());
}

void test_slot_first_match_wins() {
  TEST_ASSERT_EQUAL_STRING("03-A.bin", slotOf(3, {"03-A.bin\n03_B.bin\n"}).c_str());
}

void test_slot_needs_exactly_two_digits() {
  TEST_ASSERT_EQUAL_STRING("<none>", slotOf(3, {"30-X.bin\n003-X.bin\n3-X.bin\n13-X.bin\n03X.bin\n"}).c_str());
  TEST_ASSERT_EQUAL_STRING("<none>", slotOf(5, {"01-A.bin\n02-B.bin\n"}).c_str());
}

void test_slot_split_reads_cr_and_last_line() {
  TEST_ASSERT_EQUAL_STRING("12-Split.bin", slotOf(12, {"11-A.bin\r\n1", "2-Sp", "lit.bin\r\n"}).c_str());
  TEST_ASSERT_EQUAL_STRING("12-Last.bin", slotOf(12, {"11-A.bin\n12-Last.bin"}).c_str());   // no LF: finish()
}

void test_slot_skips_names_unsafe_to_send() {
  TEST_ASSERT_EQUAL_STRING("<none>", slotOf(4, {"04-No ext\n04-Bad^x.bin\n"}).c_str());
  TEST_ASSERT_EQUAL_STRING("04-Ok.bin", slotOf(4, {"04-No ext\n04-Ok.bin\n"}).c_str());
  std::string longName = "04-" + std::string(70, 'x') + ".bin\n04-Short.bin\n";
  TEST_ASSERT_EQUAL_STRING("04-Short.bin", slotOf(4, {longName.c_str()}).c_str());
}

// 0.9.8 review: only a raw line that can change the MCU's settings or clip
// makes the UNPROMPTED guard forget its baseline; queries keep it.
// 0.9.8 restore: the same streamed listing scan, for one exact name.
void test_name_finder_matches_exactly() {
  SlotFinder f("04-Beeps.bin");
  const char* list = "01-Orient.bin\r\n04-Beeps.bin.old\n04-Beeps.bin\n05-SW078.bin\n";
  f.feed(list, 10);
  f.feed(list + 10, strlen(list) - 10);
  f.finish();
  TEST_ASSERT_TRUE(f.found());
  SlotFinder g("04-Beeps.bin");
  g.feed("04-Beeps.bi", 11);
  g.finish();
  TEST_ASSERT_FALSE(g.found());
  SlotFinder h("04-Beeps.bin");
  h.feed("04-Beeps.bin", 12);                       // last line without a newline
  h.finish();
  TEST_ASSERT_TRUE(h.found());
}

void test_raw_line_may_change() {
  TEST_ASSERT_FALSE(rawLineMayChange("get_setting"));
  TEST_ASSERT_FALSE(rawLineMayChange("  get_playinfo"));
  TEST_ASSERT_FALSE(rawLineMayChange("s"));
  TEST_ASSERT_FALSE(rawLineMayChange("md5 03-Rings.bin"));
  TEST_ASSERT_TRUE(rawLineMayChange("m 1"));
  TEST_ASSERT_TRUE(rawLineMayChange("t 03-Rings.bin"));
  TEST_ASSERT_TRUE(rawLineMayChange("i"));
  TEST_ASSERT_TRUE(rawLineMayChange("d"));
  TEST_ASSERT_TRUE(rawLineMayChange("get_settingx"));     // not exactly a query: assume it changes
  TEST_ASSERT_TRUE(rawLineMayChange(""));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_raw_line_may_change);
  RUN_TEST(test_name_finder_matches_exactly);
  RUN_TEST(test_slot_finds_dash_and_underscore);
  RUN_TEST(test_slot_first_match_wins);
  RUN_TEST(test_slot_needs_exactly_two_digits);
  RUN_TEST(test_slot_split_reads_cr_and_last_line);
  RUN_TEST(test_slot_skips_names_unsafe_to_send);
  RUN_TEST(test_mcu_log_keeps_mculog_lines);
  RUN_TEST(test_mcu_log_line_split_across_reads);
  RUN_TEST(test_mcu_log_drops_replies_and_noise);
  RUN_TEST(test_mcu_log_sanitized_and_cut);
  RUN_TEST(test_mcu_log_queue_keeps_oldest_and_counts_drops);
  RUN_TEST(test_raw_mesh_line_volume_levels);
  RUN_TEST(test_raw_mesh_line_other_commands);
  RUN_TEST(test_build_plain_and_with_args);
  RUN_TEST(test_build_command_with_digits);
  RUN_TEST(test_v_is_never_built);
  RUN_TEST(test_args_cannot_break_framing);
  RUN_TEST(test_bad_commands_refused);
  RUN_TEST(test_too_long_refused);
  RUN_TEST(test_clip_names);
  RUN_TEST(test_parse_setting);
  RUN_TEST(test_parse_setting_rejects_partial);
  RUN_TEST(test_parse_playinfo);
  RUN_TEST(test_parse_playinfo_quoted_name_with_space);
  RUN_TEST(test_parse_playinfo_empty_name);
  RUN_TEST(test_parse_playinfo_rejects_partial);
  RUN_TEST(test_parse_list);
  RUN_TEST(test_parse_list_caps_at_catalog_max);
  RUN_TEST(test_raw_mesh_allowlist);
  return UNITY_END();
}
