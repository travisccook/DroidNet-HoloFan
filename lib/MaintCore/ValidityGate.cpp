#include "ValidityGate.h"

namespace maint {

void ValidityGate::begin(bool pendingVerify, bool meshRequired, bool mcuRequired, uint32_t now) {
  active_ = pendingVerify;
  meshRequired_ = meshRequired;
  mcuRequired_ = mcuRequired;
  meshUp_ = mcuOk_ = false;
  start_ = now;
}

Verdict ValidityGate::poll(uint32_t now) {
  if (!active_) return Verdict::NONE;
  bool ok = (!meshRequired_ || meshUp_) && (!mcuRequired_ || mcuOk_);
  if (ok) { active_ = false; return Verdict::VALID; }
  if ((uint32_t)(now - start_) >= VALIDITY_WINDOW_MS) { active_ = false; return Verdict::INVALID; }
  return Verdict::NONE;
}

}  // namespace maint
