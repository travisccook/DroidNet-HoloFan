#include "SagLog.h"
#include <stdio.h>
#include <string.h>

namespace fanlogic {

const char* sagLevelVolts(int level) {
  static const char* const V[SAG_MAX_LEVEL + 1] = {"?", "?", "?", "3.10", "2.92", "2.76", "2.64", "2.51"};
  return level >= SAG_MIN_LEVEL && level <= SAG_MAX_LEVEL ? V[level] : "?";
}

static bool levelsOk(int lo, int hi) { return lo >= SAG_MIN_LEVEL && hi <= SAG_MAX_LEVEL && lo <= hi; }

bool SagLog::begin(int lo, int hi, int seconds) {
  if (!levelsOk(lo, hi) || seconds < SAG_MIN_S || seconds > SAG_MAX_S) return false;
  lo_ = lo;
  hi_ = hi;
  seconds_ = seconds;
  deepest_ = 0;
  det_ = 0;
  memset(watched_, 0, sizeof(watched_));
  memset(hits_, 0, sizeof(hits_));
  memset(perSecond_, 0, sizeof(perSecond_));
  return true;
}

// A 32-bit mix (the murmur3 finaliser) of the slot number: a fixed order
// with no period near any load's.
static uint32_t mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  x *= 0xC2B2AE35u;
  x ^= x >> 16;
  return x;
}

int SagLog::levelAt(uint32_t ms) const {
  uint32_t n = (uint32_t)(hi_ - lo_ + 1);
  return lo_ + (int)(mix(ms / SAG_DWELL_MS) % n);
}

void SagLog::sample(uint32_t ms, int level, bool raw, bool det) {
  if (done(ms) || level < lo_ || level > hi_) return;
  watched_[level]++;
  if (det) det_++;
  if (!raw) return;
  hits_[level]++;
  uint8_t& s = perSecond_[ms / 1000];
  if (level > s) s = (uint8_t)level;
  if (level > deepest_) deepest_ = level;
}

int SagLog::formatSummary(char* out, size_t size) const {
  if (size == 0) return -1;
  int n = snprintf(out, size, "[sag] %d s, ms crossed/watched:", seconds_);
  for (int l = lo_; l <= hi_ && n >= 0 && (size_t)n < size; l++)
    n += snprintf(out + n, size - (size_t)n, " L%d(%sV)=%lu/%lu", l, sagLevelVolts(l), (unsigned long)hits_[l],
                  (unsigned long)watched_[l]);
  if (n >= 0 && (size_t)n < size) n += snprintf(out + n, size - (size_t)n, "; det %lu", (unsigned long)det_);
  return (n < 0 || (size_t)n >= size) ? -1 : n;
}

int SagLog::formatTimeline(int line, char* out, size_t size) const {
  if (line < 0 || line >= timelineLines()) return -1;
  int from = line * SAG_LINE_S;
  int to = from + SAG_LINE_S < seconds_ ? from + SAG_LINE_S : seconds_;
  int n = snprintf(out, size, "[sag] s %d-%d: ", from, to - 1);
  if (n < 0 || (size_t)(n + (to - from)) >= size) return -1;
  for (int s = from; s < to; s++) out[n++] = perSecond_[s] ? (char)('0' + perSecond_[s]) : '-';
  out[n] = '\0';
  return n;
}

bool ProbeLog::begin(int lo, int hi, int dBm) {
  if (!levelsOk(lo, hi) || dBm < PROBE_MIN_DBM || dBm > PROBE_MAX_DBM) return false;
  lo_ = lo;
  hi_ = hi;
  dBm_ = dBm;
  memset(crossed_, 0, sizeof(crossed_));
  memset(idle_, 0, sizeof(idle_));
  return true;
}

void ProbeLog::frame(int level, bool crossed, bool idleCrossed) {
  if (level < lo_ || level > hi_) return;
  if (crossed) crossed_[level]++;
  if (idleCrossed) idle_[level]++;
}

int ProbeLog::format(char* out, size_t size) const {
  if (size == 0) return -1;
  int n = snprintf(out, size, "[probe] %d dBm, %d frames/level, crossed (idle):", dBm_, PROBE_FRAMES);
  for (int l = lo_; l <= hi_ && n >= 0 && (size_t)n < size; l++)
    n += snprintf(out + n, size - (size_t)n, " L%d(%sV)=%u (%u)", l, sagLevelVolts(l), (unsigned)crossed_[l],
                  (unsigned)idle_[l]);
  return (n < 0 || (size_t)n >= size) ? -1 : n;
}

}  // namespace fanlogic
