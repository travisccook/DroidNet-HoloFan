#include "FrameRing.h"

namespace fanlogic {

bool FrameRing::begin() {
  cap_ = io_.size() / RING_BLOCK * RING_BLOCK;
  head_ = tail_ = erased_ = 0;
  return cap_ > 0;
}

bool FrameRing::full() const {
  return (head_ - tail_) + SPI_FRAME + RING_SECTOR > cap_;
}

bool FrameRing::push(const uint8_t* frame) {
  if (cap_ == 0 || full()) return false;
  // Erase whole sectors ahead of the writer. Safe: !full() means a whole
  // sector stays free after this frame, so the sector erased here (ending at
  // most SECTOR-1 bytes past it) never aliases an unread byte.
  while (erased_ < head_ + SPI_FRAME) {
    if (!io_.erase(erased_ % cap_, RING_SECTOR)) return false;
    erased_ += RING_SECTOR;
  }
  if (!io_.write(head_ % cap_, frame, SPI_FRAME)) return false;
  head_ += SPI_FRAME;
  return true;
}

bool FrameRing::pop(uint8_t* frame) {
  if (head_ == tail_) return false;
  if (!io_.read(tail_ % cap_, frame, SPI_FRAME)) return false;
  tail_ += SPI_FRAME;
  return true;
}

}  // namespace fanlogic
