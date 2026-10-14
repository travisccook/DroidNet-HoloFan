#ifndef HOLOFAN_STATUS_H
#define HOLOFAN_STATUS_H
#include <stddef.h>
#include <stdint.h>

// Pure C++ types shared by the HoloFan firmware's command and status layer.
// NO Arduino / ESP-IDF headers here: this library compiles in the native test env.

namespace holofan {

enum class FanState { NOMCU, READY };          // NOMCU: the LED/SD MCU does not answer
enum class Play { UNKNOWN, PLAYING, IDLE };

constexpr size_t CLIP_NAME_MAX = 63;            // longest clip name we keep (bytes, without NUL)
constexpr int    CATALOG_MAX = 32;              // clips kept from the SD listing

struct Status {
  FanState state = FanState::NOMCU;
  int      fileIndex = 0;        // 1-based catalog index of fileName; 0 = none / not in the catalog
  char     fileName[25] = {0};   // as reported by PlayInfo, truncated to 24
  Play     play = Play::UNKNOWN;
  int      progress = 0;
  int      total = 0;
  int      brightness = 0;       // 1-3, 0 = unknown
  int      volume = 0;           // 1-3, 0 = unknown
  bool     loopAll = true;       // true = all, false = one
  int8_t   switcher = -1;        // 1 on, 0 off, -1 unknown (the MCU never reports it)
  char     err[16] = "none";     // result of the last action
};

struct CatalogEntry {
  char name[CLIP_NAME_MAX + 1] = {0}; // exact SD name, e.g. "02-Name.bin"
};

struct Catalog {
  CatalogEntry entries[CATALOG_MAX];
  int count = 0;
};

} // namespace holofan
#endif
