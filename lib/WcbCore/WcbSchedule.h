// Timers for the periodic mesh traffic a node owes the fleet: 3 BOOT
// announces, a heartbeat every 10 s, and WDP adverts (3-advert burst, then
// every 60 s staggered by id; a SOLICIT pulls one forward).
// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#ifndef WCB_SCHEDULE_H
#define WCB_SCHEDULE_H
#include <stdint.h>
#include "WcbPacket.h"

namespace wcb {

constexpr uint8_t ACT_BOOT      = 0x01;
constexpr uint8_t ACT_HEARTBEAT = 0x02;
constexpr uint8_t ACT_WDP       = 0x04;
constexpr uint32_t HEARTBEAT_MIN_MS = 2000;
constexpr uint32_t HEARTBEAT_MAX_MS = 25000;   // a WCB drops a node after ~55 s (configurable, 33 s seen): one lost beat must fit

class Schedule {
 public:
  // (Re)start: BOOT + heartbeat due now; WDP burst from now + WDP_FIRST_MS.
  void arm(uint32_t now, uint8_t id, bool advertise);
  // A WDP SOLICIT arrived: advert within (id % 16) * 40 ms.
  void solicit(uint32_t now, uint8_t id);
  // Bitmask of ACT_* due at `now`; advances the timers.
  uint8_t due(uint32_t now);
  // Bench: heartbeat period from the next beat on (0 = HEARTBEAT_MS),
  // clamped to HEARTBEAT_MIN_MS..HEARTBEAT_MAX_MS. Kept across arm().
  void setHeartbeatMs(uint32_t ms);
  uint32_t heartbeatMs() const { return heartbeatMs_; }

 private:
  static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
  bool     advertise_ = false;
  uint8_t  bootLeft_ = 0;
  uint32_t nextBoot_ = 0;
  uint32_t nextHeartbeat_ = 0;
  uint32_t heartbeatMs_ = HEARTBEAT_MS;
  uint8_t  wdpBurstLeft_ = 0;
  uint32_t nextWdpBurst_ = 0;
  uint32_t nextWdpPeriodic_ = 0;
};

}  // namespace wcb
#endif
