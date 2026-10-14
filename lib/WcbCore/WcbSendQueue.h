// Outbound COMMAND tracking for ETM ("ensured") delivery: up to PENDING_MAX
// in flight; unicast retries every RETRY_MS, MAX_RETRIES times, reusing the
// original sequence number so receivers dedupe them.
// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#ifndef WCB_SEND_QUEUE_H
#define WCB_SEND_QUEUE_H
#include <stdint.h>
#include "WcbPacket.h"
#include "WcbPeers.h"

namespace wcb {

class SendQueue {
 public:
  typedef void (*RetransmitFn)(void* ctx, uint8_t target, uint16_t seq, const char* text);
  struct TickResult { uint16_t retries = 0; uint16_t gaveUp = 0; };

  // Track a send. Returns the slot, or -1 when every slot holds an
  // outstanding ensured send (those are never evicted). When full, the
  // oldest best-effort slot is reused.
  int add(uint8_t target, uint16_t seq, const char* text, bool ensured, uint32_t now);
  // An ACK from `sender` for `seq`. Frees the matching slot; true if matched.
  bool onAck(uint8_t sender, uint16_t seq);
  // Retry due ensured sends, give up on exhausted or departed targets, and
  // free best-effort slots older than BEST_EFFORT_FREE_MS.
  TickResult tick(uint32_t now, const PeerTable& peers, RetransmitFn fn, void* ctx);
  void clear();
  int activeCount() const;

 private:
  struct Pending {
    bool     active = false;
    bool     ensured = false;
    uint8_t  target = 0;
    uint16_t seq = 0;
    uint8_t  retries = 0;
    uint32_t sentMs = 0;
    char     text[MAX_CMD_NO_CRC + 1] = {0};
  };
  Pending slots_[PENDING_MAX];
};

}  // namespace wcb
#endif
