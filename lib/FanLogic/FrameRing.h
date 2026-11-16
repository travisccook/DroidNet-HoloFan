// A FIFO of 1440-byte SPI frames in a flash region (on the fan: the spare
// OTA slot), so a clip upload can buffer about 855 KB ahead of the MCU and ride
// out Wi-Fi stalls. Pure C++ over FlashIo, so
// the native env tests it with a fake flash.
//
// The ring is a whole number of 184320-byte blocks (lcm of the 1440-byte
// frame and the 4096-byte sector: 128 frames, 45 sectors), so frames never
// straddle the end and sectors line up. Flash is erased ahead of the writer:
// push() erases a 4 KB sector when it has to, and eraseAhead() erases whole
// 64 KB blocks, which cost a sixth as much per byte on the fan's flash (a
// block about 95 ms, a sector about 37 ms) but stop the CPU for longer, so
// the caller picks a moment when nothing is arriving (0.9.18). A push is
// refused (full()) unless a whole erase block stays free after it, so an
// erase never touches unread frames.
#ifndef FAN_FRAME_RING_H
#define FAN_FRAME_RING_H
#include <stddef.h>
#include <stdint.h>

namespace fanlogic {

constexpr size_t SPI_FRAME = 1440;
constexpr size_t RING_SECTOR = 4096;
constexpr size_t RING_BLOCK = 184320;   // lcm(SPI_FRAME, RING_SECTOR)
constexpr size_t RING_ERASE_BLOCK = 65536;   // the flash chip's block erase; the room full() keeps free

// Raw flash access (offsets relative to the region). erase() takes one
// sector, or one erase block at a multiple of RING_ERASE_BLOCK; write() may
// only be used on erased bytes.
class FlashIo {
 public:
  virtual ~FlashIo() = default;
  virtual size_t size() const = 0;
  virtual bool erase(size_t off, size_t len) = 0;
  virtual bool write(size_t off, const uint8_t* buf, size_t len) = 0;
  virtual bool read(size_t off, uint8_t* buf, size_t len) = 0;
};

class FrameRing {
 public:
  explicit FrameRing(FlashIo& io) : io_(io) {}
  // Empty the ring and size it from the region. False if it is smaller
  // than one RING_BLOCK.
  bool begin();
  // Frames the ring can hold (full() is true at this count).
  size_t capacity() const { return cap_ > RING_ERASE_BLOCK ? (cap_ - RING_ERASE_BLOCK) / SPI_FRAME : 0; }
  // Erase ahead of the writer until `want` bytes past it are erased and the
  // next erase can be a whole block, or until the next erase would touch an
  // unread frame: a 64 KB block where one lines up inside the ring, else a
  // sector. Returns the bytes erased, or -1 if the flash failed.
  long eraseAhead(size_t want);
  uint32_t popped() const { return tail_ / SPI_FRAME; }   // frames popped since begin()
  size_t count() const { return (size_t)((head_ - tail_) / SPI_FRAME); }
  bool full() const;
  // Append one SPI_FRAME frame. False if full() or the flash failed.
  bool push(const uint8_t* frame);
  // Remove the oldest frame into `frame`. False if empty or the flash failed.
  bool pop(uint8_t* frame);

 private:
  FlashIo& io_;
  size_t   cap_ = 0;                     // bytes
  // Absolute byte positions (never wrap within one upload; 4 GB limit).
  uint32_t head_ = 0, tail_ = 0, erased_ = 0;
};

}  // namespace fanlogic
#endif
