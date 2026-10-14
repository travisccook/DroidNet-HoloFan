// #HLFE<lo><hi><seconds> (bench, 0.9.6): a supply-sag meter made from the
// ESP32-C2's brownout comparator. The comparator's threshold steps through
// levels lo..hi (3 = 3.10 V, 4 = 2.92, 5 = 2.76, 6 = 2.64, 7 = 2.51 V:
// Espressif's estimates, chip-to-chip spread unknown), SAG_DWELL_MS each in
// a pseudo-random order (0.9.7: a fixed rotation phase-locked onto the 10 s
// heartbeat); a 1 ms poller reads the comparator's latched event bit, and
// its DET bit (semantics undocumented) is counted apart. The comparator
// watches the ESP's own supply pin. Level 2 (3.27 V) is left out (0.9.7):
// it sits next to the supply, and a reset keeps the threshold, so a stale 2
// could hold the bootloader in brownout resets.
// #HLFJ<lo><hi><dBm> (0.9.7): ProbeLog counts, per level, the probe frames
// whose burst crossed it and the idle windows that did (the background).
// Pure C++: no ESP-IDF headers, so the native env tests it.
#ifndef FAN_SAG_LOG_H
#define FAN_SAG_LOG_H
#include <stddef.h>
#include <stdint.h>

namespace fanlogic {

constexpr int      SAG_MIN_LEVEL = 3;
constexpr int      SAG_MAX_LEVEL = 7;
constexpr uint32_t SAG_DWELL_MS = 25;
constexpr int      SAG_MIN_S = 10;
constexpr int      SAG_MAX_S = 300;
constexpr int      SAG_LINE_S = 100;     // timeline seconds per report line
constexpr int      PROBE_FRAMES = 20;    // probe frames per level
constexpr int      PROBE_MIN_DBM = 2;
constexpr int      PROBE_MAX_DBM = 20;

const char* sagLevelVolts(int level);    // "3.10" .. "2.51", "?" outside 3..7

class SagLog {
 public:
  // False (and no change) if a level is outside 3..7, lo > hi, or seconds
  // outside SAG_MIN_S..SAG_MAX_S.
  bool begin(int lo, int hi, int seconds);
  // The level to watch `ms` after begin(): one per SAG_DWELL_MS slot, in a
  // fixed pseudo-random order.
  int levelAt(uint32_t ms) const;
  bool done(uint32_t ms) const { return ms >= (uint32_t)seconds_ * 1000u; }
  // One poll `ms` after begin(): the threshold was `level`; `raw` = the
  // latched interrupt bit was set (the supply went under it since the last
  // poll); `det` = the comparator's DET bit read set.
  void sample(uint32_t ms, int level, bool raw, bool det);
  // The deepest (highest-numbered) level crossed so far (raw), 0 if none.
  int deepest() const { return deepest_; }

  // "[sag] 60 s, ms crossed/watched: L3(3.10V)=12/15000 ... ; det 0"
  int formatSummary(char* out, size_t size) const;
  // Timeline lines, SAG_LINE_S seconds each: "[sag] s 0-99: --3-5-..." with
  // one character per second, the deepest level crossed in it ('-' none).
  // Returns the length, or -1 past the last line.
  int timelineLines() const { return (seconds_ + SAG_LINE_S - 1) / SAG_LINE_S; }
  int formatTimeline(int line, char* out, size_t size) const;

 private:
  int      lo_ = 3, hi_ = 3, seconds_ = 0, deepest_ = 0;
  uint32_t watched_[SAG_MAX_LEVEL + 1] = {0};
  uint32_t hits_[SAG_MAX_LEVEL + 1] = {0};
  uint32_t det_ = 0;
  uint8_t  perSecond_[SAG_MAX_S] = {0};   // deepest level crossed in each second (0 none)
};

class ProbeLog {
 public:
  // False if a level is outside 3..7, lo > hi, or dBm outside 2..20.
  bool begin(int lo, int hi, int dBm);
  int levels() const { return hi_ - lo_ + 1; }
  int levelOf(int i) const { return lo_ + i; }
  int dBm() const { return dBm_; }
  // One probe at `level`: its burst crossed it; the idle window before it did.
  void frame(int level, bool crossed, bool idleCrossed);
  // "[probe] 20 dBm, 20 frames/level, crossed (idle): L3(3.10V)=20 (1) ..."
  int format(char* out, size_t size) const;

 private:
  int      lo_ = 3, hi_ = 3, dBm_ = 0;
  uint16_t crossed_[SAG_MAX_LEVEL + 1] = {0};
  uint16_t idle_[SAG_MAX_LEVEL + 1] = {0};
};

}  // namespace fanlogic
#endif
