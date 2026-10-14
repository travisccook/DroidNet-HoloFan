// Inbound COMMAND anti-replay window for one sender: the highest sequence
// number seen plus a 32-bit mask of the 32 below it.
// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#ifndef WCB_SEQ_WINDOW_H
#define WCB_SEQ_WINDOW_H
#include <stdint.h>

namespace wcb {

class SeqWindow {
 public:
  // True if `seq` was already seen (drop it). Records it otherwise.
  // Callers never pass seq 0 (un-sequenced packets are not deduped).
  bool isDup(uint16_t seq);
  // Forget everything (the sender rebooted).
  void reset() { have_ = false; mask_ = 0; }

 private:
  bool     have_ = false;
  uint16_t high_ = 0;
  uint32_t mask_ = 0;
};

}  // namespace wcb
#endif
