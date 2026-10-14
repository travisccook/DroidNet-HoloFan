// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#include "WcbNode.h"
#include <string.h>
#include "WcbCrc.h"
#include "WcbRterm.h"

namespace wcb {

bool WcbNode::begin(const WcbConfig& cfg, WcbPlatform& platform) {
  if (cfg.id < 1 || cfg.id > MAX_ID || cfg.floorQ > MAX_ID || cfg.password[0] == '\0') return false;
  cfg_ = cfg;
  cfg_.password[PASSWORD_FIELD - 1] = '\0';
  plat_ = &platform;
  groupMac(cfg_.oct2, cfg_.oct3, groupMac_);
  begun_ = true;
  rejoin();
  return true;
}

bool WcbNode::setPeekHook(PeekFn fn, void* ctx) {
  if (begun_) return false;              // ingest() may already be reading it
  peekCtx_ = ctx;
  peek_ = fn;
  return true;
}

void WcbNode::suspend() { started_.store(false, std::memory_order_release); }

void WcbNode::rejoin() {
  if (!begun_) return;
  ring_.clear();
  peers_.reset();
  sendq_.clear();
  cmdHead_ = cmdCount_ = 0;
  registerPeers();
  sched_.arm(plat_->nowMs(), cfg_.id, cfg_.identity.devType[0] != '\0');
  started_.store(true, std::memory_order_release);
}

void WcbNode::registerPeers() {
  for (uint8_t id = 1; id <= cfg_.floorQ; id++)
    if (id != cfg_.id) ensurePeer(id);
  plat_->addPeer(groupMac_);
}

void WcbNode::ensurePeer(uint8_t id) {
  if (peers_.registered(id)) return;
  uint8_t mac[6];
  nodeMac(cfg_.oct2, cfg_.oct3, id, mac);
  if (plat_->addPeer(mac)) peers_.setRegistered(id, true);
}

WcbNode::Gate WcbNode::gate(const uint8_t mac[6], const EtmPacket& p, uint8_t& sender) const {
  if (!isMeshMac(mac, cfg_.oct2, cfg_.oct3)) return Gate::MAC;
  if (strncmp(p.password, cfg_.password, PASSWORD_FIELD - 1) != 0) return Gate::PASSWORD;
  int s = parseId(p.senderId);
  if (s < 1 || s > MAX_ID || s != mac[5] || s == cfg_.id) return Gate::SPOOF;
  int t = parseId(p.targetId);
  if (t != TARGET_BROADCAST && t != cfg_.id) return Gate::TARGET;
  sender = (uint8_t)s;
  return Gate::OK;
}

bool WcbNode::extractCommand(const EtmPacket& p, char out[COMMAND_FIELD + 1]) const {
  memcpy(out, p.command, COMMAND_FIELD);
  out[COMMAND_FIELD] = '\0';
  if (!cfg_.checksum) return true;
  return verifyStripCrc(out) == CrcCheck::OK;
}

void WcbNode::ingest(const uint8_t mac[6], const uint8_t* data, size_t len) {
  if (!started()) return;
  PeekFn fn = peek_;
  if (fn && len == sizeof(EtmPacket)) {
    EtmPacket p;
    memcpy(&p, data, sizeof(p));
    uint8_t sender = 0;
    char cmd[COMMAND_FIELD + 1];
    if (p.packetType == PKT_COMMAND && p.commandIncluded && gate(mac, p, sender) == Gate::OK &&
        extractCommand(p, cmd))
      fn(peekCtx_, sender, cmd);
  }
  if (!ring_.push(mac, data, len))
    ringFull_.store(ringFull_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void WcbNode::poll() {
  if (!started()) return;
  uint32_t now = plat_->nowMs();
  typename PacketRing<RING_N>::Slot s;
  while (ring_.pop(s)) handle(s, now);
  peers_.sweep(now);
  uint8_t act = sched_.due(now);
  if (act & ACT_BOOT) sendGroup(PKT_BOOT);
  if (act & ACT_HEARTBEAT) sendGroup(PKT_HEARTBEAT);
  if (act & ACT_WDP) sendAdvert();
  SendQueue::TickResult r = sendq_.tick(now, peers_, &WcbNode::retransmitThunk, this);
  counters_.retries += r.retries;
  counters_.gaveUp += r.gaveUp;
}

void WcbNode::handle(const typename PacketRing<RING_N>::Slot& s, uint32_t now) {
  if (!isMeshMac(s.mac, cfg_.oct2, cfg_.oct3)) { counters_.rejMac++; return; }
  if (s.len != sizeof(EtmPacket)) { counters_.rejSize++; return; }
  EtmPacket p;
  memcpy(&p, s.data, sizeof(p));
  uint8_t sender = 0;
  switch (gate(s.mac, p, sender)) {
    case Gate::OK:       break;
    case Gate::MAC:      counters_.rejMac++; return;
    case Gate::PASSWORD: counters_.rejPassword++; return;
    case Gate::SPOOF:    counters_.rejSpoof++; return;
    case Gate::TARGET:   counters_.rejTarget++; return;
  }
  counters_.rxOk++;
  peers_.markSeen(sender, now);
  switch (p.packetType) {
    case PKT_BOOT:    peers_.onBoot(sender, now); break;
    case PKT_ACK:     sendq_.onAck(sender, p.seq); break;
    case PKT_COMMAND: handleCommand(p, sender); break;
    case PKT_WDP:
      if (p.commandIncluded && isSolicit((const uint8_t*)p.command, (int)COMMAND_FIELD))
        sched_.solicit(now, cfg_.id);
      break;
    default: break;  // heartbeats only mark presence; other types are not for clients
  }
}

void WcbNode::handleCommand(const EtmPacket& p, uint8_t sender) {
  // No room to deliver: do not ACK and do not record the seq, so the
  // sender's ETM retry (500 ms x 3) delivers it once the app has drained.
  if (p.commandIncluded && cmdCount_ >= CMD_QUEUE_N) { counters_.cmdQueueFull++; return; }
  ensurePeer(sender);                    // before the ACK: an above-floor sender needs a peer entry
  bool jsonBroadcast = parseId(p.targetId) == TARGET_BROADCAST && p.commandIncluded && p.command[0] == '{';
  if (!jsonBroadcast) sendAck(sender, p.seq);   // ACK duplicates and bad CRCs too
  if (!p.commandIncluded) return;
  char cmd[COMMAND_FIELD + 1];
  if (!extractCommand(p, cmd)) { counters_.crcFail++; return; }
  if (peers_.isDupCommand(sender, p.seq)) { counters_.dupDrop++; return; }
  pushCmd(sender, cmd);                  // room was checked on entry
}

bool WcbNode::pushCmd(uint8_t sender, const char* text) {
  if (cmdCount_ >= CMD_QUEUE_N) return false;
  CmdItem& c = cmdq_[(cmdHead_ + cmdCount_) % CMD_QUEUE_N];
  c.sender = sender;
  strncpy(c.text, text, sizeof(c.text) - 1);
  c.text[sizeof(c.text) - 1] = '\0';
  cmdCount_++;
  return true;
}

bool WcbNode::popCommand(uint8_t& sender, char* buf, size_t size) {
  if (cmdCount_ == 0 || size == 0) return false;
  const CmdItem& c = cmdq_[cmdHead_];
  sender = c.sender;
  strncpy(buf, c.text, size - 1);
  buf[size - 1] = '\0';
  cmdHead_ = (cmdHead_ + 1) % CMD_QUEUE_N;
  cmdCount_--;
  return true;
}

bool WcbNode::sendFrame(const uint8_t mac[6], const void* frame, size_t len) {
  bool ok = plat_->send(mac, (const uint8_t*)frame, len);
  if (!ok) counters_.txFail++;
  return ok;
}

void WcbNode::sendAck(uint8_t target, uint16_t seq) {
  EtmPacket a;
  initEtm(a, cfg_.password, cfg_.id, target, PKT_ACK, seq);
  uint8_t mac[6];
  nodeMac(cfg_.oct2, cfg_.oct3, target, mac);
  sendFrame(mac, &a, sizeof(a));
}

void WcbNode::sendGroup(uint8_t type) {
  EtmPacket p;
  initEtm(p, cfg_.password, cfg_.id, TARGET_BROADCAST, type, 0);
  sendFrame(groupMac_, &p, sizeof(p));
}

void WcbNode::sendAdvert() {
  EtmPacket p;
  initEtm(p, cfg_.password, cfg_.id, TARGET_BROADCAST, PKT_WDP, 0);
  if (buildAdvert(cfg_.identity, (uint8_t*)p.command, (int)COMMAND_FIELD) == 0) return;
  p.commandIncluded = 1;
  sendFrame(groupMac_, &p, sizeof(p));
}

uint16_t WcbNode::nextSeq() {
  if (++seq_ == 0) ++seq_;               // seq 0 never goes on the wire
  return seq_;
}

void WcbNode::buildCommand(EtmPacket& p, uint8_t target, uint16_t seq, const char* text) const {
  initEtm(p, cfg_.password, cfg_.id, target, PKT_COMMAND, seq);
  p.commandIncluded = 1;
  if (cfg_.checksum) appendCrc(text, p.command, COMMAND_FIELD);
  else strncpy(p.command, text, COMMAND_FIELD - 1);
}

bool WcbNode::fitsCommand(const char* text) const {
  return strlen(text) <= (cfg_.checksum ? MAX_CMD_WITH_CRC : MAX_CMD_NO_CRC);
}

bool WcbNode::transmitCommand(uint8_t target, uint16_t seq, const char* text) {
  EtmPacket p;
  buildCommand(p, target, seq, text);
  uint8_t mac[6];
  nodeMac(cfg_.oct2, cfg_.oct3, target, mac);
  return sendFrame(mac, &p, sizeof(p));
}

void WcbNode::retransmitThunk(void* ctx, uint8_t target, uint16_t seq, const char* text) {
  static_cast<WcbNode*>(ctx)->transmitCommand(target, seq, text);
}

bool WcbNode::send(uint8_t target, const char* text, bool ensured) {
  if (!started() || !text || target < 1 || target > MAX_ID || target == cfg_.id) return false;
  if (!fitsCommand(text)) return false;
  ensurePeer(target);
  uint16_t seq = nextSeq();
  int slot = sendq_.add(target, seq, text, ensured, plat_->nowMs());
  bool ok = transmitCommand(target, seq, text);
  if (slot < 0) { counters_.sendQueueFull++; return false; }
  return ok;
}

bool WcbNode::broadcast(const char* text) {
  if (!started() || !text || !fitsCommand(text)) return false;
  EtmPacket p;
  buildCommand(p, TARGET_BROADCAST, nextSeq(), text);
  return sendFrame(groupMac_, &p, sizeof(p));
}

bool WcbNode::sendTerm(uint8_t target, const char* line) {
  if (!started() || !line || target < 1 || target > MAX_ID || target == cfg_.id) return false;
  ensurePeer(target);
  uint8_t mac[6];
  nodeMac(cfg_.oct2, cfg_.oct3, target, mac);
  size_t len = strlen(line);
  bool ok = true;
  do {
    RtermPacket r;
    size_t n = buildRterm(r, cfg_.password, cfg_.id, target, line, len);
    ok = sendFrame(mac, &r, sizeof(r)) && ok;
    line += n;
    len -= n;
  } while (len > 0);
  return ok;
}

WcbCounters WcbNode::counters() const {
  WcbCounters c = counters_;
  c.ringFull = ringFull_.load(std::memory_order_relaxed);
  return c;
}

}  // namespace wcb
