#include <unity.h>
#include <stddef.h>
#include <string.h>
#include "WcbPacket.h"

using namespace wcb;

void setUp() {}
void tearDown() {}

void test_layout_offsets() {
  TEST_ASSERT_EQUAL(252, sizeof(EtmPacket));
  TEST_ASSERT_EQUAL(0,   offsetof(EtmPacket, password));
  TEST_ASSERT_EQUAL(40,  offsetof(EtmPacket, senderId));
  TEST_ASSERT_EQUAL(44,  offsetof(EtmPacket, targetId));
  TEST_ASSERT_EQUAL(48,  offsetof(EtmPacket, commandIncluded));
  TEST_ASSERT_EQUAL(49,  offsetof(EtmPacket, command));
  TEST_ASSERT_EQUAL(249, offsetof(EtmPacket, packetType));
  TEST_ASSERT_EQUAL(250, offsetof(EtmPacket, seq));
}

void test_node_and_group_macs() {
  uint8_t m[6];
  const uint8_t node15[6] = {0x02, 0xD3, 0x3B, 0x00, 0x00, 0x0F};
  nodeMac(0xD3, 0x3B, 15, m);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(node15, m, 6);
  const uint8_t group[6] = {0xFF, 0xD3, 0x3B, 0xFF, 0xFF, 0xFF};
  groupMac(0xD3, 0x3B, m);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(group, m, 6);
}

void test_is_mesh_mac() {
  const uint8_t node2[6]  = {0x02, 0xD3, 0x3B, 0x00, 0x00, 0x02};
  const uint8_t group[6]  = {0xFF, 0xD3, 0x3B, 0xFF, 0xFF, 0xFF};
  const uint8_t other[6]  = {0x02, 0x5C, 0x57, 0x00, 0x00, 0x02};
  const uint8_t vendor[6] = {0x00, 0x00, 0x5E, 0x00, 0x53, 0x01};   // RFC 7042 documentation MAC
  TEST_ASSERT_TRUE(isMeshMac(node2, 0xD3, 0x3B));
  TEST_ASSERT_FALSE(isMeshMac(group, 0xD3, 0x3B));
  TEST_ASSERT_FALSE(isMeshMac(other, 0xD3, 0x3B));
  TEST_ASSERT_FALSE(isMeshMac(vendor, 0xD3, 0x3B));
}

void test_init_etm_golden_bytes() {
  EtmPacket p;
  memset(&p, 0xAA, sizeof(p));               // prove initEtm zeroes everything
  initEtm(p, "pw", 15, 0, PKT_HEARTBEAT, 0x1234);
  const uint8_t* b = (const uint8_t*)&p;
  TEST_ASSERT_EQUAL_UINT8('p', b[0]);
  TEST_ASSERT_EQUAL_UINT8('w', b[1]);
  TEST_ASSERT_EQUAL_UINT8(0,   b[2]);
  TEST_ASSERT_EQUAL_UINT8('1', b[40]);
  TEST_ASSERT_EQUAL_UINT8('5', b[41]);
  TEST_ASSERT_EQUAL_UINT8(0,   b[42]);
  TEST_ASSERT_EQUAL_UINT8('0', b[44]);
  TEST_ASSERT_EQUAL_UINT8(0,   b[45]);
  TEST_ASSERT_EQUAL_UINT8(0,   b[48]);       // commandIncluded
  TEST_ASSERT_EQUAL_UINT8(0,   b[49]);       // command empty
  TEST_ASSERT_EQUAL_UINT8(0,   b[248]);
  TEST_ASSERT_EQUAL_UINT8(PKT_HEARTBEAT, b[249]);
  TEST_ASSERT_EQUAL_UINT8(0x34, b[250]);     // seq little-endian
  TEST_ASSERT_EQUAL_UINT8(0x12, b[251]);
}

void test_init_etm_truncates_long_password() {
  EtmPacket p;
  char pw[60];
  memset(pw, 'x', sizeof(pw) - 1);
  pw[sizeof(pw) - 1] = '\0';
  initEtm(p, pw, 1, 2, PKT_COMMAND, 1);
  TEST_ASSERT_EQUAL(39, strnlen(p.password, sizeof(p.password)));
  TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)p.password[39]);
}

void test_parse_id() {
  const char a[4] = {'1', '5', 0, 0};
  const char b[4] = {'0', 0, 0, 0};
  const char c[4] = {'a', 'b', 'c', 0};
  const char d[4] = {'1', '2', '3', '4'};    // no NUL inside the field
  TEST_ASSERT_EQUAL(15, parseId(a));
  TEST_ASSERT_EQUAL(0, parseId(b));
  TEST_ASSERT_EQUAL(0, parseId(c));
  TEST_ASSERT_EQUAL(1234, parseId(d));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_layout_offsets);
  RUN_TEST(test_node_and_group_macs);
  RUN_TEST(test_is_mesh_mac);
  RUN_TEST(test_init_etm_golden_bytes);
  RUN_TEST(test_init_etm_truncates_long_password);
  RUN_TEST(test_parse_id);
  return UNITY_END();
}
