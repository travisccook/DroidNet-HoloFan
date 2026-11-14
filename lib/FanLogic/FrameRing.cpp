#include "FrameRing.h"

namespace fanlogic {

bool FrameRing::begin() {
  cap_ = io_.size() / RING_BLOCK * RING_BLOCK;
  head_ = tail_ = erased_ = 0;
  return cap_ > 0;
}

bool FrameRing::full() const {
  return (head_ - tail_) + SPI_FRAME + RING_ERASE_BLOCK > cap_;
}

bool FrameRing::push(const uint8_t* frame) {
  if (cap_ == 0 || full()) return false;
  // Erase whole sectors ahead of the writer where eraseAhead() has not
  // been. Safe: !full() means a whole erase block stays free after this
  // frame, so the sector erased here (ending at most SECTOR-1 bytes past it)
  // never aliases an unread byte.
  while (erased_ < head_ + SPI_FRAME) {
    if (!io_.erase(erased_ % cap_, RING_SECTOR)) return false;
    erased_ += RING_SECTOR;
  }
  if (!io_.write(head_ % cap_, frame, SPI_FRAME)) return false;
  head_ += SPI_FRAME;
  return true;
}

long FrameRing::eraseAhead(size_t want) {
  if (cap_ == 0) return 0;
  long total = 0;
  for (;;) {
    const size_t at = erased_ % cap_;
    const bool aligned = at % RING_ERASE_BLOCK == 0;
    // Enough is erased, and the next erase can be a whole block: stop. Off a
    // block boundary (after push()'s sector erases) go on by sectors until
    // one is reached, so the erases after that are blocks.
    if (aligned && erased_ - head_ >= want) break;
    const size_t len = aligned && at + RING_ERASE_BLOCK <= cap_ ? RING_ERASE_BLOCK : RING_SECTOR;
    if ((erased_ + len) - tail_ > cap_) break;                    // it would reach unread frames
    if (!io_.erase(at, len)) return -1;
    erased_ += len;
    total += (long)len;
  }
  return total;
}

bool FrameRing::pop(uint8_t* frame) {
  if (head_ == tail_) return false;
  if (!io_.read(tail_ % cap_, frame, SPI_FRAME)) return false;
  tail_ += SPI_FRAME;
  return true;
}

}  // namespace fanlogic
