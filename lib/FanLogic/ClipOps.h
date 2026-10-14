// Clip list / upload / delete over the fan's LED/SD MCU (docs/HOW_IT_WORKS.md).
// Pure C++ over McuLink + ClipSpi + BodyReader, so the native env tests it.
// Upload: "w <name> <padded size>" -> CT/CF, then 1440-byte SPI frames in
// 20-frame windows, each followed by the GPIO18 wait (and every frame after
// the GPIO18 credit check, SyncCredit), then RT/RF. Both GPIO18 steps also
// cover the zero tail that fills the last window (see SyncCredit).
#ifndef FAN_CLIP_OPS_H
#define FAN_CLIP_OPS_H
#include <stddef.h>
#include <stdint.h>
#include "FanController.h"
#include "FrameRing.h"

namespace fanlogic {

constexpr int      SPI_WINDOW_FRAMES = 20;
constexpr size_t   SPI_WINDOW = SPI_FRAME * SPI_WINDOW_FRAMES;   // 28800
constexpr size_t   CLIP_MAX_BYTES = 128u * 1024u * 1024u;
constexpr uint32_t CREATE_TIMEOUT_MS = 10000;                     // stock: ~10 s for CT/CF
constexpr uint32_t VERDICT_TIMEOUT_MS = 10000;
constexpr uint32_t DELETE_READ_MS = 3000;                         // `u` is never ACKed
// The MCU drops input while it is busy (bench trace 2026-10-06): after a
// pause and after CT, wait until it has been quiet this long (at most
// SETTLE_MAX_MS) before sending more.
constexpr uint32_t PAUSE_TIMEOUT_MS = 2000;                       // for "mculog:pause"
constexpr uint32_t SETTLE_QUIET_MS = 300;
constexpr uint32_t SETTLE_MAX_MS = 3000;
// An "s" reply may continue after a gap: keep reading until quiet this long.
constexpr uint32_t LIST_QUIET_MS = 1000;
constexpr uint32_t LIST_MAX_MS = 3000;

enum class ClipErr { OK, NAME, LENGTH, EXISTS, NOT_FOUND, STILL, CF, RF, SYNC, SPI, RECV, NOVERDICT, FLASH, ABORT, BUSY };
// "ok", "name", "length", "exists", "notfound", "still", "cf", "rf", "sync",
// "spi", "recv", "noverdict", "flash", "abort", "busy".
const char* clipErrName(ClipErr e);

// `NN-<chars>.bin` or `NN_<chars>.bin` (two ASCII digits, '-' or '_' as the
// vendor app uses, at least one char) that also
// passes isClipName() and has no '/', '\\' or ':' (FAT path separators).
bool isUploadName(const char* s);
// Rounded up to a multiple of SPI_WINDOW (the MCU drops a partial window).
size_t padSize(size_t size);
// A URI path segment (after "/api/clips/") into a clip name: stops at a raw
// '?' or '#', decodes %XX. False if empty, %00, a bad escape, or it does not
// fit `size` (never truncated into a different name).
bool decodeClipPath(const char* path, char* out, size_t size);
// `name` is one of the names in an "s" reply.
bool listHas(const char* list, const char* name);
// `{"name":"<name>"}` (with a leading ',' unless first) into out, '"' and
// '\' escaped. Returns the length, or -1 if it does not fit.
int clipJsonEntry(const char* name, bool first, char* out, size_t size);

// The SPI clock of one upload, for single-variable bench experiments (0.7.2):
// PUT /api/clips/<name>?mhz=<10|20|40>&clk=<pll|xtal>. The defaults are the
// clock of every upload before 0.7.2: 40 MHz from PLL_F40M (the IDF's
// SPI_CLK_SRC_DEFAULT on the ESP32-C2). The stock firmware uses 40 MHz from
// XTAL (also 40 MHz on the fan's board).
enum class SpiClk { PLL, XTAL };
struct SpiConfig {
  uint32_t hz = 40000000;
  SpiClk   clk = SpiClk::PLL;
};
inline bool operator==(const SpiConfig& a, const SpiConfig& b) { return a.hz == b.hz && a.clk == b.clk; }
inline bool operator!=(const SpiConfig& a, const SpiConfig& b) { return !(a == b); }
// "pll" or "xtal".
const char* spiClkName(SpiClk c);
// A clip PUT's query string (the text after the '?', up to a '#' or the
// end; null or "" = none) into `out`, starting from the defaults:
// "mhz=10|20|40" and "clk=pll|xtal", each at most once, '&'-separated, in
// any order, exactly as written (no percent-decoding, lower case). False on
// anything else (another value, an unknown or repeated key, an empty pair),
// with `out` untouched: the PUT then answers 400 before the MCU hears of it.
bool parseSpiQuery(const char* query, SpiConfig& out);

// One "mhz=..." or "clk=..." pair [pair, end) into cfg, under the
// parseSpiQuery() rules (each key once: haveMhz / haveClk track that).
// False for anything else, with cfg untouched.
bool parseSpiPair(const char* pair, const char* end, SpiConfig& cfg, bool& haveMhz, bool& haveClk);

// The SPI2 data path to the MCU (firmware: McuSpi; tests: a fake).
class ClipSpi {
 public:
  virtual ~ClipSpi() = default;
  // Right before every frame: the GPIO18 credit check (SyncCredit). False =
  // sync timeout.
  virtual bool beforeFrame() = 0;
  // One frame (SPI_FRAME bytes). False on a driver error.
  virtual bool sendFrame(const uint8_t* buf, size_t len) = 0;
  // The GPIO18 wait after each SPI_WINDOW_FRAMES frames. False = sync timeout.
  virtual bool waitWindow() = 0;
};

// GPIO18, the MCU's SPI handshake line, as the upload samples it (firmware:
// McuSpi; tests: a scripted fake).
class SyncLine {
 public:
  virtual ~SyncLine() = default;
  virtual bool high() = 0;                 // one sample
  virtual void yield() = 0;                // let other ready tasks run
  virtual void delayMs(uint32_t ms) = 0;   // sleep
};

// The GPIO18 handshake (docs/HOW_IT_WORKS.md, section 4): the MCU holds
// GPIO18 LOW while it stores a window.
//   afterWindow(), after every SPI_WINDOW_FRAMES frames: one more window is
//     pending, then count LOW samples (cumulative): ok at SYNC_LOW_TARGET; at
//     SYNC_SPIN_CAP samples ok iff >= SYNC_LOW_MIN (a yield every 1024
//     samples).
//   beforeFrame(), before every frame while windows are pending: one sample,
//     HIGH retires one; if more than one is still pending, wait (yielding)
//     for HIGH and retire until one is left, then rest SYNC_REST_MS. So at
//     most one extra window is outstanding while the MCU holds GPIO18 LOW.
//   The HIGH wait is capped at SYNC_SPIN_CAP samples; beforeFrame() then
//     fails (sync).
//   uploadClip() runs both for the zero tail too (the frames that fill the
//     last window): the credit check before each tail frame and the LOW
//     count after the window. The tail goes out back to back, so the credit
//     check is its only backpressure.
constexpr int      SYNC_SPIN_CAP = 3000000;
constexpr int      SYNC_LOW_TARGET = 100;
constexpr int      SYNC_LOW_MIN = 10;
constexpr uint32_t SYNC_REST_MS = 4;   // rest after the HIGH wait

class SyncCredit {
 public:
  explicit SyncCredit(SyncLine& line) : line_(line) {}
  // A new upload: nothing pending, counters cleared.
  void reset();
  bool beforeFrame();
  bool afterWindow();
  int pending() const { return pending_; }
  int highWaits() const { return highWaits_; }   // frames that had to wait for HIGH
  int lastLow() const { return lastLow_; }       // the last LOW-count wait: LOW samples
  int lastSpins() const { return lastSpins_; }   // and samples taken

 private:
  SyncLine& line_;
  int pending_ = 0, highWaits_ = 0, lastLow_ = 0, lastSpins_ = 0;
};

// The HTTP request body (firmware: httpd_req_recv with a ~1 s timeout).
class BodyReader {
 public:
  virtual ~BodyReader() = default;
  // Up to n bytes into buf: the count read (> 0); 0 = nothing arrived yet
  // (a short timeout, call again); < 0 = closed or failed.
  virtual int read(uint8_t* buf, size_t n) = 0;
};

class Clock {
 public:
  virtual ~Clock() = default;
  virtual uint32_t nowMs() = 0;
};

// Bench 2026-10-06/07: the MCU aborts a transfer when SPI data stops for
// more than about 3-4 s and takes a window only when its 20 frames arrive
// back to back; Wi-Fi from the dome stalls 5-30 s. So the body is staged in
// a FrameRing (flash). Sessions pace whole windows (Pace::RESERVE/BURST);
// TRICKLE (at most one frame per TRICKLE_MS unless the ring is full) is the
// single PUT's pace, which reads the body itself between frames.
constexpr uint32_t TRICKLE_MS = 1000;
constexpr uint32_t BODY_STALL_MS = 60000;   // no body bytes this long = failed (TCP back-off reaches ~30-60 s)

// How uploadClip paces frames while the body is still arriving (0.8.1).
//   TRICKLE: at most one frame per TRICKLE_MS unless the ring is full (the
//            single PUT; keeps the MCU fed through a slow link).
//   BURST:   at a window boundary wait until a whole window (20 frames) is
//            buffered and at least igapMs have passed since the last frame
//            of the previous window; inside a window send back to back;
//            never send trickle frames. Once the body is over or failed
//            everything goes out at once in both modes.
//   RESERVE: (0.8.2, the session default) the MCU aborts after ~3-5 s of SPI
//            silence anywhere and takes whole windows <= ~3 s apart (bench
//            2026-10-07). While the body is arriving, at a window boundary
//            send the next window (20 frames back to back) only if >= 20
//            frames are buffered AND (the ring is full OR keepMs have
//            passed since the previous window's last frame); the first
//            window goes as soon as 20 are buffered. No trickle. The
//            buffered frames are the cover for a Wi-Fi outage.
enum class Pace { TRICKLE, BURST, RESERVE };
constexpr uint32_t KEEP_DEFAULT_MS = 3000;   // bench 2026-10-07: 3.0 s gaps x11 and one 4.0 s gap accepted, 10 s not
struct PaceConfig {
  Pace     pace = Pace::TRICKLE;
  uint32_t igapMs = 0;
  uint32_t keepMs = KEEP_DEFAULT_MS;
  // 0.9.8 (by design, a dark fan during uploads is fine): before the pause, switch to
  // BLACK_CLIP if the card has it (dark and silent), else brightness 1.
  // FanController puts the clip and brightness back after the lease.
  bool     dark = false;
};
// A short all-black, silent clip for dark uploads (clipgen --video black,
// silence). Named to sort last, so the catalog indices of 01-.. stay put.
constexpr const char* BLACK_CLIP = "99-Black.bin";

struct UploadResult {
  ClipErr err;
  bool partial;   // a failed upload may have left a file (cleanup unconfirmed)
  size_t frames = 0;   // SPI frames sent (diagnostics)
  size_t bodyBytes = 0;  // body bytes received (diagnostics)
  char   bodyEnd = '-';  // why the body ended early: 'e' read error, 's' stalled, 'd' ring ran dry,
                         // 'a' aborted, 'f' flash; '-' it did not
  // CRC32 (zlib) of the `frames` frames handed to sendFrame, zero tail
  // frames included (diagnostics: compare with zlib.crc32 of the padded file).
  uint32_t crc = 0;
  // Timing diagnostics (ms, clock): the longest window (its first frame to
  // its last) and the longest gap between windows (last frame of one to
  // the first of the next).
  uint32_t wmax = 0;
  uint32_t igmax = 0;
};

enum class BodyState { ARRIVING, COMPLETE, FAILED };

// Where uploadClip gets its 1440-byte frames: the
// single PUT's body (PutSource) or a resumable session (ChunkFeed).
class FrameSource {
 public:
  virtual ~FrameSource() = default;
  virtual size_t count() = 0;            // frames ready to pop
  virtual bool full() = 0;               // no room for one more frame: send now
  virtual bool prefilled() = 0;          // enough buffered to create the file
  virtual BodyState state() = 0;
  virtual ClipErr failErr() = 0;         // when FAILED: RECV, ABORT or FLASH
  virtual bool pop(uint8_t* frame) = 0;  // the oldest frame; false = flash error (called only when count() > 0)
  virtual void wait(uint32_t ms) = 0;    // until something may have changed, at most about ms
  virtual size_t bodyBytes() = 0;        // diagnostics: body bytes received
  virtual char bodyEnd() = 0;            // diagnostics: '-', 'e', 's', 'd', 'a', 'f'
  // uploadClip brackets every 20-frame window, from before its first pop to
  // the end of its GPIO18 wait. A source with a concurrent producer pauses
  // its flash writes in between: an erase freezes the chip for ~40 ms and
  // can swallow the MCU's ~50 us LOW ack pulse (bench 2026-10-07, 0.8.6:
  // "get packet #1" logged but low=0). Default: nothing.
  virtual void windowBegin() {}
  virtual void windowEnd() {}
};

// The single PUT's frames: the request body staged into a FrameRing one
// frame at a time (the last zero-padded). wait() makes one read attempt (it
// blocks at most one BodyReader::read). prefilled() once the ring is full or
// the whole body is in. The body fails on a read error ('e'), after
// BODY_STALL_MS without bytes ('s') or on a flash error ('f').
class PutSource : public FrameSource {
 public:
  PutSource(BodyReader& body, FrameRing& ring, Clock& clock, size_t size, uint8_t* asmBuf);
  size_t count() override;
  bool full() override;
  bool prefilled() override;
  BodyState state() override;
  ClipErr failErr() override;
  bool pop(uint8_t* frame) override;
  void wait(uint32_t ms) override;
  size_t bodyBytes() override { return got_; }
  char bodyEnd() override;

 private:
  void pump();
  BodyReader& body_;
  FrameRing&  ring_;
  Clock&      clock_;
  size_t      size_;
  uint8_t*    asm_;
  size_t      frames_, pushed_ = 0, fill_ = 0, got_ = 0;
  uint32_t    lastByteMs_;
  bool        ok_ = true, flash_ = false, stalled_ = false;
};

// Upload `size` body bytes as `name`: validate, check the list (EXISTS), wait until `src` is
// prefilled (a body failure here creates nothing), pause, "w", CT, then
// send frames from `src`, paced (TRICKLE_MS) while the body is still
// arriving and at once when it is full or the body has ended; the GPIO18
// credit check before every frame and the GPIO18 wait after every 20
// frames; after a failed body the rest of the ring, then zeros to the
// padded size; RT/RF; cleanup on failure. `pace`: see PaceConfig. `frame` is one SPI_FRAME buffer
// (DMA-capable on the fan); `scratch` holds "s" replies.
UploadResult uploadClip(McuLink& link, ClipSpi& spi, FrameSource& src, Clock& clock,
                        const char* name, size_t size, uint8_t* frame, char* scratch, size_t scratchSize,
                        const PaceConfig& pace = PaceConfig());

// The MCU's "s" reply into scratch, read until quiet for LIST_QUIET_MS (the
// reply may continue after a gap). Returns the bytes read.
int listClips(McuLink& link, char* scratch, size_t scratchSize);

// Delete one clip: 400-class NAME if !isClipName; NOT_FOUND if the "s" list
// does not have it; else "u <name>" (then read for the whole DELETE_READ_MS:
// the MCU never ACKs; this is the settle time), list again: STILL if it is still there, else OK.
// `scratch` holds the "s" reply (8 KB is enough for 100 names).
// force: skip the "listed" check and send "u" anyway (for a file the MCU's
// list never shows); then OK unless the name is still listed.
ClipErr deleteClip(McuLink& link, const char* name, char* scratch, size_t scratchSize, bool force = false);

// SD card FORMAT (the MCU's "v": erases every clip). Owner-approved
// 2026-10-06 as the only remote way to clear a card the MCU cannot clean.
// Sent only when `confirm` equals FORMAT_CONFIRM exactly: pause ("h"),
// settle, "v", wait up to FORMAT_MAX_MS for "mculog:format.", settle, then list
// into scratch. NAME (and nothing sent) on a wrong token. buildLine() still
// refuses "v"; this is the only place it is written.
constexpr const char* FORMAT_CONFIRM = "FORMAT-SD-CARD";
constexpr uint32_t FORMAT_MAX_MS = 60000;
ClipErr formatCard(McuLink& link, const char* confirm, char* scratch, size_t scratchSize);

}  // namespace fanlogic
#endif
