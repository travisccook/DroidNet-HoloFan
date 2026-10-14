#include <unity.h>
#include "ClipOps.h"

using namespace fanlogic;

void setUp() {}
void tearDown() {}

// PUT /api/clips/<name>?mhz=<10|20|40>&clk=<pll|xtal> (0.7.2, bench
// experiments): the query string into the upload's SPI clock.

static SpiConfig parsed(const char* q) {
  SpiConfig c;
  TEST_ASSERT_TRUE_MESSAGE(parseSpiQuery(q, c), q ? q : "(null)");
  return c;
}

static void rejected(const char* q) {
  SpiConfig c;
  c.hz = 123;                                  // must stay untouched
  c.clk = SpiClk::XTAL;
  TEST_ASSERT_FALSE_MESSAGE(parseSpiQuery(q, c), q);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(123, c.hz, q);
  TEST_ASSERT_TRUE_MESSAGE(c.clk == SpiClk::XTAL, q);
}

// No parameters: 40 MHz from the PLL, exactly as before 0.7.2.
void test_no_query_is_40_mhz_pll() {
  SpiConfig d;
  TEST_ASSERT_EQUAL_UINT32(40000000, d.hz);
  TEST_ASSERT_TRUE(d.clk == SpiClk::PLL);
  TEST_ASSERT_TRUE(parsed(nullptr) == d);
  TEST_ASSERT_TRUE(parsed("") == d);
  TEST_ASSERT_TRUE(parsed("#frag") == d);
}

// A stale value in `out` never leaks into a parameterless upload.
void test_defaults_overwrite_out() {
  SpiConfig c;
  c.hz = 10000000;
  c.clk = SpiClk::XTAL;
  TEST_ASSERT_TRUE(parseSpiQuery("", c));
  TEST_ASSERT_TRUE(c == SpiConfig());
}

void test_mhz_values() {
  TEST_ASSERT_EQUAL_UINT32(10000000, parsed("mhz=10").hz);
  TEST_ASSERT_EQUAL_UINT32(20000000, parsed("mhz=20").hz);
  TEST_ASSERT_EQUAL_UINT32(40000000, parsed("mhz=40").hz);
  TEST_ASSERT_TRUE(parsed("mhz=20").clk == SpiClk::PLL);
}

void test_clk_values() {
  TEST_ASSERT_TRUE(parsed("clk=xtal").clk == SpiClk::XTAL);
  TEST_ASSERT_TRUE(parsed("clk=pll").clk == SpiClk::PLL);
  TEST_ASSERT_EQUAL_UINT32(40000000, parsed("clk=xtal").hz);
}

void test_both_in_any_order() {
  SpiConfig a = parsed("mhz=20&clk=xtal");
  TEST_ASSERT_EQUAL_UINT32(20000000, a.hz);
  TEST_ASSERT_TRUE(a.clk == SpiClk::XTAL);
  SpiConfig b = parsed("clk=xtal&mhz=10");
  TEST_ASSERT_EQUAL_UINT32(10000000, b.hz);
  TEST_ASSERT_TRUE(b.clk == SpiClk::XTAL);
}

// The query ends at a '#' (a fragment), as in the HTTP URL grammar.
void test_stops_at_fragment() {
  SpiConfig c = parsed("mhz=20#clk=xtal");
  TEST_ASSERT_EQUAL_UINT32(20000000, c.hz);
  TEST_ASSERT_TRUE(c.clk == SpiClk::PLL);
}

void test_rejects_other_mhz() {
  rejected("mhz=");
  rejected("mhz=30");
  rejected("mhz=4");
  rejected("mhz=400");
  rejected("mhz=040");
  rejected("mhz=40.0");
  rejected("mhz=40 ");
  rejected("mhz=%34%30");                      // no percent-decoding
  rejected("mhz=40000000");
}

void test_rejects_other_clk() {
  rejected("clk=");
  rejected("clk=PLL");
  rejected("clk=XTAL");
  rejected("clk=xtal2");
  rejected("clk=apb");
  rejected("clk=default");
}

// A typo must not silently run at the default clock.
void test_rejects_unknown_keys_and_malformed_pairs() {
  rejected("MHZ=20");
  rejected("freq=20");
  rejected("mhz");
  rejected("=20");
  rejected("&");
  rejected("mhz=20&");
  rejected("&mhz=20");
  rejected("mhz=20&&clk=pll");
  rejected("mhz=20&clk=pll&x=1");
  rejected("mhz=20=20");
}

void test_rejects_repeated_keys() {
  rejected("mhz=20&mhz=20");
  rejected("mhz=20&mhz=40");
  rejected("clk=pll&clk=xtal");
}

void test_clk_names() {
  TEST_ASSERT_EQUAL_STRING("pll", spiClkName(SpiClk::PLL));
  TEST_ASSERT_EQUAL_STRING("xtal", spiClkName(SpiClk::XTAL));
}

// McuSpi re-adds its device only when the configuration differs.
void test_config_equality() {
  SpiConfig a, b;
  TEST_ASSERT_TRUE(a == b);
  TEST_ASSERT_FALSE(a != b);
  b.hz = 20000000;
  TEST_ASSERT_TRUE(a != b);
  b = SpiConfig();
  b.clk = SpiClk::XTAL;
  TEST_ASSERT_TRUE(a != b);
  TEST_ASSERT_FALSE(a == b);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_query_is_40_mhz_pll);
  RUN_TEST(test_defaults_overwrite_out);
  RUN_TEST(test_mhz_values);
  RUN_TEST(test_clk_values);
  RUN_TEST(test_both_in_any_order);
  RUN_TEST(test_stops_at_fragment);
  RUN_TEST(test_rejects_other_mhz);
  RUN_TEST(test_rejects_other_clk);
  RUN_TEST(test_rejects_unknown_keys_and_malformed_pairs);
  RUN_TEST(test_rejects_repeated_keys);
  RUN_TEST(test_clk_names);
  RUN_TEST(test_config_equality);
  return UNITY_END();
}
