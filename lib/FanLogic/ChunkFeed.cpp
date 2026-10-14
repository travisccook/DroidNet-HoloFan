#include "ChunkFeed.h"
#include <string.h>

namespace fanlogic {

void ChunkFeed::begin(size_t size, uint8_t* asmBuf, size_t prefillFrames, uint32_t dryMs) {
  asm_ = asmBuf;
  size_ = size;
  staged_ = fill_ = dup_ = 0;
  const size_t cap = ring_.capacity();
  const size_t room = cap > (size_t)SPI_WINDOW_FRAMES + 1 ? cap - SPI_WINDOW_FRAMES : 1;   // one window of headroom
  prefill_ = prefillFrames == 0 || prefillFrames > room ? room : prefillFrames;
  dryMs_ = dryMs;
  ringMin_ = SIZE_MAX;
  lastByteMs_ = clock_.nowMs();
  lastPopMs_ = quietMax_ = 0;
  popped_ = false;
  openMs_ = lastByteMs_;
  stagedAtPop_ = firstPopMs_ = 0;
  state_ = size == 0 ? BodyState::COMPLETE : BodyState::ARRIVING;
  err_ = ClipErr::OK;
  end_ = '-';
}

void ChunkFeed::fail(char end, ClipErr err) {
  if (state_ == BodyState::FAILED) return;
  state_ = BodyState::FAILED;
  end_ = end;
  err_ = err;
}

void ChunkFeed::abort() { fail('a', ClipErr::ABORT); }

size_t ChunkFeed::write(size_t pos, const uint8_t* data, size_t n) {
  if (state_ == BodyState::FAILED || pos > staged_) return 0;
  size_t used = 0;
  if (pos < staged_) {                                     // already here: drop
    used = staged_ - pos < n ? staged_ - pos : n;
    dup_ += used;
  }
  const size_t before = staged_;
  while (used < n && staged_ < size_) {
    size_t k = SPI_FRAME - fill_;
    if (k > n - used) k = n - used;
    if (k > size_ - staged_) k = size_ - staged_;
    bool completes = fill_ + k == SPI_FRAME || staged_ + k == size_;
    if (completes && ring_.full()) break;                  // wait for the consumer
    memcpy(asm_ + fill_, data + used, k);
    fill_ += k;
    staged_ += k;
    used += k;
    if (completes) {
      memset(asm_ + fill_, 0, SPI_FRAME - fill_);          // the last frame is zero-padded
      fill_ = 0;
      if (!ring_.push(asm_)) { fail('f', ClipErr::FLASH); return used; }
    }
  }
  if (staged_ > before) {
    uint32_t now = clock_.nowMs();
    if ((uint32_t)(now - lastByteMs_) > quietMax_) quietMax_ = now - lastByteMs_;
    lastByteMs_ = now;
  }
  if (staged_ == size_ && state_ == BodyState::ARRIVING) state_ = BodyState::COMPLETE;
  return used;
}

BodyState ChunkFeed::state() {
  if (state_ != BodyState::ARRIVING) return state_;
  uint32_t now = clock_.nowMs();
  bool idle = !popped_ && (uint32_t)(now - lastByteMs_) >= PREFILL_IDLE_MS;
  bool dry = popped_ && (uint32_t)(now - lastPopMs_) >= dryMs_;
  if (idle || dry) {
    if ((uint32_t)(now - lastByteMs_) > quietMax_) quietMax_ = now - lastByteMs_;   // the unfinished silence counts
    fail(idle ? 's' : 'd', ClipErr::RECV);
  }
  return state_;
}

bool ChunkFeed::pop(uint8_t* frame) {
  if (!ring_.pop(frame)) return false;
  if (!popped_) { stagedAtPop_ = staged_; firstPopMs_ = clock_.nowMs(); }
  popped_ = true;
  lastPopMs_ = clock_.nowMs();
  if (state_ == BodyState::ARRIVING && ring_.count() < ringMin_) ringMin_ = ring_.count();
  return true;
}

}  // namespace fanlogic
