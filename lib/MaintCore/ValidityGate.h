// Decides whether a freshly OTA-updated image (bootloader state
// PENDING_VERIFY) is kept or rolled back: it must reach
// "mesh up" (and, on the fan, "MCU answered") within 30 s of boot.
// Pure C++; the adapter maps the verdict to esp_ota_mark_app_valid_cancel_
// rollback() / esp_ota_mark_app_invalid_rollback_and_reboot().
#ifndef VALIDITY_GATE_H
#define VALIDITY_GATE_H
#include <stdint.h>

namespace maint {

constexpr uint32_t VALIDITY_WINDOW_MS = 30000;

enum class Verdict { NONE, VALID, INVALID };

class ValidityGate {
 public:
  // pendingVerify: the running image is in PENDING_VERIFY.
  // meshRequired: false when the mesh is deliberately off (no password set),
  //   so an update to an unconfigured node is not rolled back for that.
  // mcuRequired: true on the fan.
  void begin(bool pendingVerify, bool meshRequired, bool mcuRequired, uint32_t now);
  void meshUp() { meshUp_ = true; }
  void mcuOk() { mcuOk_ = true; }
  // VALID or INVALID exactly once, then NONE forever.
  Verdict poll(uint32_t now);

 private:
  bool     active_ = false;
  bool     meshRequired_ = true, mcuRequired_ = false;
  bool     meshUp_ = false, mcuOk_ = false;
  uint32_t start_ = 0;
};

}  // namespace maint
#endif
