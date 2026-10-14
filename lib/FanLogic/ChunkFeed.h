// The staging side of a resumable clip upload (docs/MAINTENANCE_API.md):
// chunk requests write clip bytes at an offset, the upload task
// pops 1440-byte frames for the MCU. Pure C++ and single-threaded: on the
// fan every call is made under the session mutex (UploadIdf.cpp).
#ifndef FAN_CHUNK_FEED_H
#define FAN_CHUNK_FEED_H
#include <stddef.h>
#include <stdint.h>
#include "ClipOps.h"

namespace fanlogic {

// Before the first frame goes to the MCU: no new body bytes this long = failed ('s').
constexpr uint32_t PREFILL_IDLE_MS = 600000;
// After it: the body incomplete and no frame popped for dryMs (default
// DRY_MS, per session since 0.8.1) = failed ('d'), whatever the ring holds
// (a BURST session can hold a partial window for a long time). The MCU
// aborts a transfer when windows come more than about 3-4 s apart (bench
// 2026-10-07; 3.0 s gaps were accepted).
constexpr uint32_t DRY_MS = 2000;

class ChunkFeed {
 public:
  ChunkFeed(FrameRing& ring, Clock& clock) : ring_(ring), clock_(clock) {}
  // A new session of `size` clip bytes. The ring must be begun (empty);
  // `asmBuf` holds SPI_FRAME bytes (the frame being assembled).
  // prefilled() at `prefillFrames` buffered (0 = the ring's capacity minus
  // one window, 620 frames on the fan, so the create phase has a window of
  // headroom; clamped to [1, capacity - SPI_WINDOW_FRAMES]) or when the whole
  // body is in. `dryMs`: the dry rule's limit.
  void begin(size_t size, uint8_t* asmBuf, size_t prefillFrames = 0, uint32_t dryMs = DRY_MS);
  size_t size() const { return size_; }
  // Clip bytes the feed holds: in the ring, popped, or in the assembly buffer.
  size_t staged() const { return staged_; }
  // Producer. Bytes [pos, pos + n) of the clip: those below staged() are
  // dropped (dupBytes), the rest staged. Returns the bytes consumed
  // (dropped or staged): fewer than n only when the ring is full (call
  // again after a pop), the clip is complete (nothing past size() is
  // staged) or the feed has failed; 0 if pos > staged().
  size_t write(size_t pos, const uint8_t* data, size_t n);
  // DELETE /api/upload, or the session ended: the body fails ('a').
  void abort();
  // Consumer (the FrameSource calls).
  size_t count() const { return ring_.count(); }
  bool full() const { return ring_.full(); }
  bool prefilled() const { return staged_ == size_ || ring_.count() >= prefill_; }
  BodyState state();          // also applies the idle and dry rules (clock)
  ClipErr failErr() const { return err_; }
  bool pop(uint8_t* frame);   // false: empty or a flash error
  bool started() const { return popped_; }
  size_t bodyBytes() const { return staged_; }
  char bodyEnd() const { return end_; }
  // Diagnostics.
  size_t dupBytes() const { return dup_; }
  uint32_t longestQuietMs() const { return quietMax_; }
  // The lowest count() right after a pop while the body was still
  // arriving (how close the session came to running dry); SIZE_MAX before.
  size_t ringMin() const { return ringMin_; }
  // "stg": clip bytes staged by the time the first frame was popped, per
  // second from begin() to that pop (B/s; 0 before the first pop).
  uint32_t stagingRate() const {
    uint32_t ms = firstPopMs_ - openMs_;
    return popped_ && ms ? (uint32_t)((uint64_t)stagedAtPop_ * 1000 / ms) : 0;
  }

 private:
  void fail(char end, ClipErr err);
  FrameRing& ring_;
  Clock&     clock_;
  uint8_t*   asm_ = nullptr;
  size_t     size_ = 0, staged_ = 0, fill_ = 0, prefill_ = 0, dup_ = 0, ringMin_ = SIZE_MAX;
  uint32_t   dryMs_ = DRY_MS, lastByteMs_ = 0, lastPopMs_ = 0, quietMax_ = 0;
  size_t     stagedAtPop_ = 0;
  uint32_t   openMs_ = 0, firstPopMs_ = 0;
  bool       popped_ = false;
  BodyState  state_ = BodyState::ARRIVING;
  ClipErr    err_ = ClipErr::OK;
  char       end_ = '-';
};

}  // namespace fanlogic
#endif
