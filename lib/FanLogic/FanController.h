// Drives the fan's LED/SD MCU for the #HLF playback commands and keeps the
// cached status and catalog that #HLFS / #HLFD answer from (docs/SERIAL_COMMANDS.md).
// Pure C++ over an injected McuLink, so the native env tests it with a fake.
// Single task: on the fan everything here runs on the MCU task.
#ifndef FAN_CONTROLLER_H
#define FAN_CONTROLLER_H
#include <stddef.h>
#include <stdint.h>
#include "HolofanStatus.h"
#include "McuProto.h"
#include "command.h"

namespace fanlogic {

// The UART1 link, implemented by firmware/holofan/components/mcu_link and by
// a fake in the tests. Lines passed in are already framed by buildLine().
class McuLink {
 public:
  virtual ~McuLink() = default;
  // The stock boot wake: the two bytes "\n\n".
  virtual void wake() = 0;
  // Write one framed line. False on a write error.
  virtual bool send(const char* line) = 0;
  // Drop stale input, write `line`, then collect input into `reply`
  // (NUL-terminated) until `needle` has arrived or `timeoutMs` passes. With
  // needle == nullptr, collect until the input has been quiet for a moment
  // after the first byte (or timeoutMs). Returns the bytes collected.
  virtual int transact(const char* line, const char* needle, uint32_t timeoutMs, char* reply, size_t size) = 0;
  // With `line`: drop stale input and write it first; with nullptr only
  // read. Then collect input until one of tokens[0..n) appears (substring,
  // as stock matches CT/CF/RT/RF) or timeoutMs passes. Returns the index of
  // the token found, or -1. With n == 0 it reads for the whole timeout.
  virtual int expect(const char* line, const char* const* tokens, int n, uint32_t timeoutMs) = 0;
  // Read without writing until the input has been quiet for quietMs (or
  // maxMs in all). With buf, append what arrives after strlen(buf) (kept
  // NUL-terminated, truncated at size); with nullptr, drop it. Returns the
  // bytes read.
  virtual int collect(char* buf, size_t size, uint32_t quietMs, uint32_t maxMs) = 0;
  // transact(line, nullptr, timeoutMs, ...) that hands the reply to `feed`
  // in pieces as it arrives, so a reply of any length needs no buffer (the
  // full "s" listing for #HLFT). The default collects into 1 KB first.
  virtual int transactChunks(const char* line, uint32_t timeoutMs, void (*feed)(void* ctx, const char* b, size_t n),
                             void* ctx) {
    char buf[1024];
    int n = transact(line, nullptr, timeoutMs, buf, sizeof(buf));
    if (n > 0) feed(ctx, buf, (size_t)n);
    return n;
  }
};

constexpr int      SETTING_TRIES = 21;          // stock: give up after 21 get_setting attempts
constexpr uint32_t SETTING_TIMEOUT_MS = 400;
constexpr uint32_t PLAYINFO_TIMEOUT_MS = 300;
constexpr uint32_t LIST_TIMEOUT_MS = 1500;
constexpr uint32_t POLL_MS = 3000;              // idle poll cadence
constexpr uint32_t QUICK_POLL_MS = 300;         // refresh soon after a playback command
constexpr uint32_t NOMCU_RETRY_MS = 10000;      // NOMCU: one get_setting try this often
constexpr int      POLL_MISSES_NOMCU = 5;       // READY -> NOMCU after this many silent polls
// READY: get_setting this often (0.9.8), so changes nobody commanded are
// reported (takeUnprompted) within this time.
constexpr uint32_t SETTINGS_POLL_MS = 30000;
constexpr uint32_t SETTINGS_RETRY_MS = SETTINGS_POLL_MS;
constexpr uint32_t EXPECT_CLIP_MS = 5000;       // a clip change we asked for may show up this late
constexpr int      SOON_TRIES = 3;              // checkSettingsSoon: polls that may try before the 30 s period
constexpr uint32_t RESTORE_MAX_MS = 60000;      // a restore after a clip lease is dropped if not done by then
constexpr int      CLIP_RESTORE_TRIES = 3;      // polls the clip part may retry (no PlayInfo, no listing)
constexpr int      VOLUME_READS = 3;            // setVolumeChecked: get_setting tries after each "m" (MCU busy)
// After every action line the MCU is left to settle (0.9.7): it drops or
// misreads a line that arrives while it is busy (bench 0.9.6: get_setting 4 ms
// after "m 3" lost its 'g' and ran as 'e', previous clip). As ClipOps after
// "h" and CT: read until it has been quiet this long, at most ACT_SETTLE_MAX_MS.
constexpr uint32_t ACT_SETTLE_QUIET_MS = 300;
constexpr uint32_t ACT_SETTLE_MAX_MS = 3000;
// The same after the wake bytes and after a query that got no answer (0.9.8).
constexpr uint32_t QUERY_SETTLE_QUIET_MS = 300;
constexpr uint32_t QUERY_SETTLE_MAX_MS = 1000;
constexpr size_t   UNPROMPTED_MAX = 96;

class FanController {
 public:
  explicit FanController(McuLink& link) : link_(link) {}

  // Wake the MCU and ask for its settings, up to SETTING_TRIES times. If it
  // answers: READY, then load the catalog and the play state. Else NOMCU.
  void begin(uint32_t now);
  // Run one playback action (PLAY_INDEX .. CATALOG_REFRESH). STATUS,
  // CATALOG_DUMP and POWER_* are not for this class and are ignored. In
  // NOMCU every action is dropped with err=nomcu.
  void execute(const holofan::Cmd& c, uint32_t now);
  // Call often. READY: get_playinfo every POLL_MS. NOMCU: retry get_setting
  // every NOMCU_RETRY_MS and come back READY if it answers.
  void poll(uint32_t now);

  // Set volume v (1-3) and read it back (up to VOLUME_READS get_setting:
  // the MCU is often busy right after "m"), sending "m" once more only if
  // it reports another volume (#HLFY, maintenance). Returns the volume the
  // MCU reports afterwards, or 0: no read-back (the volume is then
  // unknown, so poll() reads it again), NOMCU (v is still sent, unread),
  // or v out of range.
  int setVolumeChecked(int v);

  // The UART to the MCU never came up: NOMCU with err=nouart (call instead of begin()).
  void markNoLink();

  // 0.9.9 (Bluetooth is not needed on the mesh): keep the MCU's "ble" setting (its
  // Bluetooth audio chip; on by default, it pairs with nearby speakers) at
  // `on`: switched with c/d and read back at connect, at every settings
  // read and after clip leases; a change behind our back is reported
  // ("ble off->on (switched off)"); an MCU that will not switch is reported
  // once and left alone until the next wantBluetooth(). Not called: the
  // setting is left as it is. CmdType::BLUETOOTH sets it too.
  void wantBluetooth(bool on) { bleWant_ = on ? 1 : 0; bleStuck_ = false; }

  // 0.9.8: a raw line went to the MCU behind this class (#HLFX, console):
  // the next settings read sets a new baseline instead of reporting.
  // Also reads the settings at the next poll and allows a clip change for
  // EXPECT_CLIP_MS. Call it only for lines that may change something
  // (rawLineMayChange): queries keep the baseline.
  void forgetBaseline();
  // A clip lease ended (upload, delete, format): the MCU may have
  // re-initialised, so read the settings at the next poll (up to SOON_TRIES
  // polls if it stays silent).
  void checkSettingsSoon() { soonTries_ = SOON_TRIES; }
  // 0.9.8: a clip lease starts / ends (the lease holder calls these while it
  // holds the link, so this class is not running meanwhile). `changed`: the
  // lease touched the MCU (upload, delete, format; not a list, EXISTS or an
  // SPI failure). At the first settings read after such a lease, what the
  // MCU changed since beginLease() is put back: volume, loop
  // mode (if it was known), brightness, then the clip: in loop all a playing
  // fan is left alone unless the MCU changed its loop mode; otherwise the
  // clip that was playing is started again, or shown paused, if a fresh
  // listing still has it (retried for CLIP_RESTORE_TRIES polls). A pending
  // restore survives a following lease; it is dropped after RESTORE_MAX_MS.
  // A command after the lease (and #HLFY's volume, a raw #HLFX line) wins
  // over the restore of what it sets. Reported by takeLeaseReport(), not as
  // UNPROMPTED (leaseChanges() counts them).
  void beginLease();
  void endLease(bool changed);
  bool takeLeaseReport(char* out, size_t size);
  uint32_t leaseChanges() const { return leaseChanges_; }
  // Changes nobody here commanded, seen by the periodic settings read or (in
  // loop one) by a clip change: "vol 2->3", "loop one->all",
  // "clip A->B". One pending text (later events append); false if none.
  bool takeUnprompted(char* out, size_t size);
  uint32_t unprompted() const { return unpromptedCount_; }
  // What the MCU reported at the first connect: "[fan] MCU at connect: ble
  // on, loop all, br 2, vol 3, angle 164; playing <name> <progress>/<total>".
  // bootDone() once known.
  bool bootDone() const { return bootDone_; }
  // The settings were first read after the connect (READY through PlayInfo
  // alone): the boot record has them now. True once per such read.
  bool takeBootLate() { bool b = bootLate_; bootLate_ = false; return b; }
  int formatBoot(char* out, size_t size) const;
  int bootVolume() const { return bootSettingsKnown_ ? bootSettings_.volume : 0; }

  const holofan::Status& status() const { return st_; }
  const holofan::Catalog& catalog() const { return cat_; }
  // The MCU has answered get_setting at least once since boot (OTA validity gate).
  bool mcuAnswered() const { return answered_; }
  // get_setting + get_playinfo requests that went unanswered or were tried
  // while not READY (diagnostics).
  uint32_t queryTries() const { return tries_; }

 private:
  // `diff` (optional): the fields that changed against the baseline, when one exists.
  bool querySettings(char* diff = nullptr, size_t diffSize = 0);
  bool recover(char* diff = nullptr, size_t diffSize = 0);
  bool refreshPlay();                  // true if the MCU answered (even if unparseable)
  void settle(uint32_t quietMs, uint32_t maxMs) { link_.collect(nullptr, 0, quietMs, maxMs); }
  void report(const char* text);
  void periodicSettings(uint32_t now);
  void restoreSettingsAfterLease();
  bool switchBle();                    // c/d to bleWant_, read back; false if it did not take
  void restoreClip();
  int clipOnCard(const char* name);    // 1 yes, 0 no, -1 the listing failed
  void reportLease(const char* text);
  void refreshCatalog();
  bool act(const char* cmd, const char* arg = nullptr);
  void setErr(const char* e);
  void updateIndex();
  void becomeReady(uint32_t now);
  static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

  McuLink&         link_;
  holofan::Status  st_;
  holofan::Catalog cat_;
  char     playing_[holofan::CLIP_NAME_MAX + 1] = {0};   // full PlayInfo name
  bool     answered_ = false;
  bool     muted_ = false;
  int      preMuteVolume_ = VOLUME_MAX;
  uint32_t now_ = 0;                   // the latest time given to begin/poll/execute
  Settings base_;                      // the MCU's settings as last read or commanded
  bool     baseValid_ = false;
  bool     clipKnown_ = false;         // playing_ is a baseline for loop-one clip changes
  bool     expectClip_ = false;        // we asked for a clip change (t/e/f) ...
  uint32_t expectClipUntil_ = 0;       // ... that may show up until then
  char     unprompted_[UNPROMPTED_MAX] = {0};
  uint32_t unpromptedCount_ = 0;
  bool     bootDone_ = false;
  bool     bootLate_ = false;           // bootSettings_ filled by a later read (takeBootLate)
  bool     bootSettingsLate_ = false;
  int      soonTries_ = 0;
  struct PreLease {
    bool valid = false;
    char clip[holofan::CLIP_NAME_MAX + 1] = {0};
    bool playing = false;
    bool loopAll = true;
    bool loopKnown = false;
    int  volume = 0, brightness = 0;
  } pre_;
  bool     settingsRead_ = false;      // a get_setting has been parsed since boot
  bool     lastPlayParsed_ = false;    // the last refreshPlay parsed a PlayInfo
  int      bleWant_ = -1;              // -1 leave it, 0 off, 1 on
  bool     bleStuck_ = false;          // the MCU did not switch: stop trying
  bool     restoreArmed_ = false;      // the settings part waits for the next read
  bool     clipPending_ = false;       // the clip part waits (after the settings part)
  int      clipTries_ = 0;
  bool     freshSnapshot_ = false;     // beginLease took pre_ (not kept from a pending restore)
  bool     deadlineSet_ = false;
  uint32_t restoreDeadline_ = 0;
  bool     loopChangedByMcu_ = false;
  uint8_t  overridden_ = 0;            // OV_* set by commands after endLease()
  char     leaseReport_[UNPROMPTED_MAX] = {0};
  uint32_t leaseChanges_ = 0;
  bool     bootSettingsKnown_ = false;
  Settings bootSettings_;
  bool     bootPlayKnown_ = false;
  char     bootClip_[holofan::CLIP_NAME_MAX + 1] = {0};
  int      bootProgress_ = 0, bootTotal_ = 0;
  int      misses_ = 0;
  uint32_t tries_ = 0;
  uint32_t nextPoll_ = 0;
  uint32_t nextSettings_ = 0;   // READY with the volume unknown: next get_setting
};

}  // namespace fanlogic
#endif
