#include "PowerCycleTrigger.h"

namespace maint {

void PowerCycleTrigger::begin(bool powerOnReset) {
  powerOn_ = powerOnReset;
  active_ = powerOnReset;
  counted_ = cleared_ = false;
  newCount_ = 0;
}

PowerCycleTrigger::Action PowerCycleTrigger::poll(uint32_t uptimeMs, uint8_t stored) {
  if (!powerOn_) {
    // Not a power cycle, so it never counts. But a count left by earlier
    // power cycles must not outlive a boot that runs normally: clear it.
    if (!cleared_ && stored != 0 && uptimeMs >= PC_CLEAR_AFTER_MS) {
      cleared_ = true;
      return Action::CLEAR;
    }
    return Action::NONE;
  }
  if (!active_) return Action::NONE;
  if (!counted_ && uptimeMs >= PC_COUNT_AFTER_MS) {
    counted_ = true;
    uint8_t prev = stored < PC_CYCLES ? stored : 0;     // garbage restarts the count
    newCount_ = (uint8_t)(prev + 1);
    if (newCount_ >= PC_CYCLES) {
      active_ = false;
      return Action::ENTER_MAINT;
    }
    return Action::COUNT;
  }
  if (counted_ && !cleared_ && uptimeMs >= PC_CLEAR_AFTER_MS) {
    cleared_ = true;
    active_ = false;
    return Action::CLEAR;
  }
  return Action::NONE;
}

}  // namespace maint
