// Per-node mesh state: online tracking, ESP-NOW peer registration and the
// inbound anti-replay window (cleared by the sender's BOOT announce).
// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#ifndef WCB_PEERS_H
#define WCB_PEERS_H
#include <stdint.h>
#include "WcbPacket.h"
#include "WcbSeqWindow.h"

namespace wcb {

class PeerTable {
 public:
  void reset();                                  // forget all state (begin / rejoin)
  void markSeen(uint8_t id, uint32_t now);       // any valid packet = presence
  bool isOnline(uint8_t id) const;
  bool everSeen(uint8_t id) const;
  void sweep(uint32_t now);                      // offline after OFFLINE_MS of silence
  bool isDupCommand(uint8_t id, uint16_t seq);   // seq 0 is never a dup
  // BOOT announce: clears the sender's window at most once per
  // BOOT_RESET_HOLD_MS (one boot sends 3 announces ~1.2 s apart).
  void onBoot(uint8_t id, uint32_t now);
  bool registered(uint8_t id) const;             // ESP-NOW peer added?
  void setRegistered(uint8_t id, bool on);

 private:
  struct Entry {
    bool      online = false;
    bool      seen = false;
    uint32_t  lastSeenMs = 0;
    bool      resetArmed = false;
    uint32_t  seqResetMs = 0;
    bool      registered = false;
    SeqWindow window;
  };
  static bool valid(uint8_t id) { return id >= 1 && id <= MAX_ID; }
  Entry e_[MAX_ID + 1];                          // indexed by id; [0] unused
};

}  // namespace wcb
#endif
