// What WcbNode needs from the board. Implemented by lib/WcbArduino
// (HoloLifter) and firmware/holofan/components/wcb_idf (fan), and by a fake
// in the native tests. Every method is called only from the poll() task.
#ifndef WCB_PLATFORM_H
#define WCB_PLATFORM_H
#include <stddef.h>
#include <stdint.h>

namespace wcb {

class WcbPlatform {
 public:
  virtual ~WcbPlatform() = default;
  virtual uint32_t nowMs() = 0;                                              // monotonic ms
  virtual bool send(const uint8_t mac[6], const uint8_t* data, size_t len) = 0;  // esp_now_send
  virtual bool addPeer(const uint8_t mac[6]) = 0;                            // esp_now_add_peer, unencrypted, channel 0
};

}  // namespace wcb
#endif
