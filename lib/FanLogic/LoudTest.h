// #HLFY<r><seconds> (bench, 0.9.4): volume 3 past the cap for 10-300 s with
// the ESP's radio on (r=1), fully off (r=0) or receiving only (r=2: off the
// mesh, Wi-Fi started; 0.9.6), then back to the old volume. A brownout with
// the radio off means the rest of the fan (amplifier, LEDs, MCU) pulls the
// supply under the ESP's brownout level; surviving it means the radio is the
// tipping point (docs/HOW_IT_WORKS.md).
// With r=0/2 the radio leaves before the volume step (0.9.6), so the step
// and the whole hold run without it. Every volume change is read back from
// the MCU before the next step, and the radio comes back once the old volume
// reads back, or after LOUD_RESTORE_MAX_MS whatever the MCU reports (the fan
// must never stay off the mesh). phase()/holdMs() feed a record that
// survives a software reset (LoudRecord), so a brownout can be placed.
// Pure C++: the mesh task starts it, polls it, carries out each Action and
// hands back each read-back with volumeResult().
#ifndef FAN_LOUD_TEST_H
#define FAN_LOUD_TEST_H
#include <stddef.h>
#include <stdint.h>
#include "McuProto.h"

namespace fanlogic {

// Longest wait for one read-back. 0.9.8 review: setVolumeChecked can take
// 2 x (ACT_SETTLE_MAX_MS + VOLUME_READS x (SETTING_TIMEOUT_MS +
// QUERY_SETTLE_MAX_MS)) = 14.4 s, plus an MCU-task pass (FanMesh.cpp checks).
constexpr uint32_t LOUD_RESULT_MS = 22000;
constexpr uint32_t LOUD_RADIO_OFF_MS = 300;      // r=0/2: radio off this long after the start (start line out)
constexpr uint32_t LOUD_QUIET_MS = 200;          // r=0/2: then the volume step this long after the radio left
constexpr int      LOUD_RESTORE_TRIES = 3;       // restore requests before giving up on the read-back
constexpr uint32_t LOUD_RESTORE_MAX_MS = 25000;  // radio back on by then, whatever the MCU says
constexpr uint32_t LOUD_REPORT_MS = 1000;        // after RADIO_ON: the closing line, once the mesh is back
constexpr uint32_t LOUD_RETRY_MS = 200;
constexpr int      LOUD_MIN_S = 10;
constexpr int      LOUD_MAX_S = 300;
constexpr int      LOUD_VOLUME = 3;

// Where a test is, for the reset record: PRE (nothing changed yet), QUIET
// (radio off, still the old volume), STEP ("m 3" sent, not read back), HOLD
// (3 read back), RESTORE (old volume sent), REJOIN (radio restarted), DONE.
enum class LoudPhase : uint8_t { IDLE, PRE, QUIET, STEP, HOLD, RESTORE, REJOIN, DONE };
const char* loudPhaseName(LoudPhase p);

// The running test, kept in RAM that a software reset does not clear (the
// brownout handler resets in software): the next boot can tell where a
// reset came. Valid only when sealed (magic + CRC), so power-on garbage
// does not count.
struct LoudRecord {
  uint32_t magic;
  uint32_t seq;          // test number since the record was first sealed
  uint32_t param;        // r * 1000 + seconds
  uint32_t testMs;       // since #HLFY was accepted
  uint32_t holdMs;       // since 3 was read back (0 before)
  uint8_t  phase;        // LoudPhase
  uint8_t  reported;     // the next boot has reported it
  uint8_t  pad[2];
  uint32_t check;
};
void loudRecordSeal(LoudRecord& r);
bool loudRecordValid(const LoudRecord& r);
// "[test] RESET during #HLFY0300 (test 7): phase HOLD, 43.2 s into the hold,
// 45.7 s into the test; rst=9". Returns the length, or -1 if it did not fit.
int loudRecordFormat(const LoudRecord& r, int resetReason, char* out, size_t size);

class LoudTest {
 public:
  enum class Action { NONE, VOLUME_UP, RADIO_OFF, VOLUME_RESTORE, RADIO_ON, REPORT };
  // param = r * 1000 + seconds (CmdType::LOUD_TEST); volume = the MCU's
  // volume now (restored afterwards; outside 1..cap: cap, the volume cap,
  // 3 or VOLUME_MAX).
  // False if a test is running or param is out of range.
  bool start(int param, int volume, uint32_t now, int cap = VOLUME_MAX);
  // The next action due at `now`, at most one per call:
  //   VOLUME_UP       set volume 3 and read it back (answer with volumeResult)
  //   RADIO_OFF       r=0, after 3 was read back: leave the mesh, radio off
  //   VOLUME_RESTORE  set restoreVolume() and read it back (volumeResult)
  //   RADIO_ON        r=0: radio back on, rejoin the mesh
  //   REPORT          the test is over (confirmed(), the read-backs); no ESP reset
  Action poll(uint32_t now);
  // The read-back for the last VOLUME_UP / VOLUME_RESTORE (0 = the MCU did
  // not report one). Ignored when none is awaited.
  void volumeResult(int readBack, uint32_t now);
  // The last VOLUME_UP / VOLUME_RESTORE could not be queued: repeat it
  // LOUD_RETRY_MS later (a restore stays bounded by LOUD_RESTORE_MAX_MS).
  void retry(uint32_t now);
  // Stop at once (only safe before VOLUME_UP took effect).
  void cancel() { step_ = Step::IDLE; done_ = false; }

  bool active() const { return step_ != Step::IDLE; }
  bool radioOff() const { return radioOff_; }         // r=0 or r=2: off the mesh during the test
  int mode() const { return mode_; }                  // r
  LoudPhase phase() const;
  uint32_t testMs(uint32_t now) const { return now - start_; }                          // since start()
  uint32_t holdMs(uint32_t now) const { return holding_ ? now - holdStart_ : holdLen_; }  // at 3, so far
  bool radioDown() const { return radioDown_; }       // between RADIO_OFF and RADIO_ON
  int seconds() const { return seconds_; }
  int restoreVolume() const { return restore_; }
  bool confirmed() const { return confirmed_; }       // the MCU reported 3: the hold ran
  int upReadBack() const { return upRead_; }
  int restoreReadBack() const { return restoreRead_; }

 private:
  enum class Step { IDLE, RADIO_OFF, UP, WAIT_UP, HOLD, RESTORE, WAIT_RESTORE, RADIO_ON, REPORT };
  static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
  void go(Step s, uint32_t at) { step_ = s; at_ = at; }
  void onUp(int v, uint32_t now);
  void onRestore(int v, uint32_t now);
  void finishRestore(uint32_t now);

  Step     step_ = Step::IDLE;
  bool     done_ = false;           // a test ran to REPORT (phase DONE until the next start)
  uint32_t start_ = 0;
  uint32_t holdStart_ = 0;
  uint32_t holdLen_ = 0;
  bool     holding_ = false;
  int      mode_ = 1;
  Step     last_ = Step::IDLE;      // the last action step, for retry()
  uint32_t at_ = 0;
  uint32_t holdEnd_ = 0;
  uint32_t restoreStart_ = 0;
  bool     restoring_ = false;
  int      tries_ = 0;
  bool     radioOff_ = false;
  bool     radioDown_ = false;
  bool     confirmed_ = false;
  int      seconds_ = 0;
  int      restore_ = VOLUME_MAX;
  int      upRead_ = 0;
  int      restoreRead_ = 0;
};

}  // namespace fanlogic
#endif
