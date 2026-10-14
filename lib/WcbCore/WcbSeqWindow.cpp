// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#include "WcbSeqWindow.h"

namespace wcb {

bool SeqWindow::isDup(uint16_t seq) {
  if (!have_) { have_ = true; high_ = seq; mask_ = 0; return false; }
  int16_t d = (int16_t)(uint16_t)(seq - high_);
  if (d == 0) return true;
  if (d > 0) {                                   // newer: slide the window up
    if (d >= 32) mask_ = 0;
    else         mask_ = (mask_ << d) | (1u << (d - 1));
    high_ = seq;
    return false;
  }
  uint16_t back = (uint16_t)(-d);                // older than the high
  if (back <= 32) {
    uint32_t bit = 1u << (back - 1);
    if (mask_ & bit) return true;
    mask_ |= bit;
    return false;
  }
  return false;                                  // too old to remember: accept
}

}  // namespace wcb
