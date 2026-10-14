// Mesh-independent way into maintenance mode: power-cycle the node three
// times, each on-period 2-10 s, and the third boot enters maintenance (so a
// node whose mesh link is down can still be updated without wires).
// Pure C++; the adapter stores the count in NVS and supplies the reset reason.
//
// Only power-on resets count. Brown-outs (which reset within milliseconds of
// power-up) are excluded twice: by the reset reason and by the 2 s minimum.
// A boot that stays up 10 s clears the count. Acts at most once per boot.
#ifndef POWER_CYCLE_TRIGGER_H
#define POWER_CYCLE_TRIGGER_H
#include <stdint.h>

namespace maint {

constexpr uint32_t PC_COUNT_AFTER_MS = 2000;
constexpr uint32_t PC_CLEAR_AFTER_MS = 10000;
constexpr uint8_t  PC_CYCLES = 3;

class PowerCycleTrigger {
 public:
  enum class Action { NONE, COUNT, ENTER_MAINT, CLEAR };
  // powerOnReset: this boot came from a power-on reset.
  void begin(bool powerOnReset);
  // `stored` is the count kept across boots. COUNT: store newCount().
  // ENTER_MAINT and CLEAR: store 0 (ENTER_MAINT: also enter maintenance).
  Action poll(uint32_t uptimeMs, uint8_t stored);
  uint8_t newCount() const { return newCount_; }

 private:
  bool    active_ = false;
  bool    counted_ = false;
  bool    cleared_ = false;
  uint8_t newCount_ = 0;
};

}  // namespace maint
#endif
