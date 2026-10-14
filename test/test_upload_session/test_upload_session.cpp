#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "UploadSession.h"

using namespace fanlogic;

void setUp() {}
void tearDown() {}

void test_query_open_ok() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-Rings.bin&size=6134400", q));
  TEST_ASSERT_EQUAL_STRING("03-Rings.bin", q.name);
  TEST_ASSERT_EQUAL_UINT32(6134400, (uint32_t)q.size);
  TEST_ASSERT_TRUE(q.spi == SpiConfig());
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "size=100&clk=xtal&name=03-R.bin&mhz=10", q));
  TEST_ASSERT_EQUAL_UINT32(10000000, q.spi.hz);
  TEST_ASSERT_EQUAL(SpiClk::XTAL, q.spi.clk);
}

// Timing knobs: pace, igap, keep, pre, dry (0.8.2 defaults: reserve, keep 2500,
// pre 0 = full, dry 4000 for reserve and 30000 otherwise).
void test_query_open_timing_knobs() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1", q));
  TEST_ASSERT_EQUAL(Pace::RESERVE, q.pace.pace);        // sessions default to reserve
  TEST_ASSERT_EQUAL_UINT32(0, q.pace.igapMs);
  TEST_ASSERT_EQUAL_UINT32(3000, q.pace.keepMs);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)q.pre);
  TEST_ASSERT_EQUAL_UINT32(4000, q.dryMs);
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN,
                    "name=03-R.bin&size=1&pace=burst&igap=60000&pre=617&dry=600000", q));
  TEST_ASSERT_EQUAL(Pace::BURST, q.pace.pace);
  TEST_ASSERT_EQUAL_UINT32(60000, q.pace.igapMs);
  TEST_ASSERT_EQUAL_UINT32(617, (uint32_t)q.pre);
  TEST_ASSERT_EQUAL_UINT32(600000, q.dryMs);
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN,
                    "dry=1000&pre=20&igap=0&pace=burst&size=1&name=03-R.bin", q));
  TEST_ASSERT_EQUAL(Pace::BURST, q.pace.pace);
  TEST_ASSERT_EQUAL_UINT32(20, (uint32_t)q.pre);
  TEST_ASSERT_EQUAL_UINT32(1000, q.dryMs);
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN,
                    "pace=reserve&keep=1000&dry=1001&size=1&name=03-R.bin", q));
  TEST_ASSERT_EQUAL(Pace::RESERVE, q.pace.pace);
  TEST_ASSERT_EQUAL_UINT32(1000, q.pace.keepMs);
  TEST_ASSERT_EQUAL_UINT32(1001, q.dryMs);
}

// Sessions refuse the 0.8.0 trickle: the MCU rejects a window spread over
// seconds (bench 2026-10-07), and a trickle waits for the body inside a
// window while the window gate keeps the chunk handler out (review 0.9.0).
void test_query_open_refuses_trickle() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&pace=trickle", q));
}

// The dry default follows the pace, whatever the key order.
void test_query_open_dry_default_follows_pace() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&pace=burst", q));
  TEST_ASSERT_EQUAL_UINT32(30000, q.dryMs);
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "pace=reserve&name=03-R.bin&size=1", q));
  TEST_ASSERT_EQUAL_UINT32(4000, q.dryMs);
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&pace=burst&dry=5000", q));
  TEST_ASSERT_EQUAL_UINT32(5000, q.dryMs);
}

void test_query_open_igap_must_be_below_dry() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&pace=burst&igap=30000", q));   // burst's default dry
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&pace=burst&igap=29999", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&igap=4000", q));   // reserve's default dry 4000
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&igap=3999", q));
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&igap=5000&dry=6000", q));
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&dry=6000&igap=5000", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&igap=6000&dry=6000", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&dry=6000&igap=6000", q));
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&igap=60000&dry=600000", q));
}

void test_query_open_keep_must_be_below_dry() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&keep=3999", q));   // default dry 4000
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&keep=4000", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&keep=5000&dry=5000", q));
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&dry=5001&keep=5000", q));
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&keep=10000&dry=10001", q));
  // keep is only meaningful for reserve: other paces accept it without the dry rule
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&pace=burst&keep=5000", q));
}

void test_query_open_timing_knobs_strict() {
  UploadQuery q;
  const char* bad[] = {
    "pace=fast", "pace=", "pace=burst&pace=burst", "pace=BURST", "pace=reserve&pace=reserve",
    "igap=60001", "igap=-1", "igap=", "igap=1&igap=1",
    "keep=999", "keep=10001", "keep=", "keep=-1", "keep=2000&keep=2000",
    "pre=19", "pre=618", "pre=0", "pre=20&pre=20",
    "dry=999", "dry=600001", "dry=", "dry=2000&dry=2000"};
  for (const char* k : bad) {
    char qs[96];
    snprintf(qs, sizeof(qs), "name=03-R.bin&size=1&%s", k);
    TEST_ASSERT_EQUAL_MESSAGE(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, qs, q), k);
  }
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::CHUNK, "id=1&off=0&pace=burst", q));   // OPEN only
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::CHUNK, "id=1&off=0&keep=2500", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::ABORT, "dry=2000", q));
}

// Review focus 4.
void test_query_open_name_decoding() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03%2DRings.bin&size=1", q));
  TEST_ASSERT_EQUAL_STRING("03-Rings.bin", q.name);
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=07-a%2Bb.bin&size=1", q));
  TEST_ASSERT_EQUAL_STRING("07-a+b.bin", q.name);
  TEST_ASSERT_EQUAL(QueryErr::NAME, parseUploadQuery(UploadReq::OPEN, "name=07-a%20b.bin&size=1", q));   // space
  TEST_ASSERT_EQUAL(QueryErr::NAME, parseUploadQuery(UploadReq::OPEN, "name=07-a%2Fb.bin&size=1", q));   // '/'
  TEST_ASSERT_EQUAL(QueryErr::NAME, parseUploadQuery(UploadReq::OPEN, "name=Rings.bin&size=1", q));
  TEST_ASSERT_EQUAL(QueryErr::NAME, parseUploadQuery(UploadReq::OPEN, "name=07-a%00.bin&size=1", q));
  TEST_ASSERT_EQUAL(QueryErr::NAME, parseUploadQuery(UploadReq::OPEN, "size=1", q));
  TEST_ASSERT_EQUAL(QueryErr::NAME, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin?junk&size=1", q));   // literal '?'
}

void test_query_open_size() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::LENGTH, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin", q));
  TEST_ASSERT_EQUAL(QueryErr::LENGTH, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=0", q));
  TEST_ASSERT_EQUAL(QueryErr::LENGTH, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=-5", q));
  TEST_ASSERT_EQUAL(QueryErr::LENGTH, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=134217729", q));
  TEST_ASSERT_EQUAL(QueryErr::LENGTH, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=99999999999", q));
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=134217728", q));
}

void test_query_rejects_junk() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&x=1", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&size=2", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&mhz=30", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&id=5", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::CHUNK, "id=5&off=0&name=03-R.bin", q));
}

// 0.9.8: dark=1 makes the upload dark and silent (pace.dark); OPEN only.
void test_query_open_dark() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&dark=1", q));
  TEST_ASSERT_TRUE(q.pace.dark);
  UploadQuery d;
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1", d));
  TEST_ASSERT_FALSE(d.pace.dark);
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&dark=0", d));
  TEST_ASSERT_FALSE(d.pace.dark);
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&dark=2", d));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::OPEN, "name=03-R.bin&size=1&dark=1&dark=1", d));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::CHUNK, "id=1&off=0&dark=1", d));
}

void test_query_chunk() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::CHUNK, "id=123456&off=28800", q));
  TEST_ASSERT_EQUAL_UINT32(123456, q.id);
  TEST_ASSERT_TRUE(q.hasId);
  TEST_ASSERT_EQUAL_UINT32(28800, (uint32_t)q.off);
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::CHUNK, "off=0&id=1", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::CHUNK, "off=0", q));          // no id
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::CHUNK, "id=0&off=0", q));
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::CHUNK, "id=2147483648&off=0", q));
  TEST_ASSERT_EQUAL(QueryErr::LENGTH, parseUploadQuery(UploadReq::CHUNK, "id=1", q));          // no off
  TEST_ASSERT_EQUAL(QueryErr::LENGTH, parseUploadQuery(UploadReq::CHUNK, "id=1&off=x", q));
}

void test_query_abort() {
  UploadQuery q;
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::ABORT, nullptr, q));
  TEST_ASSERT_FALSE(q.hasId);
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::ABORT, "", q));
  TEST_ASSERT_EQUAL(QueryErr::OK, parseUploadQuery(UploadReq::ABORT, "id=7", q));
  TEST_ASSERT_TRUE(q.hasId);
  TEST_ASSERT_EQUAL_UINT32(7, q.id);
  TEST_ASSERT_EQUAL(QueryErr::QUERY, parseUploadQuery(UploadReq::ABORT, "id=7&off=1", q));
}

void test_query_err_names() {
  TEST_ASSERT_EQUAL_STRING("ok", queryErrName(QueryErr::OK));
  TEST_ASSERT_EQUAL_STRING("name", queryErrName(QueryErr::NAME));
  TEST_ASSERT_EQUAL_STRING("length", queryErrName(QueryErr::LENGTH));
  TEST_ASSERT_EQUAL_STRING("query", queryErrName(QueryErr::QUERY));
}

void test_open_decision() {
  TEST_ASSERT_EQUAL(OpenDecision::NEW, openDecision(false, "03-R.bin", 100, "03-R.bin", 100));
  TEST_ASSERT_EQUAL(OpenDecision::SAME, openDecision(true, "03-R.bin", 100, "03-R.bin", 100));
  TEST_ASSERT_EQUAL(OpenDecision::BUSY, openDecision(true, "03-R.bin", 100, "03-R.bin", 101));   // other size
  TEST_ASSERT_EQUAL(OpenDecision::BUSY, openDecision(true, "03-R.bin", 100, "04-R.bin", 100));
}

void test_json_string() {
  char out[32];
  TEST_ASSERT_TRUE(jsonString("03-a\"b\\c.bin", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("03-a\\\"b\\\\c.bin", out);
  TEST_ASSERT_TRUE(jsonString("a\nb", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("a.b", out);
  char small[4];
  TEST_ASSERT_FALSE(jsonString("abcd", small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("", small);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_query_open_dark);
  RUN_TEST(test_query_open_ok);
  RUN_TEST(test_query_open_timing_knobs);
  RUN_TEST(test_query_open_refuses_trickle);
  RUN_TEST(test_query_open_timing_knobs_strict);
  RUN_TEST(test_query_open_dry_default_follows_pace);
  RUN_TEST(test_query_open_igap_must_be_below_dry);
  RUN_TEST(test_query_open_keep_must_be_below_dry);
  RUN_TEST(test_query_open_name_decoding);
  RUN_TEST(test_query_open_size);
  RUN_TEST(test_query_rejects_junk);
  RUN_TEST(test_query_chunk);
  RUN_TEST(test_query_abort);
  RUN_TEST(test_query_err_names);
  RUN_TEST(test_open_decision);
  RUN_TEST(test_json_string);
  return UNITY_END();
}
