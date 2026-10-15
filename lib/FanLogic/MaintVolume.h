// The fan's volume around maintenance mode (HoloFan 0.9.12). Maintenance is
// Wi-Fi at full transmit power, so the fan plays at volume 1 while it is in
// maintenance and gets its volume back afterwards, also when maintenance ends
// in the reboot after an OTA: the record lives in .noinit, which survives a
// software reset.
#ifndef FANLOGIC_MAINT_VOLUME_H
#define FANLOGIC_MAINT_VOLUME_H
#include <stdint.h>

namespace fanlogic {

constexpr int MAINT_VOLUME = 1;            // the volume while in maintenance
constexpr int MAINT_RESTORE_UNKNOWN = -1;  // maintVolumeLeave: the old volume was not known; unmute (top volume)

struct MaintVolumeRecord {
  uint32_t magic;
  uint8_t  volume;       // to put back: 1-3, or 0 = not known when maintenance started
  uint8_t  pad[3];
  uint32_t check;
};

constexpr uint32_t MAINT_VOLUME_MAGIC = 0x4D564F4Cu;   // "MVOL"

inline uint32_t maintVolumeCheck(const MaintVolumeRecord& r) {
  return (r.magic ^ 0x5A5A5A5Au) + (uint32_t)r.volume * 0x01010101u + r.pad[0] + r.pad[1] + r.pad[2];
}

inline bool maintVolumeValid(const MaintVolumeRecord& r) {
  return r.magic == MAINT_VOLUME_MAGIC && r.volume <= 3 && r.check == maintVolumeCheck(r);
}

inline void maintVolumeClear(MaintVolumeRecord& r) { r = MaintVolumeRecord{}; }

// Entering maintenance with the MCU at `volume` (1-3; anything else: not
// known). Remembers it in r, unless r still holds a volume that was never
// put back (maintenance again within seconds of a return): that one stays.
// Returns the volume to set now, MAINT_VOLUME, or 0 when it is that already.
inline int maintVolumeEnter(MaintVolumeRecord& r, int volume) {
  if (volume < 1 || volume > 3) volume = 0;
  if (!maintVolumeValid(r)) {
    r = MaintVolumeRecord{};
    r.magic = MAINT_VOLUME_MAGIC;
    r.volume = (uint8_t)volume;
    r.check = maintVolumeCheck(r);
  }
  return volume == MAINT_VOLUME ? 0 : MAINT_VOLUME;
}

// Back on the mesh, or the first boot after maintenance: the volume to set
// (2 or 3), MAINT_RESTORE_UNKNOWN, or 0 (no valid record, or it was 1 before).
// Clears r.
inline int maintVolumeLeave(MaintVolumeRecord& r) {
  if (!maintVolumeValid(r)) return 0;
  int v = r.volume;
  maintVolumeClear(r);
  return v == 0 ? MAINT_RESTORE_UNKNOWN : v == MAINT_VOLUME ? 0 : v;
}

}  // namespace fanlogic
#endif
