#include <unity.h>
#include <string.h>
#include "FakePlatform.h"
#include "WcbCrc.h"
#include "WcbNode.h"

using namespace wcb;

static const uint8_t OCT2 = 0xD3, OCT3 = 0x3B;
static FakePlatform* plat;
static WcbNode* node;

static WcbConfig config(uint8_t id = 15) {
  WcbConfig c;
  c.oct2 = OCT2; c.oct3 = OCT3; c.id = id; c.floorQ = 3;
  strcpy(c.password, "testpw");
  c.checksum = true;
  strcpy(c.identity.devType, "HoloLifter");
  strcpy(c.identity.fw, "2.0.0");
  strcpy(c.identity.caps, "lift.motor");
  return c;
}

// Build a frame "from" node `sender` as a WCB would send it.
static EtmPacket frame(uint8_t sender, uint8_t target, uint8_t type, uint16_t seq,
                       const char* text = nullptr, bool crc = true, const char* pw = "testpw") {
  EtmPacket p;
  initEtm(p, pw, sender, target, type, seq);
  if (text) {
    p.commandIncluded = 1;
    if (crc) appendCrc(text, p.command, sizeof(p.command));
    else strncpy(p.command, text, sizeof(p.command) - 1);
  }
  return p;
}

static void deliver(const EtmPacket& p, uint8_t macId) {
  uint8_t mac[6];
  nodeMac(OCT2, OCT3, macId, mac);
  node->ingest(mac, (const uint8_t*)&p, sizeof(p));
}
static void deliver(const EtmPacket& p) { deliver(p, (uint8_t)parseId(p.senderId)); }

static bool popText(uint8_t& sender, char* buf) { return node->popCommand(sender, buf, 201); }

void setUp() {
  plat = new FakePlatform();
  node = new WcbNode();
  TEST_ASSERT_TRUE(node->begin(config(), *plat));
}
void tearDown() { delete node; delete plat; }

// ---- begin / config ---------------------------------------------------------

void test_begin_registers_floor_peers_and_group() {
  uint8_t m[6];
  for (uint8_t id = 1; id <= 3; id++) { nodeMac(OCT2, OCT3, id, m); TEST_ASSERT_TRUE(plat->hasPeer(m)); }
  groupMac(OCT2, OCT3, m);
  TEST_ASSERT_TRUE(plat->hasPeer(m));
  TEST_ASSERT_EQUAL(4, plat->peers.size());
}

void test_begin_skips_own_id_in_floor() {
  FakePlatform p2; WcbNode n2;
  WcbConfig c = config(2);
  TEST_ASSERT_TRUE(n2.begin(c, p2));
  uint8_t m[6];
  nodeMac(OCT2, OCT3, 2, m);
  TEST_ASSERT_FALSE(p2.hasPeer(m));
  TEST_ASSERT_EQUAL(3, p2.peers.size());           // 1, 3, group
}

void test_begin_rejects_bad_config() {
  FakePlatform p2; WcbNode n2;
  WcbConfig c = config(0);
  TEST_ASSERT_FALSE(n2.begin(c, p2));
  c = config(21);
  TEST_ASSERT_FALSE(n2.begin(c, p2));
  c = config(15); c.password[0] = '\0';
  TEST_ASSERT_FALSE(n2.begin(c, p2));
  c = config(15); c.floorQ = 21;
  TEST_ASSERT_FALSE(n2.begin(c, p2));
  TEST_ASSERT_FALSE(n2.started());
  TEST_ASSERT_FALSE(n2.send(2, "x"));
}

// ---- periodic traffic -------------------------------------------------------

void test_first_poll_sends_boot_and_heartbeat_to_group() {
  node->poll();
  auto boots = plat->ofType(PKT_BOOT);
  auto hbs = plat->ofType(PKT_HEARTBEAT);
  TEST_ASSERT_EQUAL(1, boots.size());
  TEST_ASSERT_EQUAL(1, hbs.size());
  uint8_t g[6]; groupMac(OCT2, OCT3, g);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(g, boots[0].mac, 6);
  TEST_ASSERT_EQUAL_STRING("15", boots[0].etm().senderId);
  TEST_ASSERT_EQUAL_STRING("0", boots[0].etm().targetId);
  TEST_ASSERT_EQUAL_STRING("testpw", hbs[0].etm().password);
  TEST_ASSERT_EQUAL(0, hbs[0].etm().seq);
}

void test_three_boot_announces_then_heartbeats_every_10s() {
  for (plat->now = 0; plat->now <= 30000; plat->now += 100) node->poll();
  TEST_ASSERT_EQUAL(3, plat->ofType(PKT_BOOT).size());
  TEST_ASSERT_EQUAL(4, plat->ofType(PKT_HEARTBEAT).size());   // 0, 10, 20, 30 s
}

void test_wdp_advert_bytes_and_burst() {
  for (plat->now = 0; plat->now <= 4000; plat->now += 100) node->poll();
  auto adv = plat->ofType(PKT_WDP);
  TEST_ASSERT_EQUAL(3, adv.size());
  const EtmPacket& p = adv[0].etm();
  TEST_ASSERT_EQUAL_UINT8(1, p.commandIncluded);
  uint8_t expect[200];
  int n = buildAdvert(config().identity, expect, sizeof(expect));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, (const uint8_t*)p.command, n);
  TEST_ASSERT_TRUE(strstr(p.command, "|CRC") == nullptr);    // WDP never carries a CRC
}

void test_no_advert_without_identity() {
  FakePlatform p2; WcbNode n2;
  WcbConfig c = config(); c.identity = Identity();
  n2.begin(c, p2);
  for (p2.now = 0; p2.now <= 70000; p2.now += 100) n2.poll();
  TEST_ASSERT_EQUAL(0, p2.ofType(PKT_WDP).size());
}

void test_solicit_triggers_advert_with_jitter() {
  for (plat->now = 0; plat->now <= 5000; plat->now += 100) node->poll();
  plat->now = 5000;
  size_t before = plat->ofType(PKT_WDP).size();
  EtmPacket s = frame(2, 0, PKT_WDP, 0);
  s.commandIncluded = 1;
  const uint8_t sol[] = {'W', 0x01, 0x11, 0x00, 0x00};
  memcpy(s.command, sol, sizeof(sol));
  deliver(s);
  node->poll();                                    // t = 5000: solicit armed for +600 ms
  plat->now = 5599; node->poll();
  TEST_ASSERT_EQUAL(before, plat->ofType(PKT_WDP).size());
  plat->now = 5600; node->poll();
  TEST_ASSERT_EQUAL(before + 1, plat->ofType(PKT_WDP).size());
}

// ---- receiving commands -----------------------------------------------------

void test_command_is_acked_and_delivered_without_crc() {
  deliver(frame(2, 15, PKT_COMMAND, 41, "#HLQ"));
  node->poll();
  auto acks = plat->ofType(PKT_ACK);
  TEST_ASSERT_EQUAL(1, acks.size());
  uint8_t m2[6]; nodeMac(OCT2, OCT3, 2, m2);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(m2, acks[0].mac, 6);
  TEST_ASSERT_EQUAL(41, acks[0].etm().seq);
  TEST_ASSERT_EQUAL_STRING("2", acks[0].etm().targetId);
  TEST_ASSERT_EQUAL_UINT8(0, acks[0].etm().commandIncluded);
  uint8_t sender; char buf[201];
  TEST_ASSERT_TRUE(popText(sender, buf));
  TEST_ASSERT_EQUAL(2, sender);
  TEST_ASSERT_EQUAL_STRING("#HLQ", buf);
  TEST_ASSERT_FALSE(popText(sender, buf));
}

void test_broadcast_command_is_accepted() {
  deliver(frame(2, 0, PKT_COMMAND, 5, "#HLS"));
  node->poll();
  uint8_t sender; char buf[201];
  TEST_ASSERT_TRUE(popText(sender, buf));
  TEST_ASSERT_EQUAL_STRING("#HLS", buf);
}

// Regression: duplicates are ACKed (the sender's retry must stop) but not re-run.
void test_duplicate_is_acked_but_not_delivered_twice() {
  deliver(frame(2, 15, PKT_COMMAND, 41, "#HLU"));
  deliver(frame(2, 15, PKT_COMMAND, 41, "#HLU"));
  node->poll();
  TEST_ASSERT_EQUAL(2, plat->ofType(PKT_ACK).size());
  uint8_t sender; char buf[201];
  TEST_ASSERT_TRUE(popText(sender, buf));
  TEST_ASSERT_FALSE(popText(sender, buf));
  TEST_ASSERT_EQUAL(1, node->counters().dupDrop);
}

// Regression: ACK before the CRC check, so a corrupt frame is not retried forever.
void test_bad_crc_is_acked_and_dropped() {
  EtmPacket p = frame(2, 15, PKT_COMMAND, 9, "#HLU");
  p.command[2] = 'X';                              // corrupt the body, keep the tag
  deliver(p);
  deliver(frame(2, 15, PKT_COMMAND, 10, "#HLU", false));   // no tag at all
  node->poll();
  TEST_ASSERT_EQUAL(2, plat->ofType(PKT_ACK).size());
  uint8_t sender; char buf[201];
  TEST_ASSERT_FALSE(popText(sender, buf));
  TEST_ASSERT_EQUAL(2, node->counters().crcFail);
}

void test_checksum_off_delivers_text_verbatim() {
  FakePlatform p2; WcbNode n2;
  WcbConfig c = config(); c.checksum = false;
  n2.begin(c, p2);
  EtmPacket p = frame(2, 15, PKT_COMMAND, 1, "note|CRC12345678", false);
  uint8_t m[6]; nodeMac(OCT2, OCT3, 2, m);
  n2.ingest(m, (const uint8_t*)&p, sizeof(p));
  n2.poll();
  uint8_t sender; char buf[201];
  TEST_ASSERT_TRUE(n2.popCommand(sender, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("note|CRC12345678", buf);
}

void test_gate_rejections() {
  deliver(frame(3, 15, PKT_COMMAND, 1, "#HLU"), 2);              // claims 3 from MAC ..:02
  deliver(frame(2, 15, PKT_COMMAND, 2, "#HLU", true, "wrong"));
  deliver(frame(2, 9, PKT_COMMAND, 3, "#HLU"));                 // for node 9
  deliver(frame(15, 15, PKT_COMMAND, 4, "#HLU"), 15);           // our own id
  deliver(frame(0, 15, PKT_COMMAND, 5, "#HLU"), 0);             // sender out of range
  EtmPacket p = frame(2, 15, PKT_COMMAND, 6, "#HLU");
  const uint8_t foreign[6] = {0x02, 0x5C, 0x57, 0x00, 0x00, 0x02};
  node->ingest(foreign, (const uint8_t*)&p, sizeof(p));
  node->ingest(foreign, (const uint8_t*)&p, 100);               // short frame, foreign too
  uint8_t m2[6]; nodeMac(OCT2, OCT3, 2, m2);
  node->ingest(m2, (const uint8_t*)&p, 204);                     // e.g. an RTERM-sized frame
  node->poll();
  WcbCounters c = node->counters();
  TEST_ASSERT_EQUAL(3, c.rejSpoof);
  TEST_ASSERT_EQUAL(1, c.rejPassword);
  TEST_ASSERT_EQUAL(1, c.rejTarget);
  TEST_ASSERT_EQUAL(2, c.rejMac);
  TEST_ASSERT_EQUAL(1, c.rejSize);
  TEST_ASSERT_EQUAL(0, c.rxOk);
  TEST_ASSERT_EQUAL(0, plat->ofType(PKT_ACK).size());
  uint8_t sender; char buf[201];
  TEST_ASSERT_FALSE(popText(sender, buf));
}

void test_json_broadcast_is_not_acked() {
  deliver(frame(2, 0, PKT_COMMAND, 7, "{\"t\":1}"));
  node->poll();
  TEST_ASSERT_EQUAL(0, plat->ofType(PKT_ACK).size());
}

void test_above_floor_sender_gets_a_peer_before_the_ack() {
  deliver(frame(7, 15, PKT_COMMAND, 1, "#HLQ"));
  node->poll();
  uint8_t m7[6]; nodeMac(OCT2, OCT3, 7, m7);
  TEST_ASSERT_TRUE(plat->hasPeer(m7));
  TEST_ASSERT_EQUAL(1, plat->ofType(PKT_ACK).size());
}

// Regression (WCB_Client 1.17.1): a rebooted WCB restarts its seq; its BOOT
// announce clears our window once, and a second announce of the same boot
// does not (so a lost-ACK retry is still deduped).
void test_boot_announce_clears_dedupe_once_per_boot() {
  deliver(frame(2, 15, PKT_COMMAND, 5, "#HLU"));
  node->poll();
  plat->now = 10000;
  deliver(frame(2, 0, PKT_BOOT, 0));
  deliver(frame(2, 15, PKT_COMMAND, 5, "#HLD"));            // reused seq after reboot
  node->poll();
  plat->now = 11200;
  deliver(frame(2, 0, PKT_BOOT, 0));                        // 2nd announce, same boot
  deliver(frame(2, 15, PKT_COMMAND, 5, "#HLD"));            // retry of the same command
  node->poll();
  uint8_t sender; char buf[201];
  TEST_ASSERT_TRUE(popText(sender, buf)); TEST_ASSERT_EQUAL_STRING("#HLU", buf);
  TEST_ASSERT_TRUE(popText(sender, buf)); TEST_ASSERT_EQUAL_STRING("#HLD", buf);
  TEST_ASSERT_FALSE(popText(sender, buf));
}

void test_command_queue_overflow_is_counted() {
  for (uint16_t s = 1; s <= 9; s++) deliver(frame(2, 15, PKT_COMMAND, s, "#HLP"));
  node->poll();                                     // ring holds 8; the 9th is lost there
  TEST_ASSERT_EQUAL(1, node->counters().ringFull);
  for (uint16_t s = 10; s <= 17; s++) deliver(frame(2, 15, PKT_COMMAND, s, "#HLP"));
  node->poll();                                     // queue already holds 8
  TEST_ASSERT_EQUAL(8, node->counters().cmdQueueFull);
}

// Review finding: a command that cannot be queued must not be ACKed or
// recorded as seen, so the sender's ETM retry delivers it once there is room.
void test_full_queue_does_not_ack_so_retry_delivers() {
  for (uint16_t s = 1; s <= 8; s++) deliver(frame(2, 15, PKT_COMMAND, s, "#HLP"));
  node->poll();                                     // queue now full
  size_t acks = plat->ofType(PKT_ACK).size();
  deliver(frame(2, 15, PKT_COMMAND, 9, "#HLG500"));
  node->poll();
  TEST_ASSERT_EQUAL(acks, plat->ofType(PKT_ACK).size());   // not ACKed
  TEST_ASSERT_EQUAL(1, node->counters().cmdQueueFull);
  uint8_t sender; char buf[201];
  while (popText(sender, buf)) {}
  deliver(frame(2, 15, PKT_COMMAND, 9, "#HLG500"));        // the sender's retry
  node->poll();
  TEST_ASSERT_EQUAL(acks + 1, plat->ofType(PKT_ACK).size());
  TEST_ASSERT_TRUE(popText(sender, buf));
  TEST_ASSERT_EQUAL_STRING("#HLG500", buf);
}

struct Seen { int n = 0; uint8_t sender = 0; char text[201] = {0}; };
static void recordPeek(void* ctx, uint8_t sender, const char* cmd) {
  Seen* s = (Seen*)ctx; s->n++; s->sender = sender; strncpy(s->text, cmd, 200);
}

void test_peek_hook_sees_gated_command_in_ingest() {
  FakePlatform p2; WcbNode n2; Seen seen;
  TEST_ASSERT_TRUE(n2.setPeekHook(recordPeek, &seen));   // before begin()
  TEST_ASSERT_TRUE(n2.begin(config(), p2));
  uint8_t m[6]; nodeMac(OCT2, OCT3, 2, m);
  EtmPacket good = frame(2, 15, PKT_COMMAND, 1, "#HLS");
  n2.ingest(m, (const uint8_t*)&good, sizeof(good));
  TEST_ASSERT_EQUAL(1, seen.n);                     // before any poll()
  TEST_ASSERT_EQUAL(2, seen.sender);
  TEST_ASSERT_EQUAL_STRING("#HLS", seen.text);
  EtmPacket wrongPw = frame(2, 15, PKT_COMMAND, 2, "#HLS", true, "wrong");
  n2.ingest(m, (const uint8_t*)&wrongPw, sizeof(wrongPw));
  EtmPacket bad = frame(2, 15, PKT_COMMAND, 3, "#HLS");
  bad.command[1] = 'X';
  n2.ingest(m, (const uint8_t*)&bad, sizeof(bad));
  TEST_ASSERT_EQUAL(1, seen.n);
}

// Review finding: ingest() reads the hook unsynchronised, so it may only be
// set while the node has never started.
void test_peek_hook_refused_after_begin() {
  Seen seen;
  TEST_ASSERT_FALSE(node->setPeekHook(recordPeek, &seen));   // node already begun in setUp
  deliver(frame(2, 15, PKT_COMMAND, 1, "#HLS"));
  TEST_ASSERT_EQUAL(0, seen.n);
}

void test_password_of_39_chars_matches() {
  FakePlatform p2; WcbNode n2;
  WcbConfig c = config();
  memset(c.password, 'k', 39); c.password[39] = '\0';
  TEST_ASSERT_TRUE(n2.begin(c, p2));
  EtmPacket p = frame(2, 15, PKT_COMMAND, 1, "#HLQ", true, c.password);
  uint8_t m[6]; nodeMac(OCT2, OCT3, 2, m);
  n2.ingest(m, (const uint8_t*)&p, sizeof(p));
  n2.poll();
  uint8_t sender; char buf[201];
  TEST_ASSERT_TRUE(n2.popCommand(sender, buf, sizeof(buf)));
}

void test_command_field_without_nul_is_bounded() {
  EtmPacket p = frame(2, 15, PKT_COMMAND, 1, "#HLQ");
  memset(p.command, 'A', sizeof(p.command));       // 200 bytes, no terminator
  deliver(p);
  node->poll();
  uint8_t sender; char buf[201];
  TEST_ASSERT_FALSE(popText(sender, buf));          // no valid CRC tag: dropped, no overrun
  TEST_ASSERT_EQUAL(1, node->counters().crcFail);
  TEST_ASSERT_EQUAL(1, plat->ofType(PKT_ACK).size());
}

void test_pop_truncates_to_caller_buffer() {
  deliver(frame(2, 15, PKT_COMMAND, 1, "#HLG12345"));
  node->poll();
  uint8_t sender; char small[5];
  TEST_ASSERT_TRUE(node->popCommand(sender, small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("#HLG", small);
}

// ---- online tracking ---------------------------------------------------------

void test_online_tracking() {
  TEST_ASSERT_FALSE(node->isOnline(2));
  deliver(frame(2, 0, PKT_HEARTBEAT, 0));
  node->poll();
  TEST_ASSERT_TRUE(node->isOnline(2));
  plat->now = 50001;
  node->poll();
  TEST_ASSERT_FALSE(node->isOnline(2));
}

// ---- sending --------------------------------------------------------------------

void test_send_appends_crc_and_uses_nonzero_seq() {
  TEST_ASSERT_TRUE(node->send(2, ";S2,#HL:POS 4520"));
  auto cmds = plat->ofType(PKT_COMMAND);
  TEST_ASSERT_EQUAL(1, cmds.size());
  TEST_ASSERT_EQUAL_STRING(";S2,#HL:POS 4520|CRCBDA30ACC", cmds[0].etm().command);
  TEST_ASSERT_EQUAL_STRING("2", cmds[0].etm().targetId);
  TEST_ASSERT_EQUAL(1, cmds[0].etm().seq);
  uint8_t m2[6]; nodeMac(OCT2, OCT3, 2, m2);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(m2, cmds[0].mac, 6);
}

void test_send_rejects_bad_target_and_length() {
  char s187[188]; memset(s187, 'a', 187); s187[187] = '\0';
  char s188[189]; memset(s188, 'a', 188); s188[188] = '\0';
  TEST_ASSERT_TRUE(node->send(2, s187));
  TEST_ASSERT_FALSE(node->send(2, s188));
  TEST_ASSERT_FALSE(node->send(0, "x"));
  TEST_ASSERT_FALSE(node->send(21, "x"));
  TEST_ASSERT_FALSE(node->send(15, "x"));            // self
  TEST_ASSERT_FALSE(node->send(2, nullptr));
}

void test_ensured_send_retries_with_same_seq_until_acked() {
  node->send(2, "#HL:OK");
  plat->now = 500; node->poll();
  plat->now = 1000; node->poll();
  auto cmds = plat->ofType(PKT_COMMAND);
  TEST_ASSERT_EQUAL(3, cmds.size());
  TEST_ASSERT_EQUAL(cmds[0].etm().seq, cmds[2].etm().seq);
  deliver(frame(2, 15, PKT_ACK, cmds[0].etm().seq));
  plat->now = 1100; node->poll();
  plat->now = 3000; node->poll();
  TEST_ASSERT_EQUAL(3, plat->ofType(PKT_COMMAND).size());
  TEST_ASSERT_EQUAL(2, node->counters().retries);
  TEST_ASSERT_EQUAL(0, node->counters().gaveUp);
}

void test_ensured_send_gives_up_after_three_retries() {
  node->send(2, "#HL:OK");
  for (plat->now = 0; plat->now <= 3000; plat->now += 100) node->poll();
  TEST_ASSERT_EQUAL(4, plat->ofType(PKT_COMMAND).size());   // 1 + 3 retries
  TEST_ASSERT_EQUAL(1, node->counters().gaveUp);
}

void test_seq_skips_zero_on_wrap() {
  bool sawZero = false;
  uint16_t last = 0;
  for (uint32_t i = 0; i < 65536; i++) {
    plat->sent.clear();
    node->send(2, "x", false);
    uint16_t s = plat->ofType(PKT_COMMAND)[0].etm().seq;
    if (s == 0) sawZero = true;
    last = s;
  }
  TEST_ASSERT_FALSE(sawZero);
  TEST_ASSERT_EQUAL(1, last);                        // 65535 then wraps to 1
}

void test_send_failure_is_counted() {
  plat->sendOk = false;
  TEST_ASSERT_FALSE(node->send(2, "x"));
  TEST_ASSERT_EQUAL(1, node->counters().txFail);
}

void test_send_queue_full_reports_false() {
  for (int i = 0; i < 10; i++) TEST_ASSERT_TRUE(node->send(2, "x"));
  TEST_ASSERT_FALSE(node->send(2, "y"));
  TEST_ASSERT_EQUAL(1, node->counters().sendQueueFull);
  TEST_ASSERT_EQUAL(11, plat->ofType(PKT_COMMAND).size());   // still sent once
}

// Query replies are broadcast so every WCB forwards them to its broadcast-out
// ports (docs/SERIAL_COMMANDS.md).
void test_broadcast_goes_to_group_mac_untracked() {
  TEST_ASSERT_TRUE(node->broadcast("#HL:POS 4520"));
  auto cmds = plat->ofType(PKT_COMMAND);
  TEST_ASSERT_EQUAL(1, cmds.size());
  uint8_t g[6]; groupMac(OCT2, OCT3, g);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(g, cmds[0].mac, 6);
  TEST_ASSERT_EQUAL_STRING("0", cmds[0].etm().targetId);
  TEST_ASSERT_EQUAL_STRING("15", cmds[0].etm().senderId);
  TEST_ASSERT_EQUAL_UINT8(1, cmds[0].etm().commandIncluded);
  char expect[64]; appendCrc("#HL:POS 4520", expect, sizeof(expect));
  TEST_ASSERT_EQUAL_STRING(expect, cmds[0].etm().command);
  TEST_ASSERT_NOT_EQUAL(0, cmds[0].etm().seq);
  for (plat->now = 0; plat->now <= 3000; plat->now += 100) node->poll();
  TEST_ASSERT_EQUAL(1, plat->ofType(PKT_COMMAND).size());   // best effort: never retried
  TEST_ASSERT_EQUAL(0, node->counters().retries);
}

void test_broadcast_rejects_bad_input() {
  char s188[189]; memset(s188, 'a', 188); s188[188] = '\0';
  TEST_ASSERT_FALSE(node->broadcast(s188));
  TEST_ASSERT_FALSE(node->broadcast(nullptr));
  node->suspend();
  TEST_ASSERT_FALSE(node->broadcast("#HL:POS 1"));
  TEST_ASSERT_EQUAL(0, plat->ofType(PKT_COMMAND).size());
}

// ---- RTERM ---------------------------------------------------------------------

void test_send_term_splits_long_lines() {
  char line[200]; memset(line, 'z', 199); line[199] = '\0';
  TEST_ASSERT_TRUE(node->sendTerm(2, line));
  int terms = 0;
  for (const auto& f : plat->sent) {
    if (f.data.size() != sizeof(RtermPacket)) continue;
    terms++;
    TEST_ASSERT_EQUAL_UINT8(PKT_RTERM, f.rterm().packetType);
    TEST_ASSERT_EQUAL_UINT8(15, f.rterm().sourceId);
    TEST_ASSERT_EQUAL_UINT8(2, f.rterm().destId);
  }
  TEST_ASSERT_EQUAL(2, terms);                       // 160 + 39
  TEST_ASSERT_EQUAL_UINT8(39, plat->sent.back().rterm().textLen);
}

// ---- suspend / rejoin ------------------------------------------------------------

void test_suspend_stops_everything() {
  node->suspend();
  deliver(frame(2, 15, PKT_COMMAND, 1, "#HLU"));
  plat->now = 20000;
  node->poll();
  TEST_ASSERT_EQUAL(0, plat->sent.size());
  TEST_ASSERT_FALSE(node->send(2, "x"));
  TEST_ASSERT_FALSE(node->sendTerm(2, "x"));
}

void test_rejoin_reannounces_and_forgets_windows() {
  deliver(frame(2, 15, PKT_COMMAND, 5, "#HLU"));
  node->poll();
  uint8_t sender; char buf[201];
  popText(sender, buf);
  node->suspend();
  plat->now = 100000;
  plat->sent.clear();
  plat->peers.clear();
  node->rejoin();
  node->poll();
  TEST_ASSERT_EQUAL(1, plat->ofType(PKT_BOOT).size());
  TEST_ASSERT_EQUAL(1, plat->ofType(PKT_HEARTBEAT).size());
  TEST_ASSERT_EQUAL(4, plat->peers.size());          // peers re-registered
  deliver(frame(2, 15, PKT_COMMAND, 5, "#HLD"));    // missed reboot: same seq again
  node->poll();
  TEST_ASSERT_TRUE(popText(sender, buf));
  TEST_ASSERT_EQUAL_STRING("#HLD", buf);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_begin_registers_floor_peers_and_group);
  RUN_TEST(test_begin_skips_own_id_in_floor);
  RUN_TEST(test_begin_rejects_bad_config);
  RUN_TEST(test_first_poll_sends_boot_and_heartbeat_to_group);
  RUN_TEST(test_three_boot_announces_then_heartbeats_every_10s);
  RUN_TEST(test_wdp_advert_bytes_and_burst);
  RUN_TEST(test_no_advert_without_identity);
  RUN_TEST(test_solicit_triggers_advert_with_jitter);
  RUN_TEST(test_command_is_acked_and_delivered_without_crc);
  RUN_TEST(test_broadcast_command_is_accepted);
  RUN_TEST(test_duplicate_is_acked_but_not_delivered_twice);
  RUN_TEST(test_bad_crc_is_acked_and_dropped);
  RUN_TEST(test_checksum_off_delivers_text_verbatim);
  RUN_TEST(test_gate_rejections);
  RUN_TEST(test_json_broadcast_is_not_acked);
  RUN_TEST(test_above_floor_sender_gets_a_peer_before_the_ack);
  RUN_TEST(test_boot_announce_clears_dedupe_once_per_boot);
  RUN_TEST(test_command_queue_overflow_is_counted);
  RUN_TEST(test_full_queue_does_not_ack_so_retry_delivers);
  RUN_TEST(test_peek_hook_sees_gated_command_in_ingest);
  RUN_TEST(test_peek_hook_refused_after_begin);
  RUN_TEST(test_password_of_39_chars_matches);
  RUN_TEST(test_command_field_without_nul_is_bounded);
  RUN_TEST(test_pop_truncates_to_caller_buffer);
  RUN_TEST(test_online_tracking);
  RUN_TEST(test_send_appends_crc_and_uses_nonzero_seq);
  RUN_TEST(test_send_rejects_bad_target_and_length);
  RUN_TEST(test_ensured_send_retries_with_same_seq_until_acked);
  RUN_TEST(test_ensured_send_gives_up_after_three_retries);
  RUN_TEST(test_seq_skips_zero_on_wrap);
  RUN_TEST(test_send_failure_is_counted);
  RUN_TEST(test_send_queue_full_reports_false);
  RUN_TEST(test_broadcast_goes_to_group_mac_untracked);
  RUN_TEST(test_broadcast_rejects_bad_input);
  RUN_TEST(test_send_term_splits_long_lines);
  RUN_TEST(test_suspend_stops_everything);
  RUN_TEST(test_rejoin_reannounces_and_forgets_windows);
  return UNITY_END();
}
