#include "LoudTest.h"
#include <stdio.h>
#include <string.h>
#include "Crc32.h"

namespace fanlogic {

static constexpr uint32_t RECORD_MAGIC = 0x4C4F5544u;   // "LOUD"

const char* loudPhaseName(LoudPhase p) {
  switch (p) {
    case LoudPhase::IDLE: return "IDLE";
    case LoudPhase::PRE: return "PRE";
    case LoudPhase::QUIET: return "QUIET";
    case LoudPhase::STEP: return "STEP";
    case LoudPhase::HOLD: return "HOLD";
    case LoudPhase::RESTORE: return "RESTORE";
    case LoudPhase::REJOIN: return "REJOIN";
    case LoudPhase::DONE: return "DONE";
  }
  return "?";
}

static uint32_t recordCheck(const LoudRecord& r) {
  return crc32Update(0, &r, offsetof(LoudRecord, check));
}

void loudRecordSeal(LoudRecord& r) {
  r.magic = RECORD_MAGIC;
  r.check = recordCheck(r);
}

bool loudRecordValid(const LoudRecord& r) { return r.magic == RECORD_MAGIC && r.check == recordCheck(r); }

int loudRecordFormat(const LoudRecord& r, int resetReason, char* out, size_t size) {
  int n = snprintf(out, size,
                   "[test] RESET during #HLFY%04lu (test %lu): phase %s, %lu.%lu s into the hold, %lu.%lu s into the "
                   "test; rst=%d",
                   (unsigned long)r.param, (unsigned long)r.seq, loudPhaseName((LoudPhase)r.phase),
                   (unsigned long)(r.holdMs / 1000), (unsigned long)(r.holdMs % 1000 / 100),
                   (unsigned long)(r.testMs / 1000), (unsigned long)(r.testMs % 1000 / 100), resetReason);
  return (n < 0 || (size_t)n >= size) ? -1 : n;
}

bool LoudTest::start(int param, int volume, uint32_t now) {
  if (active() || param < 0) return false;
  int r = param / 1000, s = param % 1000;
  if (r > 2 || s < LOUD_MIN_S || s > LOUD_MAX_S) return false;
  mode_ = r;
  radioOff_ = r != 1;
  radioDown_ = false;
  confirmed_ = false;
  restoring_ = false;
  holding_ = false;
  done_ = false;
  holdLen_ = 0;
  tries_ = 0;
  upRead_ = restoreRead_ = 0;
  seconds_ = s;
  restore_ = volume >= 1 && volume <= VOLUME_MAX ? volume : VOLUME_MAX;
  last_ = Step::IDLE;
  start_ = now;
  if (radioOff_) go(Step::RADIO_OFF, now + LOUD_RADIO_OFF_MS);   // off the mesh before the step
  else go(Step::UP, now);
  return true;
}

LoudPhase LoudTest::phase() const {
  switch (step_) {
    case Step::IDLE: return done_ ? LoudPhase::DONE : LoudPhase::IDLE;
    case Step::RADIO_OFF: return LoudPhase::PRE;
    case Step::UP: return radioDown_ ? LoudPhase::QUIET : LoudPhase::PRE;
    case Step::WAIT_UP: return LoudPhase::STEP;
    case Step::HOLD: return LoudPhase::HOLD;
    case Step::RESTORE:
    case Step::WAIT_RESTORE: return LoudPhase::RESTORE;
    case Step::RADIO_ON: return LoudPhase::RESTORE;   // the restore is over; the radio is about to start
    case Step::REPORT: return radioOff_ ? LoudPhase::REJOIN : LoudPhase::RESTORE;
  }
  return LoudPhase::IDLE;
}

void LoudTest::onUp(int v, uint32_t now) {
  upRead_ = v;
  if (v != LOUD_VOLUME) { go(Step::RESTORE, now); return; }   // not loud: no hold, restore at once
  confirmed_ = true;
  holding_ = true;
  holdStart_ = now;
  go(Step::HOLD, now + (uint32_t)seconds_ * 1000u);           // the hold counts from the read-back
}

void LoudTest::onRestore(int v, uint32_t now) {
  restoreRead_ = v;
  tries_++;
  if (v == restore_ || tries_ >= LOUD_RESTORE_TRIES) finishRestore(now);
  else go(Step::RESTORE, now);
}

void LoudTest::finishRestore(uint32_t now) {
  if (radioDown_) go(Step::RADIO_ON, now);
  else go(Step::REPORT, now);
}

void LoudTest::volumeResult(int readBack, uint32_t now) {
  if (step_ == Step::WAIT_UP) onUp(readBack, now);
  else if (step_ == Step::WAIT_RESTORE) onRestore(readBack, now);
}

LoudTest::Action LoudTest::poll(uint32_t now) {
  if (step_ == Step::IDLE) return Action::NONE;
  if (step_ == Step::WAIT_UP && due(now, at_)) onUp(0, now);
  if (restoring_ && (step_ == Step::RESTORE || step_ == Step::WAIT_RESTORE) &&
      due(now, restoreStart_ + LOUD_RESTORE_MAX_MS))
    finishRestore(now);
  if (step_ == Step::WAIT_RESTORE && due(now, at_)) onRestore(0, now);
  if (!due(now, at_)) return Action::NONE;
  switch (step_) {
    case Step::RADIO_OFF:
      radioDown_ = true;
      go(Step::UP, now + LOUD_QUIET_MS);
      return Action::RADIO_OFF;
    case Step::UP:
      last_ = Step::UP;
      go(Step::WAIT_UP, now + LOUD_RESULT_MS);
      return Action::VOLUME_UP;
    case Step::HOLD:
    case Step::RESTORE:
      if (holding_) { holding_ = false; holdLen_ = now - holdStart_; }
      if (!restoring_) { restoring_ = true; restoreStart_ = now; }
      last_ = Step::RESTORE;
      go(Step::WAIT_RESTORE, now + LOUD_RESULT_MS);
      return Action::VOLUME_RESTORE;
    case Step::RADIO_ON:
      radioDown_ = false;
      go(Step::REPORT, now + LOUD_REPORT_MS);
      return Action::RADIO_ON;
    case Step::REPORT:
      done_ = true;
      go(Step::IDLE, now);
      return Action::REPORT;
    case Step::IDLE:
    case Step::WAIT_UP:
    case Step::WAIT_RESTORE:
      break;
  }
  return Action::NONE;
}

void LoudTest::retry(uint32_t now) {
  if (step_ == Step::IDLE || last_ == Step::IDLE) return;
  go(last_, now + LOUD_RETRY_MS);
}

}  // namespace fanlogic
