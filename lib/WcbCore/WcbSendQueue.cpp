// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#include "WcbSendQueue.h"
#include <string.h>

namespace wcb {

int SendQueue::add(uint8_t target, uint16_t seq, const char* text, bool ensured, uint32_t now) {
  int slot = -1;
  for (int i = 0; i < (int)PENDING_MAX; i++)
    if (!slots_[i].active) { slot = i; break; }
  if (slot < 0) {                                  // full: reuse the oldest best-effort slot
    uint32_t oldest = 0;
    for (int i = 0; i < (int)PENDING_MAX; i++) {
      if (slots_[i].ensured) continue;
      uint32_t age = now - slots_[i].sentMs;
      if (slot < 0 || age > oldest) { slot = i; oldest = age; }
    }
  }
  if (slot < 0) return -1;
  Pending& p = slots_[slot];
  p.active = true;
  p.ensured = ensured;
  p.target = target;
  p.seq = seq;
  p.retries = 0;
  p.sentMs = now;
  strncpy(p.text, text, sizeof(p.text) - 1);
  p.text[sizeof(p.text) - 1] = '\0';
  return slot;
}

bool SendQueue::onAck(uint8_t sender, uint16_t seq) {
  for (auto& p : slots_) {
    if (p.active && p.seq == seq && p.target == sender) { p.active = false; return true; }
  }
  return false;
}

SendQueue::TickResult SendQueue::tick(uint32_t now, const PeerTable& peers, RetransmitFn fn, void* ctx) {
  TickResult r;
  for (auto& p : slots_) {
    if (!p.active) continue;
    uint32_t age = now - p.sentMs;
    if (!p.ensured) {
      if (age > BEST_EFFORT_FREE_MS) p.active = false;
      continue;
    }
    if (age < RETRY_MS) continue;
    bool departed = peers.everSeen(p.target) && !peers.isOnline(p.target);
    if (departed || p.retries >= MAX_RETRIES) { p.active = false; r.gaveUp++; continue; }
    p.retries++;
    p.sentMs = now;
    r.retries++;
    fn(ctx, p.target, p.seq, p.text);
  }
  return r;
}

void SendQueue::clear() {
  for (auto& p : slots_) p.active = false;
}

int SendQueue::activeCount() const {
  int n = 0;
  for (const auto& p : slots_) n += p.active ? 1 : 0;
  return n;
}

}  // namespace wcb
