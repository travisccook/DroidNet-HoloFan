// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#include "WcbPeers.h"

namespace wcb {

void PeerTable::reset() {
  for (auto& e : e_) e = Entry();
}

void PeerTable::markSeen(uint8_t id, uint32_t now) {
  if (!valid(id)) return;
  e_[id].online = true;
  e_[id].seen = true;
  e_[id].lastSeenMs = now;
}

bool PeerTable::isOnline(uint8_t id) const { return valid(id) && e_[id].online; }

bool PeerTable::everSeen(uint8_t id) const { return valid(id) && e_[id].seen; }

void PeerTable::sweep(uint32_t now) {
  for (uint8_t id = 1; id <= MAX_ID; id++) {
    Entry& e = e_[id];
    if (e.online && (uint32_t)(now - e.lastSeenMs) > OFFLINE_MS) e.online = false;
  }
}

bool PeerTable::isDupCommand(uint8_t id, uint16_t seq) {
  if (!valid(id) || seq == 0) return false;
  return e_[id].window.isDup(seq);
}

void PeerTable::onBoot(uint8_t id, uint32_t now) {
  if (!valid(id)) return;
  Entry& e = e_[id];
  if (!e.resetArmed || (uint32_t)(now - e.seqResetMs) > BOOT_RESET_HOLD_MS) {
    e.window.reset();
    e.resetArmed = true;
    e.seqResetMs = now;
  }
}

bool PeerTable::registered(uint8_t id) const { return valid(id) && e_[id].registered; }

void PeerTable::setRegistered(uint8_t id, bool on) {
  if (valid(id)) e_[id].registered = on;
}

}  // namespace wcb
