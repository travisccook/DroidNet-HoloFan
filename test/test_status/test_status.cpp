#include <unity.h>
#include "command.h"
#include <stdio.h>
#include <string.h>

using namespace holofan;

void setUp() {}
void tearDown() {}

void test_ready_line() {
  Status s;
  s.state = FanState::READY; s.fileIndex = 3;
  strcpy(s.fileName, "04-Name.bin"); s.play = Play::PLAYING;
  s.progress = 47; s.total = 88; s.brightness = 3; s.volume = 2;
  s.loopAll = true; s.switcher = 1;
  char out[200];
  int n = formatStatus(s, out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_EQUAL_STRING(
    "#HL:FAN st=READY idx=3 name=04-Name.bin play=ing prog=47 total=88 br=3 vol=2 loop=all sw=on err=none",
    out);
}

void test_nomcu_defaults() {
  Status s;   // never heard from the MCU
  char out[200];
  formatStatus(s, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING(
    "#HL:FAN st=NOMCU idx=0 name=- play=? prog=0 total=0 br=0 vol=0 loop=all sw=? err=none", out);
}

void test_switcher_off_and_loop_one() {
  Status s;
  s.switcher = 0; s.loopAll = false; strcpy(s.err, "badindex");
  char out[200];
  formatStatus(s, out, sizeof(out));
  TEST_ASSERT_NOT_NULL(strstr(out, "loop=one sw=off err=badindex"));
}

// Longest possible status (24-char name, 10-digit numbers, longest err) fits one mesh command.
void test_status_worst_case_fits() {
  Status s;
  s.state = FanState::NOMCU; s.fileIndex = 2147483647;
  memset(s.fileName, 'x', 24); s.fileName[24] = '\0';
  s.play = Play::IDLE; s.progress = -2147483647 - 1; s.total = -2147483647 - 1;
  s.brightness = -2147483647 - 1; s.volume = -2147483647 - 1;
  memset(s.err, 'e', 15); s.err[15] = '\0';
  char out[300];
  int n = formatStatus(s, out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_LESS_OR_EQUAL((int)REPLY_MAX, n);
}

void test_status_too_small_buffer() {
  Status s;
  char out[20];
  TEST_ASSERT_EQUAL(-1, formatStatus(s, out, sizeof(out)));
}

void test_catalog_line() {
  Catalog c;
  c.count = 2;
  strcpy(c.entries[0].name, "02-Name.bin");
  strcpy(c.entries[1].name, "03-Name.bin");
  char out[200];
  formatCatalog(c, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("#HL:FANDCIM n=2 1=02-Name.bin 2=03-Name.bin", out);
}

void test_catalog_empty() {
  Catalog c;
  char out[200];
  formatCatalog(c, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("#HL:FANDCIM n=0", out);
}

// 32 clips never fit: the line stops at the last whole entry, n stays the full count.
void test_catalog_truncates_to_one_mesh_command() {
  Catalog c;
  c.count = CATALOG_MAX;
  for (int i = 0; i < CATALOG_MAX; ++i) snprintf(c.entries[i].name, sizeof(c.entries[i].name), "%02d-Clip.bin", i + 2);
  char out[400];
  int n = formatCatalog(c, out, sizeof(out));
  TEST_ASSERT_LESS_OR_EQUAL((int)REPLY_MAX, n);
  TEST_ASSERT_EQUAL((int)strlen(out), n);
  TEST_ASSERT_EQUAL_STRING_LEN("#HL:FANDCIM n=32 1=02-Clip.bin", out, 30);
  const char* last = strrchr(out, ' ');
  TEST_ASSERT_NOT_NULL(strstr(last, "-Clip.bin"));   // no half entry at the end
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ready_line);
  RUN_TEST(test_nomcu_defaults);
  RUN_TEST(test_switcher_off_and_loop_one);
  RUN_TEST(test_status_worst_case_fits);
  RUN_TEST(test_status_too_small_buffer);
  RUN_TEST(test_catalog_line);
  RUN_TEST(test_catalog_empty);
  RUN_TEST(test_catalog_truncates_to_one_mesh_command);
  return UNITY_END();
}
