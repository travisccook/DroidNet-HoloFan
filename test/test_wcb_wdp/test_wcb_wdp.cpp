#include <unity.h>
#include <string.h>
#include "WcbWdp.h"

using namespace wcb;

void setUp() {}
void tearDown() {}

static Identity fan() {
  Identity id;
  strcpy(id.devType, "HoloFan");
  strcpy(id.fw, "1.0.0");
  strcpy(id.caps, "holo.fan");
  return id;
}

void test_advert_golden_bytes() {
  uint8_t buf[200];
  memset(buf, 0xEE, sizeof(buf));
  int n = buildAdvert(fan(), buf, sizeof(buf));
  const uint8_t expect[] = {
    'W', 0x01,
    0x0B, 7, 'H', 'o', 'l', 'o', 'F', 'a', 'n',
    0x03, 5, '1', '.', '0', '.', '0',
    0x0D, 8, 'h', 'o', 'l', 'o', '.', 'f', 'a', 'n',
    0x00};
  TEST_ASSERT_EQUAL(sizeof(expect), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, buf, sizeof(expect));
}

void test_advert_includes_hwrev_when_set() {
  Identity id = fan();
  strcpy(id.hwRev, "revA");
  uint8_t buf[200];
  int n = buildAdvert(id, buf, sizeof(buf));
  // DEVTYPE, FWVER, HWREV, CAPTAGS order
  TEST_ASSERT_EQUAL_UINT8(0x0C, buf[18]);
  TEST_ASSERT_EQUAL_UINT8(4, buf[19]);
  TEST_ASSERT_EQUAL_MEMORY("revA", &buf[20], 4);
  TEST_ASSERT_EQUAL_UINT8(0x0D, buf[24]);
  TEST_ASSERT_EQUAL_UINT8(0x00, buf[n - 1]);
}

void test_no_advert_without_devtype() {
  Identity id;
  uint8_t buf[200];
  TEST_ASSERT_EQUAL(0, buildAdvert(id, buf, sizeof(buf)));
}

void test_tlv_skipped_when_it_does_not_fit() {
  uint8_t buf[14];                                 // room for header + DEVTYPE only
  int n = buildAdvert(fan(), buf, sizeof(buf));
  TEST_ASSERT_EQUAL(12, n);                        // 2 + 2 + 7 + END
  TEST_ASSERT_EQUAL_UINT8(0x00, buf[11]);
}

void test_solicit_detected() {
  const uint8_t s[] = {'W', 0x01, 0x11, 0x00, 0x00};
  TEST_ASSERT_TRUE(isSolicit(s, sizeof(s)));
}

void test_solicit_after_other_tlvs() {
  const uint8_t s[] = {'W', 0x01, 0x0B, 2, 'X', 'Y', 0x11, 0x00, 0x00};
  TEST_ASSERT_TRUE(isSolicit(s, sizeof(s)));
}

void test_plain_advert_is_not_solicit() {
  uint8_t buf[200];
  int n = buildAdvert(fan(), buf, sizeof(buf));
  TEST_ASSERT_FALSE(isSolicit(buf, n));
}

void test_bad_magic_or_truncated_is_not_solicit() {
  const uint8_t a[] = {'X', 0x01, 0x11, 0x00};
  const uint8_t b[] = {'W', 0x01, 0x0B, 50, 'X'};  // TLV runs past the end
  TEST_ASSERT_FALSE(isSolicit(a, sizeof(a)));
  TEST_ASSERT_FALSE(isSolicit(b, sizeof(b)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_advert_golden_bytes);
  RUN_TEST(test_advert_includes_hwrev_when_set);
  RUN_TEST(test_no_advert_without_devtype);
  RUN_TEST(test_tlv_skipped_when_it_does_not_fit);
  RUN_TEST(test_solicit_detected);
  RUN_TEST(test_solicit_after_other_tlvs);
  RUN_TEST(test_plain_advert_is_not_solicit);
  RUN_TEST(test_bad_magic_or_truncated_is_not_solicit);
  return UNITY_END();
}
