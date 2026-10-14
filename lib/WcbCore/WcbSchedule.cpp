// Portions derived from WCB_Client (c) 2026 Greg Hulette, MIT License.
#include "WcbSchedule.h"
#include "WcbPacket.h"

namespace wcb {

void Schedule::arm(uint32_t now, uint8_t id, bool advertise) {
  advertise_ = advertise;
  bootLeft_ = BOOT_ANNOUNCES;
  nextBoot_ = now;
  nextHeartbeat_ = now;
  wdpBurstLeft_ = advertise ? WDP_BURST : 0;
  nextWdpBurst_ = now + WDP_FIRST_MS;
  nextWdpPeriodic_ = now + WDP_PERIOD_MS + (uint32_t)(id % 16) * 500u;
}

void Schedule::solicit(uint32_t now, uint8_t id) {
  if (!advertise_) return;
  uint32_t at = now + (uint32_t)(id % 16) * 40u;
  if (wdpBurstLeft_ < 1) { wdpBurstLeft_ = 1; nextWdpBurst_ = at; }
  else if ((int32_t)(at - nextWdpBurst_) < 0) nextWdpBurst_ = at;
}

uint8_t Schedule::due(uint32_t now) {
  uint8_t act = 0;
  if (bootLeft_ > 0 && reached(now, nextBoot_)) {
    act |= ACT_BOOT;
    bootLeft_--;
    nextBoot_ = now + BOOT_GAP_MS;
  }
  if (reached(now, nextHeartbeat_)) {
    act |= ACT_HEARTBEAT;
    nextHeartbeat_ = now + heartbeatMs_;
  }
  if (advertise_) {
    if (wdpBurstLeft_ > 0 && reached(now, nextWdpBurst_)) {
      act |= ACT_WDP;
      wdpBurstLeft_--;
      nextWdpBurst_ = now + WDP_GAP_MS;
    }
    if (reached(now, nextWdpPeriodic_)) {
      act |= ACT_WDP;
      nextWdpPeriodic_ = now + WDP_PERIOD_MS;
    }
  }
  return act;
}

}  // namespace wcb

namespace wcb {

void Schedule::setHeartbeatMs(uint32_t ms) {
  if (ms == 0) ms = HEARTBEAT_MS;
  if (ms < HEARTBEAT_MIN_MS) ms = HEARTBEAT_MIN_MS;
  if (ms > HEARTBEAT_MAX_MS) ms = HEARTBEAT_MAX_MS;
  heartbeatMs_ = ms;
}

}  // namespace wcb
