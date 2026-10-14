#include "FanController.h"
#include <stdio.h>
#include <string.h>

namespace fanlogic {

using holofan::CmdType;
using holofan::FanState;

static constexpr size_t REPLY_BUF = 1024;   // a 32-name listing is about 450 bytes
enum : uint8_t { OV_VOLUME = 1, OV_LOOP = 2, OV_BRIGHTNESS = 4, OV_CLIP = 8 };   // overridden_ bits

void FanController::setErr(const char* e) {
  strncpy(st_.err, e, sizeof(st_.err) - 1);
  st_.err[sizeof(st_.err) - 1] = '\0';
}

static void appendf(char* out, size_t size, const char* fmt, const char* a, const char* b) {
  size_t n = strlen(out);
  if (n + 1 >= size) return;
  snprintf(out + n, size - n, fmt, n ? " " : "", a, b);
}

static void appendNum(char* out, size_t size, const char* name, int a, int b) {
  char x[12], y[12], fmt[24];
  snprintf(x, sizeof(x), "%d", a);
  snprintf(y, sizeof(y), "%d", b);
  snprintf(fmt, sizeof(fmt), "%%s%s %%s->%%s", name);
  appendf(out, size, fmt, x, y);
}

// The fields of `now` that differ from `was`, in a fixed order.
static void diffSettings(const Settings& was, const Settings& now, char* out, size_t size) {
  if (was.ble != now.ble) appendf(out, size, "%sble %s->%s", was.ble ? "on" : "off", now.ble ? "on" : "off");
  if (was.loopAll != now.loopAll)
    appendf(out, size, "%sloop %s->%s", was.loopAll ? "all" : "one", now.loopAll ? "all" : "one");
  if (was.brightness != now.brightness) appendNum(out, size, "br", was.brightness, now.brightness);
  if (was.volume != now.volume) appendNum(out, size, "vol", was.volume, now.volume);
  if (was.angle != now.angle) appendNum(out, size, "angle", was.angle, now.angle);
}

bool FanController::querySettings(char* diff, size_t diffSize) {
  char line[MCU_LINE_MAX], reply[REPLY_BUF] = {0};
  if (buildLine("get_setting", nullptr, 0, line, sizeof(line)) < 0) return false;
  link_.transact(line, "setting ", SETTING_TIMEOUT_MS, reply, sizeof(reply));
  Settings s;
  if (!parseSetting(reply, s)) {
    settle(QUERY_SETTLE_QUIET_MS, QUERY_SETTLE_MAX_MS);   // it may still be chewing on the line
    return false;
  }
  if (diff && diffSize && baseValid_) diffSettings(base_, s, diff, diffSize);
  if (!s.loopAll && (!baseValid_ || base_.loopAll)) clipKnown_ = false;   // loop one starts a fresh clip baseline
  if (muted_ && s.volume != 1) muted_ = false;      // someone changed it since our mute
  settingsRead_ = true;
  if (bleStuck_ && s.ble == (bleWant_ == 1)) bleStuck_ = false;   // it took after all: watch it again
  base_ = s;
  baseValid_ = true;
  st_.brightness = s.brightness;
  st_.volume = s.volume;
  st_.loopAll = s.loopAll;
  answered_ = true;
  return true;
}

void FanController::report(const char* text) {
  if (!text[0]) return;
  unpromptedCount_++;
  size_t n = strlen(unprompted_);
  snprintf(unprompted_ + n, sizeof(unprompted_) - n, "%s%s", n ? "; " : "", text);
}

bool FanController::takeUnprompted(char* out, size_t size) {
  if (!unprompted_[0] || size == 0) return false;
  strncpy(out, unprompted_, size - 1);
  out[size - 1] = '\0';
  unprompted_[0] = '\0';
  return true;
}

bool FanController::switchBle() {
  const bool want = bleWant_ == 1;
  for (int i = 0; i < 2; i++) {                  // one resend: the MCU drops input while busy (as capVolume)
    if (!act(want ? "c" : "d")) return false;
    bool read = false;
    for (int r = 0; r < VOLUME_READS && !read; r++) read = querySettings();
    if (!read) { baseValid_ = false; return false; }   // unverified: the next read tries again
    if (base_.ble == want) return true;
  }
  bleStuck_ = true;
  char text[UNPROMPTED_MAX];
  snprintf(text, sizeof(text), "ble did not switch %s (MCU reports %s)", want ? "on" : "off", base_.ble ? "on" : "off");
  report(text);
  return false;
}

void FanController::beginLease() {
  if (restoreArmed_ || clipPending_) { freshSnapshot_ = false; return; }   // keep the pending snapshot
  pre_ = PreLease();
  pre_.valid = st_.state == FanState::READY;
  strncpy(pre_.clip, playing_, sizeof(pre_.clip) - 1);
  pre_.playing = st_.play == holofan::Play::PLAYING;
  pre_.loopAll = st_.loopAll;
  pre_.loopKnown = settingsRead_;
  pre_.volume = st_.volume;
  pre_.brightness = settingsRead_ ? st_.brightness : 0;
  freshSnapshot_ = true;
}

void FanController::endLease(bool changed) {
  if (!changed) return;                          // nothing to put back (a pending restore stays)
  if (freshSnapshot_) overridden_ = 0;           // else commands between the leases still win
  restoreArmed_ = pre_.valid;
  clipPending_ = false;
  deadlineSet_ = false;                          // set at the first poll (now_ is stale during a lease)
  soonTries_ = SOON_TRIES;
}

static void appendWord(char* out, size_t size, const char* word) {
  size_t n = strlen(out);
  if (n + 1 < size) snprintf(out + n, size - n, "%s%s", n ? " " : "", word);
}

void FanController::reportLease(const char* text) {
  if (!text[0]) return;
  size_t n = strlen(leaseReport_);
  snprintf(leaseReport_ + n, sizeof(leaseReport_) - n, "%s%s", n ? "; " : "", text);
}

bool FanController::takeLeaseReport(char* out, size_t size) {
  if (!leaseReport_[0] || size == 0) return false;
  strncpy(out, leaseReport_, size - 1);
  out[size - 1] = '\0';
  leaseReport_[0] = '\0';
  return true;
}

void FanController::restoreSettingsAfterLease() {
  char done[UNPROMPTED_MAX] = {0}, w[24], num[12];
  loopChangedByMcu_ = pre_.loopKnown && st_.loopAll != pre_.loopAll;
  if (!(overridden_ & OV_VOLUME) && pre_.volume >= 1 && pre_.volume <= cap_ && st_.volume != pre_.volume) {
    snprintf(num, sizeof(num), "%d", pre_.volume);
    if (act("m", num)) {
      st_.volume = base_.volume = pre_.volume;
      snprintf(w, sizeof(w), "vol %d", pre_.volume);
      appendWord(done, sizeof(done), w);
    }
  }
  if (!(overridden_ & OV_LOOP) && pre_.loopKnown && st_.loopAll != pre_.loopAll && act(pre_.loopAll ? "j" : "i")) {
    st_.loopAll = base_.loopAll = pre_.loopAll;
    if (!pre_.loopAll) clipKnown_ = false;
    appendWord(done, sizeof(done), pre_.loopAll ? "loop all" : "loop one");
  }
  if (!(overridden_ & OV_BRIGHTNESS) && pre_.brightness >= 1 && pre_.brightness <= 3 &&
      st_.brightness != pre_.brightness) {
    snprintf(num, sizeof(num), "%d", pre_.brightness);
    if (act("k", num)) {
      st_.brightness = base_.brightness = pre_.brightness;
      snprintf(w, sizeof(w), "br %d", pre_.brightness);
      appendWord(done, sizeof(done), w);
    }
  }
  if (done[0]) {
    char text[UNPROMPTED_MAX];
    snprintf(text, sizeof(text), "restored %.80s", done);
    reportLease(text);
  }
}

int FanController::clipOnCard(const char* name) {
  char line[MCU_LINE_MAX];
  if (buildLine("s", nullptr, 0, line, sizeof(line)) < 0) return -1;
  SlotFinder f(name);
  if (link_.transactChunks(line, LIST_TIMEOUT_MS, &SlotFinder::feedThunk, &f) <= 0) return -1;
  f.finish();
  return f.found() ? 1 : 0;
}

// The clip part of a restore (after the settings part): one try per poll.
void FanController::restoreClip() {
  if (!clipPending_) return;
  if ((overridden_ & OV_CLIP) || !pre_.clip[0]) { clipPending_ = false; return; }
  bool parsed = refreshPlay() && lastPlayParsed_;
  if (!parsed) {                                 // where is it? ask again at the next poll
    if (++clipTries_ >= CLIP_RESTORE_TRIES) clipPending_ = false;
    return;
  }
  bool playing = st_.play == holofan::Play::PLAYING;
  if (pre_.playing && pre_.loopAll && st_.loopAll && playing && !loopChangedByMcu_) {
    clipPending_ = false;                        // loop all moves on by itself: any clip is right
    return;
  }
  bool sameClip = strcmp(playing_, pre_.clip) == 0;
  if (sameClip && playing == pre_.playing) { clipPending_ = false; return; }
  char w[holofan::CLIP_NAME_MAX + 24];
  if (!(sameClip && !pre_.playing)) {            // the clip itself must change
    int on = clipOnCard(pre_.clip);
    if (on < 0) {
      if (++clipTries_ >= CLIP_RESTORE_TRIES) clipPending_ = false;
      return;
    }
    if (on == 0) { clipPending_ = false; return; }   // deleted, or a format
    expectClip_ = true;
    expectClipUntil_ = now_ + EXPECT_CLIP_MS;
    if (!act("t", pre_.clip)) { clipPending_ = false; return; }
  }
  if (!pre_.playing) act("h");                   // it was paused (or idle) before the lease
  snprintf(w, sizeof(w), "restored clip %.40s%s", pre_.clip, pre_.playing ? "" : " (paused)");
  reportLease(w);
  clipPending_ = false;
}

void FanController::forgetBaseline() {
  overridden_ = 0xFF;                            // a raw line wins over a pending restore
  baseValid_ = false;
  clipKnown_ = false;
  nextSettings_ = now_;                          // new baseline at the next poll
  expectClip_ = true;
  expectClipUntil_ = now_ + EXPECT_CLIP_MS;
}

// Every SETTINGS_POLL_MS while READY (and soon after a clip lease): report
// what changed behind our back and cap a 3 unless #HLFY holds it.
void FanController::periodicSettings(uint32_t now) {
  if (restoreArmed_ || clipPending_) {
    if (!deadlineSet_) { restoreDeadline_ = now + RESTORE_MAX_MS; deadlineSet_ = true; }
    else if (due(now, restoreDeadline_)) { restoreArmed_ = false; clipPending_ = false; }
  }
  if (clipPending_ && !restoreArmed_) restoreClip();   // a retry of the clip part
  bool soon = soonTries_ > 0;
  if (!soon && !due(now, nextSettings_)) return;
  nextSettings_ = now + SETTINGS_POLL_MS;
  bool hadBase = baseValid_;
  char diff[UNPROMPTED_MAX] = {0};
  if (!querySettings(diff, sizeof(diff))) {
    if (soon) soonTries_--;
    return;
  }
  soonTries_ = 0;
  bool late = !bootSettingsKnown_ && bootDone_;
  if (late) {                                    // READY came through PlayInfo alone: the boot record gets this
    bootSettingsKnown_ = true;
    bootSettingsLate_ = true;
    bootSettings_ = base_;
  }
  if (st_.volume > cap_ && !loudAllowed_) {         // the cap first: volume 3 is the safety one
    int was = st_.volume;
    capVolume();
    if (late) bootCapTo_ = baseValid_ ? st_.volume : -1;
    size_t n = strlen(diff);
    if (n) snprintf(diff + n, sizeof(diff) - n, " (capped)");
    else snprintf(diff, sizeof(diff), "vol %d (capped; %s)", was, hadBase ? "again" : "no baseline");
  }
  if (bleWant_ >= 0 && !bleStuck_ && baseValid_ && base_.ble != (bleWant_ == 1) && switchBle()) {
    size_t n = strlen(diff);
    if (n) snprintf(diff + n, sizeof(diff) - n, " (switched %s)", bleWant_ ? "on" : "off");
    else snprintf(diff, sizeof(diff), "ble %s (switched %s)", bleWant_ ? "off" : "on", bleWant_ ? "on" : "off");
  }
  if (late) bootLate_ = true;
  if (!restoreArmed_) { report(diff); return; }
  restoreArmed_ = false;                         // the first read after a clip lease
  if (diff[0]) reportLease(diff);
  restoreSettingsAfterLease();
  clipPending_ = true;
  clipTries_ = 0;
  restoreClip();
  if (diff[0] || leaseReport_[0]) leaseChanges_++;
}

bool FanController::refreshPlay() {
  char line[MCU_LINE_MAX], reply[REPLY_BUF] = {0};
  if (buildLine("get_playinfo", nullptr, 0, line, sizeof(line)) < 0) return false;
  link_.transact(line, "PlayInfo ", PLAYINFO_TIMEOUT_MS, reply, sizeof(reply));
  bool alive = strstr(reply, "PlayInfo") != nullptr;   // proof of life even if it does not parse
  lastPlayParsed_ = false;
  if (alive) answered_ = true;
  else settle(QUERY_SETTLE_QUIET_MS, QUERY_SETTLE_MAX_MS);
  PlayInfo p;
  if (!parsePlayInfo(reply, p)) return alive;
  lastPlayParsed_ = true;
  if (expectClip_ && due(now_, expectClipUntil_)) expectClip_ = false;
  if (clipKnown_ && baseValid_ && !expectClip_ && !st_.loopAll && p.name[0] && playing_[0] &&
      strcmp(playing_, p.name) != 0) {
    char text[UNPROMPTED_MAX];
    snprintf(text, sizeof(text), "clip %.40s->%.40s", playing_, p.name);
    report(text);
  }
  clipKnown_ = p.name[0] != '\0';
  strcpy(playing_, p.name);
  strncpy(st_.fileName, p.name, sizeof(st_.fileName) - 1);
  st_.fileName[sizeof(st_.fileName) - 1] = '\0';
  for (char* c = st_.fileName; *c; c++) {              // replies are plain ASCII, no '^' (WCB reply delimiter)
    unsigned char b = (unsigned char)*c;
    if (b < 0x21 || b > 0x7E || b == '^') *c = '?';
  }
  st_.play = p.playing ? holofan::Play::PLAYING : holofan::Play::IDLE;
  st_.progress = p.progress;
  st_.total = p.total;
  updateIndex();
  return true;
}

void FanController::refreshCatalog() {
  char line[MCU_LINE_MAX], reply[REPLY_BUF] = {0};
  if (buildLine("s", nullptr, 0, line, sizeof(line)) < 0) return;
  if (link_.transact(line, nullptr, LIST_TIMEOUT_MS, reply, sizeof(reply)) <= 0) {
    setErr("timeout");                 // keep the old catalog
    return;
  }
  parseList(reply, cat_);
  updateIndex();
  setErr("none");
}

void FanController::updateIndex() {
  st_.fileIndex = 0;
  for (int i = 0; i < cat_.count; i++)
    if (playing_[0] && strcmp(cat_.entries[i].name, playing_) == 0) { st_.fileIndex = i + 1; return; }
}

bool FanController::act(const char* cmd, const char* arg) {
  char line[MCU_LINE_MAX];
  const char* args[1] = {arg};
  if (buildLine(cmd, args, arg ? 1 : 0, line, sizeof(line)) < 0) { setErr("badname"); return false; }
  if (!link_.send(line)) { setErr("txfail"); return false; }
  link_.collect(nullptr, 0, ACT_SETTLE_QUIET_MS, ACT_SETTLE_MAX_MS);   // its "mculog:" line, if any, is logged
  setErr("none");
  return true;
}

void FanController::setVolumeCap(int cap) {
  cap_ = cap == 3 ? 3 : VOLUME_MAX;
  if (preMuteVolume_ > cap_) preMuteVolume_ = cap_;
}

// A volume above the cap (the MCU powers up at 3, and its IR remote can set
// 3) is lowered and read back: the MCU drops input while busy, so one
// resend if it did not take.
void FanController::capVolume() {
  char num[12];
  snprintf(num, sizeof(num), "%d", cap_);
  for (int i = 0; i < 2 && st_.volume > cap_; i++) {
    if (!act("m", num)) return;
    st_.volume = cap_;                           // the read-back corrects it if the MCU dropped the line
    bool read = false;
    for (int r = 0; r < VOLUME_READS && !read; r++) read = querySettings();
    if (!read) { baseValid_ = false; return; }   // unverified: the next read is a new baseline, not a report
  }
}

int FanController::formatBoot(char* out, size_t size) const {
  if (size == 0) return -1;
  int n;
  if (bootSettingsKnown_) {
    char cap[32] = "";
    if (bootCapTo_ > 0) snprintf(cap, sizeof(cap), " (capped to %d)", bootCapTo_);
    else if (bootCapTo_ < 0) snprintf(cap, sizeof(cap), " (cap sent, not read back)");
    n = snprintf(out, size, bootSettingsLate_ ? "[fan] MCU first settings (read late): ble %s, loop %s, br %d, vol %d%s, angle %d"
                                              : "[fan] MCU at connect: ble %s, loop %s, br %d, vol %d%s, angle %d",
                 bootSettings_.ble ? "on" : "off", bootSettings_.loopAll ? "all" : "one", bootSettings_.brightness,
                 bootSettings_.volume, cap, bootSettings_.angle);
  } else {
    n = snprintf(out, size, "[fan] MCU at connect: settings not read");
  }
  if (n < 0 || (size_t)n >= size) return -1;
  if (bootPlayKnown_) n += snprintf(out + n, size - (size_t)n, "; playing %s %d/%d", bootClip_, bootProgress_, bootTotal_);
  else n += snprintf(out + n, size - (size_t)n, "; clip ?");
  return (n < 0 || (size_t)n >= size) ? -1 : n;
}

int FanController::setVolumeChecked(int v) {
  if (v < 1 || v > 3) return 0;
  char num[12];
  snprintf(num, sizeof(num), "%d", v);
  if (st_.state != FanState::READY) {            // NOMCU: still lower it (no read-back), never raise it
    char line[MCU_LINE_MAX];
    const char* args[1] = {num};
    if (v <= cap_ && buildLine("m", args, 1, line, sizeof(line)) > 0 && link_.send(line)) {
      loudAllowed_ = false;
      baseValid_ = false;                        // unread: the next read is a new baseline
      settle(ACT_SETTLE_QUIET_MS, ACT_SETTLE_MAX_MS);
    }
    return 0;
  }
  loudAllowed_ = v > cap_;                       // #HLFY: the periodic read leaves this 3 alone
  if (v != 1) muted_ = false;
  overridden_ |= OV_VOLUME;                      // wins over a pending restore
  for (int i = 0; i < 2; i++) {
    if (!act("m", num)) return 0;
    st_.volume = v;                              // the read-back corrects it
    bool read = false;
    for (int r = 0; r < VOLUME_READS && !read; r++) read = querySettings();
    if (!read) { st_.volume = 0; baseValid_ = false; return 0; }   // unknown: poll() reads it again and caps a 3
    if (st_.volume == v) return v;
  }                                              // it reported another volume: "m" was dropped
  return st_.volume;
}

void FanController::becomeReady(uint32_t now) {
  st_.state = FanState::READY;
  misses_ = 0;
  bool first = !bootDone_;
  if (first) {                                   // what the MCU said, before the cap touches it
    bootSettingsKnown_ = baseValid_;
    bootSettings_ = base_;
  }
  loudAllowed_ = false;
  capVolume();
  if (first && bootSettingsKnown_ && bootSettings_.volume > cap_) bootCapTo_ = baseValid_ ? st_.volume : -1;
  if (bleWant_ >= 0 && !bleStuck_ && baseValid_ && base_.ble != (bleWant_ == 1)) switchBle();
  nextSettings_ = now + SETTINGS_POLL_MS;
  refreshCatalog();
  refreshPlay();
  if (first) {
    bootPlayKnown_ = playing_[0] != '\0';
    strncpy(bootClip_, playing_, sizeof(bootClip_) - 1);
    bootProgress_ = st_.progress;
    bootTotal_ = st_.total;
    bootDone_ = true;
  }
  nextPoll_ = now + POLL_MS;
}

void FanController::markNoLink() {
  st_.state = FanState::NOMCU;
  st_.play = holofan::Play::UNKNOWN;
  setErr("nouart");
}

// Not READY: is the MCU there at all? get_setting first; if it stays silent
// a PlayInfo reply also proves it is alive (it can stop answering
// get_setting while it plays, e.g. after the C2 rebooted under it).
bool FanController::recover(char* diff, size_t diffSize) {
  if (querySettings(diff, diffSize)) return true;
  tries_++;
  if (refreshPlay()) return true;
  tries_++;
  return false;
}

void FanController::begin(uint32_t now) {
  now_ = now;
  link_.wake();
  settle(QUERY_SETTLE_QUIET_MS, QUERY_SETTLE_MAX_MS);   // let it finish the wake bytes first
  for (int i = 0; i < SETTING_TRIES - 1; i++) {
    if (querySettings()) { becomeReady(now); return; }
    tries_++;
  }
  if (recover()) { becomeReady(now); return; }
  st_.state = FanState::NOMCU;
  setErr("nomcu");
  nextPoll_ = now + NOMCU_RETRY_MS;
}

void FanController::poll(uint32_t now) {
  now_ = now;
  if (!due(now, nextPoll_)) return;
  if (st_.state == FanState::NOMCU) {
    link_.wake();                                    // in case its parser holds a partial line
    settle(QUERY_SETTLE_QUIET_MS, QUERY_SETTLE_MAX_MS);
    char diff[UNPROMPTED_MAX] = {0};
    if (recover(diff, sizeof(diff))) {
      setErr("none");
      bool loud = st_.volume > cap_;
      becomeReady(now);                              // caps a 3
      if (diff[0]) {
        char text[UNPROMPTED_MAX];
        snprintf(text, sizeof(text), "after NOMCU: %.70s%s", diff, loud ? " (capped)" : "");
        report(text);
      } else if (loud) {
        report("after NOMCU: vol 3 (capped; no baseline)");
      }
    } else {
      nextPoll_ = now + NOMCU_RETRY_MS;
    }
    return;
  }
  periodicSettings(now);
  if (refreshPlay()) misses_ = 0;
  else if (++misses_ >= POLL_MISSES_NOMCU) {
    st_.state = FanState::NOMCU;
    st_.play = holofan::Play::UNKNOWN;
    setErr("nomcu");
    nextPoll_ = now + NOMCU_RETRY_MS;
    return;
  }
  nextPoll_ = now + POLL_MS;
}

void FanController::execute(const holofan::Cmd& c, uint32_t now) {
  switch (c.type) {
    case CmdType::NONE: case CmdType::STATUS: case CmdType::CATALOG_DUMP:
    case CmdType::POWER_ON: case CmdType::POWER_OFF:
      return;
    default:
      break;
  }
  now_ = now;
  if (c.type == CmdType::BLUETOOTH) wantBluetooth(c.param != 0);   // remembered even while NOMCU
  if (c.type == CmdType::VOLUME_CAP) setVolumeCap(c.param);        // the same
  if (st_.state != FanState::READY) { setErr("nomcu"); return; }
  char num[12];
  snprintf(num, sizeof(num), "%d", c.param);
  bool playback = false;
  switch (c.type) {                                // a clip change we ask for is not "unprompted"
    case CmdType::PLAY_INDEX: case CmdType::PLAY_SLOT: case CmdType::NEXT: case CmdType::PREV:
      expectClip_ = true;
      expectClipUntil_ = now + EXPECT_CLIP_MS;
      break;
    default:
      break;
  }
  switch (c.type) {                                // after a lease, our commands win over the restore
    case CmdType::PLAY_INDEX: case CmdType::PLAY_SLOT: case CmdType::NEXT: case CmdType::PREV:
    case CmdType::PAUSE:
      overridden_ |= OV_CLIP; break;
    case CmdType::LOOP: overridden_ |= OV_LOOP; break;
    case CmdType::VOLUME: case CmdType::MUTE: case CmdType::UNMUTE: overridden_ |= OV_VOLUME; break;
    case CmdType::BRIGHTNESS: overridden_ |= OV_BRIGHTNESS; break;
    default: break;
  }
  switch (c.type) {
    case CmdType::PLAY_INDEX:
      if (c.param < 1 || c.param > cat_.count) { setErr("badindex"); return; }
      // Only "t": the MCU drops a "t" that follows "a" (switcher on) at once.
      playback = act("t", cat_.entries[c.param - 1].name);
      break;
    case CmdType::PLAY_SLOT: {                     // the full listing, not the 32-entry catalog
      char line[MCU_LINE_MAX];
      if (buildLine("s", nullptr, 0, line, sizeof(line)) < 0) return;
      SlotFinder f(c.param);
      if (link_.transactChunks(line, LIST_TIMEOUT_MS, &SlotFinder::feedThunk, &f) <= 0) { setErr("timeout"); return; }
      f.finish();
      if (!f.found()) { setErr("noslot"); return; }
      playback = act("t", f.name());               // "t" only, as PLAY_INDEX
      break;
    }
    case CmdType::PAUSE: playback = act("h"); break;
    case CmdType::NEXT:  playback = act("f"); break;
    case CmdType::PREV:  playback = act("e"); break;
    case CmdType::BRIGHTNESS:
      if (c.param < 1 || c.param > 3) { setErr("badlevel"); return; }
      if (act("k", num)) { st_.brightness = c.param; base_.brightness = c.param; }
      break;
    case CmdType::VOLUME: {
      if (c.param < 1 || c.param > 3) { setErr("badlevel"); return; }
      int v = c.param > cap_ ? cap_ : c.param;
      snprintf(num, sizeof(num), "%d", v);
      if (act("m", num)) { st_.volume = v; base_.volume = v; muted_ = false; loudAllowed_ = false; }
      break;
    }
    case CmdType::MUTE:                            // volume 1 is the MCU's floor: there is no 0
      if (muted_) return;
      if (act("m", "1")) {
        preMuteVolume_ = st_.volume >= 1 && st_.volume <= cap_ ? st_.volume : cap_;
        st_.volume = 1;
        base_.volume = 1;
        muted_ = true;
        loudAllowed_ = false;
      }
      break;
    case CmdType::UNMUTE: {                        // only undoes a mute; at the floor without one, the cap
      int v = muted_ ? preMuteVolume_ : (st_.volume == 1 ? cap_ : 0);
      if (v == 0) return;
      snprintf(num, sizeof(num), "%d", v);
      if (act("m", num)) { st_.volume = v; base_.volume = v; muted_ = false; loudAllowed_ = false; }
      break;
    }
    case CmdType::LOOP:
      if (act(c.param ? "j" : "i")) {
        st_.loopAll = c.param != 0;
        base_.loopAll = c.param != 0;
        if (!c.param) clipKnown_ = false;        // loop one: the next PlayInfo is the clip baseline
      }
      break;
    case CmdType::SWITCHER:
      if (act(c.param ? "a" : "b")) st_.switcher = c.param ? 1 : 0;
      break;
    case CmdType::CATALOG_REFRESH:
      refreshCatalog();
      break;
    case CmdType::BLUETOOTH:
      if (!baseValid_) querySettings();
      if (baseValid_ && base_.ble != (bleWant_ == 1)) switchBle();
      break;
    case CmdType::VOLUME_CAP:                      // a lower cap takes effect now; a higher one raises nothing
      if (st_.volume > cap_) {
        loudAllowed_ = false;
        capVolume();
      }
      break;
    default:
      return;
  }
  if (playback) nextPoll_ = now + QUICK_POLL_MS;
}

}  // namespace fanlogic
