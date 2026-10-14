#include <unity.h>
#include <string.h>
#include "NodeConfig.h"

using namespace nodecfg;

void setUp() {}
void tearDown() {}

static NodeConfig base() {
  NodeConfig c;
  setDefaults(c, 15, "HoloLifter");
  return c;
}

void test_defaults() {
  NodeConfig c = base();
  TEST_ASSERT_EQUAL(15, c.id);
  TEST_ASSERT_EQUAL_STRING("HoloLifter", c.alias);
  TEST_ASSERT_TRUE(c.replies);                         // replies broadcast by default
  TEST_ASSERT_EQUAL(3, c.floorQ);
  TEST_ASSERT_EQUAL(1, c.channel);
  TEST_ASSERT_TRUE(c.checksum);
  TEST_ASSERT_EQUAL_STRING("", c.password);          // mesh off until configured
  TEST_ASSERT_EQUAL_STRING("", c.profile[0].ssid);
}

void test_validators() {
  TEST_ASSERT_TRUE(validId(1)); TEST_ASSERT_TRUE(validId(19));
  TEST_ASSERT_FALSE(validId(0)); TEST_ASSERT_FALSE(validId(20));
  TEST_ASSERT_TRUE(validAlias("HoloFan")); TEST_ASSERT_TRUE(validAlias("Holo_Fan-2.b"));
  TEST_ASSERT_FALSE(validAlias("")); TEST_ASSERT_FALSE(validAlias("2Fan"));
  TEST_ASSERT_FALSE(validAlias("Holo Fan")); TEST_ASSERT_FALSE(validAlias("Holo,Fan"));
  TEST_ASSERT_FALSE(validAlias("Holo]Fan")); TEST_ASSERT_FALSE(validAlias("abcdefghijklmnopqrstuvwxy"));  // 25
  TEST_ASSERT_TRUE(validChannel(1)); TEST_ASSERT_TRUE(validChannel(11)); TEST_ASSERT_FALSE(validChannel(12));
  TEST_ASSERT_TRUE(validSsid("My Home Net")); TEST_ASSERT_FALSE(validSsid(""));
  TEST_ASSERT_TRUE(validPass("")); TEST_ASSERT_TRUE(validPass("12345678")); TEST_ASSERT_FALSE(validPass("1234567"));
  TEST_ASSERT_TRUE(validPassword("Jq6SECq8dsNRmq")); TEST_ASSERT_FALSE(validPassword(""));
  TEST_ASSERT_FALSE(validPassword("a^b")); TEST_ASSERT_FALSE(validPassword("a,b"));
}

void test_hostname() {
  char h[32];
  hostnameFromAlias("HoloFan", h, sizeof(h));     TEST_ASSERT_EQUAL_STRING("holofan", h);
  hostnameFromAlias("Holo_Lift.2", h, sizeof(h)); TEST_ASSERT_EQUAL_STRING("holo-lift-2", h);
}

void test_hex_decode() {
  char out[33];
  TEST_ASSERT_TRUE(hexDecode("4D7920486F6D65", out, sizeof(out)));  TEST_ASSERT_EQUAL_STRING("My Home", out);
  TEST_ASSERT_TRUE(hexDecode("4d79", out, sizeof(out)));            TEST_ASSERT_EQUAL_STRING("My", out);
  TEST_ASSERT_TRUE(hexDecode("", out, sizeof(out)));                TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_FALSE(hexDecode("4D7", out, sizeof(out)));            // odd
  TEST_ASSERT_FALSE(hexDecode("4G", out, sizeof(out)));             // not hex
  TEST_ASSERT_FALSE(hexDecode("4100", out, sizeof(out)));           // embedded NUL
  char small[3];
  TEST_ASSERT_FALSE(hexDecode("414243", small, sizeof(small)));     // does not fit
}

void test_parse_z_commands() {
  ZCmd z;
  TEST_ASSERT_TRUE(parseZ("#HLZP1", z));  TEST_ASSERT_EQUAL((int)ZType::REPLY, (int)z.type); TEST_ASSERT_EQUAL(1, z.num);
  TEST_ASSERT_TRUE(parseZ("#HLZP0", z));  TEST_ASSERT_EQUAL(0, z.num);
  TEST_ASSERT_TRUE(parseZ("#HLZI16", z)); TEST_ASSERT_EQUAL((int)ZType::ID, (int)z.type); TEST_ASSERT_EQUAL(16, z.num);
  TEST_ASSERT_TRUE(parseZ("#HLZNHoloFan", z)); TEST_ASSERT_EQUAL_STRING("HoloFan", z.text);
  TEST_ASSERT_TRUE(parseZ("#HLZKS14D79", z)); TEST_ASSERT_EQUAL((int)ZType::SSID, (int)z.type);
  TEST_ASSERT_EQUAL(1, z.num); TEST_ASSERT_EQUAL_STRING("My", z.text);
  TEST_ASSERT_TRUE(parseZ("#HLZKP23132333435363738", z)); TEST_ASSERT_EQUAL((int)ZType::PASS, (int)z.type);
  TEST_ASSERT_EQUAL(2, z.num); TEST_ASSERT_EQUAL_STRING("12345678", z.text);
  TEST_ASSERT_TRUE(parseZ("#HLZKP1", z)); TEST_ASSERT_EQUAL_STRING("", z.text);   // open network
  TEST_ASSERT_TRUE(parseZ("#HLZX2", z));  TEST_ASSERT_EQUAL((int)ZType::MAINT, (int)z.type); TEST_ASSERT_EQUAL(2, z.num);
  TEST_ASSERT_TRUE(parseZ("#HLZQ", z));   TEST_ASSERT_EQUAL((int)ZType::QUERY, (int)z.type);
}

void test_parse_z_rejects() {
  ZCmd z;
  const char* bad[] = {"#HLZ", "#HLZP2", "#HLZP", "#HLZPx", "#HLZI0", "#HLZI20", "#HLZN", "#HLZN2Fan",
                       "#HLZN Fan", "#HLZKS3414243", "#HLZKX14142", "#HLZKS1", "#HLZKP131", "#HLZKS1ZZ",
                       "#HLZX0", "#HLZX3", "#HLZQ1", "#HLZW", "#HLU"};
  for (const char* b : bad) TEST_ASSERT_FALSE_MESSAGE(parseZ(b, z), b);
}

void test_apply_z() {
  NodeConfig c = base();
  ZCmd z;
  parseZ("#HLZP0", z);         TEST_ASSERT_EQUAL((int)ZResult::SAVED, (int)applyZ(z, c));        TEST_ASSERT_FALSE(c.replies);
  parseZ("#HLZP1", z);         applyZ(z, c);                                                     TEST_ASSERT_TRUE(c.replies);
  parseZ("#HLZI14", z);        TEST_ASSERT_EQUAL((int)ZResult::SAVED_REBOOT, (int)applyZ(z, c)); TEST_ASSERT_EQUAL(14, c.id);
  parseZ("#HLZNLift2", z);     TEST_ASSERT_EQUAL((int)ZResult::SAVED_REBOOT, (int)applyZ(z, c)); TEST_ASSERT_EQUAL_STRING("Lift2", c.alias);
  parseZ("#HLZKS24D79", z);    applyZ(z, c);  TEST_ASSERT_EQUAL_STRING("My", c.profile[1].ssid);
  parseZ("#HLZKP23132333435363738", z); applyZ(z, c); TEST_ASSERT_EQUAL_STRING("12345678", c.profile[1].pass);
  parseZ("#HLZX1", z);         TEST_ASSERT_EQUAL((int)ZResult::MAINT, (int)applyZ(z, c));
  parseZ("#HLZQ", z);          TEST_ASSERT_EQUAL((int)ZResult::QUERY, (int)applyZ(z, c));
}

void test_node_reply_fits_one_mesh_command() {
  NodeConfig c = base();
  strcpy(c.alias, "abcdefghijklmnopqrstuvwx");     // 24
  NodeRuntime rt;
  rt.fw = "10.20.30-rc1"; rt.slot = "app1"; rt.v2 = true; rt.meshUp = true;
  rt.rx = rt.rej = rt.crc = rt.dup = rt.retry = rt.gaveUp = 4294967295u;
  char out[200];
  int n = formatNodeReply(c, rt, out, sizeof(out));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE_MESSAGE(n + 4 <= 187, out);
  TEST_ASSERT_EQUAL_STRING("#HL:NODE id=15 name=abcdefghijklmnopqrstuvwx reply=on ch=1 q=3 v2=1 up=1 rx=4294967295 rej=4294967295 crc=4294967295 dup=4294967295 rt=4294967295 gu=4294967295", out);
}

void test_usb_parse_and_apply() {
  NodeConfig c = base();
  UsbCmd u; char err[96];
  TEST_ASSERT_TRUE(parseUsb("mesh pw Jq6SECq8dsNRmq", u, err, sizeof(err)));
  TEST_ASSERT_TRUE(applyUsb(u, c)); TEST_ASSERT_EQUAL_STRING("Jq6SECq8dsNRmq", c.password);   // case kept
  TEST_ASSERT_TRUE(parseUsb("mesh oct D3 3b", u, err, sizeof(err))); applyUsb(u, c);
  TEST_ASSERT_EQUAL_HEX8(0xD3, c.oct2); TEST_ASSERT_EQUAL_HEX8(0x3B, c.oct3);
  TEST_ASSERT_TRUE(parseUsb("mesh ch 6", u, err, sizeof(err)));   applyUsb(u, c); TEST_ASSERT_EQUAL(6, c.channel);
  TEST_ASSERT_TRUE(parseUsb("mesh q 4", u, err, sizeof(err)));    applyUsb(u, c); TEST_ASSERT_EQUAL(4, c.floorQ);
  TEST_ASSERT_TRUE(parseUsb("mesh crc off", u, err, sizeof(err))); applyUsb(u, c); TEST_ASSERT_FALSE(c.checksum);
  TEST_ASSERT_TRUE(parseUsb("mesh id 17", u, err, sizeof(err)));  applyUsb(u, c); TEST_ASSERT_EQUAL(17, c.id);
  TEST_ASSERT_TRUE(parseUsb("mesh name LiftB", u, err, sizeof(err))); applyUsb(u, c); TEST_ASSERT_EQUAL_STRING("LiftB", c.alias);
  TEST_ASSERT_TRUE(parseUsb("mesh reply off", u, err, sizeof(err))); applyUsb(u, c); TEST_ASSERT_FALSE(c.replies);
  TEST_ASSERT_TRUE(parseUsb("mesh reply on", u, err, sizeof(err)));  applyUsb(u, c); TEST_ASSERT_TRUE(c.replies);
  TEST_ASSERT_TRUE(parseUsb("mesh ssid 1 My Home Net", u, err, sizeof(err))); applyUsb(u, c);
  TEST_ASSERT_EQUAL_STRING("My Home Net", c.profile[0].ssid);
  TEST_ASSERT_TRUE(parseUsb("mesh pass 1 correct horse battery", u, err, sizeof(err))); applyUsb(u, c);
  TEST_ASSERT_EQUAL_STRING("correct horse battery", c.profile[0].pass);
  TEST_ASSERT_TRUE(parseUsb("mesh pass 2", u, err, sizeof(err))); applyUsb(u, c);
  TEST_ASSERT_EQUAL_STRING("", c.profile[1].pass);
}

void test_usb_non_config_commands() {
  UsbCmd u; char err[96]; NodeConfig c = base();
  TEST_ASSERT_TRUE(parseUsb("mesh", u, err, sizeof(err)));         TEST_ASSERT_EQUAL((int)UsbType::SHOW, (int)u.type);
  TEST_ASSERT_TRUE(parseUsb("mesh show", u, err, sizeof(err)));    TEST_ASSERT_EQUAL((int)UsbType::SHOW, (int)u.type);
  TEST_ASSERT_TRUE(parseUsb("mesh maint 2", u, err, sizeof(err))); TEST_ASSERT_EQUAL((int)UsbType::MAINT, (int)u.type);
  TEST_ASSERT_EQUAL(2, u.a);
  TEST_ASSERT_TRUE(parseUsb("mesh restart", u, err, sizeof(err))); TEST_ASSERT_EQUAL((int)UsbType::RESTART, (int)u.type);
  TEST_ASSERT_FALSE(applyUsb(u, c));
}

void test_usb_off_and_clear() {
  UsbCmd u; char err[96]; NodeConfig c = base();
  strcpy(c.password, "secretpw");
  strcpy(c.profile[0].ssid, "Home"); strcpy(c.profile[0].pass, "wifisecret");
  strcpy(c.profile[1].ssid, "Shop"); strcpy(c.profile[1].pass, "shopsecret");
  TEST_ASSERT_TRUE(parseUsb("mesh off", u, err, sizeof(err)));
  TEST_ASSERT_EQUAL((int)UsbType::MESH_OFF, (int)u.type);
  TEST_ASSERT_TRUE(applyUsb(u, c)); TEST_ASSERT_EQUAL_STRING("", c.password);
  TEST_ASSERT_TRUE(parseUsb("mesh clear 2", u, err, sizeof(err)));
  TEST_ASSERT_EQUAL((int)UsbType::CLEAR, (int)u.type);
  TEST_ASSERT_TRUE(applyUsb(u, c));
  TEST_ASSERT_EQUAL_STRING("", c.profile[1].ssid); TEST_ASSERT_EQUAL_STRING("", c.profile[1].pass);
  TEST_ASSERT_EQUAL_STRING("Home", c.profile[0].ssid); TEST_ASSERT_EQUAL_STRING("wifisecret", c.profile[0].pass);
  const char* bad[] = {"mesh clear 3", "mesh clear"};
  for (const char* b : bad) {
    err[0] = '\0';
    TEST_ASSERT_FALSE_MESSAGE(parseUsb(b, u, err, sizeof(err)), b);
    TEST_ASSERT_TRUE_MESSAGE(err[0] != '\0', b);
  }
}

void test_usb_rejects_with_usage() {
  UsbCmd u; char err[96];
  const char* bad[] = {"mesh ch 12", "mesh ch", "mesh q 21", "mesh id 20", "mesh reply 2", "mesh reply", "mesh port 2", "mesh oct D3",
                       "mesh oct D3 3B 00", "mesh oct ZZ 3B", "mesh crc maybe", "mesh pw", "mesh pw a^b",
                       "mesh name 9x", "mesh ssid 3 Net", "mesh ssid 1", "mesh pass 1 short", "mesh maint 0",
                       "mesh bogus", "meshy"};
  for (const char* b : bad) {
    err[0] = '\0';
    TEST_ASSERT_FALSE_MESSAGE(parseUsb(b, u, err, sizeof(err)), b);
    TEST_ASSERT_TRUE_MESSAGE(err[0] != '\0', b);
  }
}

void test_show_masks_password() {
  NodeConfig c = base();
  strcpy(c.password, "secret-pw");
  strcpy(c.profile[0].ssid, "Home"); strcpy(c.profile[0].pass, "wifisecret");
  NodeRuntime rt; rt.fw = "2.0.0"; rt.slot = "app0";
  char out[600];
  TEST_ASSERT_TRUE(formatShow(c, rt, out, sizeof(out)) > 0);
  TEST_ASSERT_NULL(strstr(out, "secret-pw"));
  TEST_ASSERT_NULL(strstr(out, "wifisecret"));
  TEST_ASSERT_NOT_NULL(strstr(out, "wifi1=Home (pass set)"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_defaults);
  RUN_TEST(test_validators);
  RUN_TEST(test_hostname);
  RUN_TEST(test_hex_decode);
  RUN_TEST(test_parse_z_commands);
  RUN_TEST(test_parse_z_rejects);
  RUN_TEST(test_apply_z);
  RUN_TEST(test_node_reply_fits_one_mesh_command);
  RUN_TEST(test_usb_parse_and_apply);
  RUN_TEST(test_usb_non_config_commands);
  RUN_TEST(test_usb_rejects_with_usage);
  RUN_TEST(test_usb_off_and_clear);
  RUN_TEST(test_show_masks_password);
  return UNITY_END();
}
