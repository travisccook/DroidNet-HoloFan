// Single-producer / single-consumer ring of received ESP-NOW frames.
// The producer is the radio receive callback (WiFi task); the consumer is the
// task that calls WcbNode::poll(). No locks, no heap: only aligned 32-bit
// atomic loads/stores, which are native on Xtensa and RV32IMC.
#ifndef WCB_RING_H
#define WCB_RING_H
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "WcbPacket.h"

namespace wcb {

template <size_t N>
class PacketRing {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

 public:
  struct Slot {
    uint8_t  mac[6];
    uint16_t len;
    uint8_t  data[sizeof(EtmPacket)];
  };

  // Producer. False if full or the frame is larger than a slot.
  bool push(const uint8_t mac[6], const uint8_t* data, size_t len) {
    if (len > sizeof(EtmPacket)) return false;
    uint32_t h = head_.load(std::memory_order_relaxed);
    uint32_t t = tail_.load(std::memory_order_acquire);
    if (h - t >= N) return false;
    Slot& s = slots_[h & (N - 1)];
    memcpy(s.mac, mac, 6);
    s.len = (uint16_t)len;
    memcpy(s.data, data, len);
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // Consumer. False if empty.
  bool pop(Slot& out) {
    uint32_t t = tail_.load(std::memory_order_relaxed);
    uint32_t h = head_.load(std::memory_order_acquire);
    if (t == h) return false;
    out = slots_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Consumer. Drop everything currently queued.
  void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  Slot slots_[N];
  std::atomic<uint32_t> head_{0};
  std::atomic<uint32_t> tail_{0};
};

}  // namespace wcb
#endif
