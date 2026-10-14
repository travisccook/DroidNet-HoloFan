// The resumable clip upload session's pure parts (docs/MAINTENANCE_API.md):
// /api/upload query parsing, the open decision, JSON strings, and
// runSession(), the upload task's body. The ESP-IDF side is
// firmware/holofan/components/maint_idf/UploadIdf.cpp.
#ifndef FAN_UPLOAD_SESSION_H
#define FAN_UPLOAD_SESSION_H
#include <stddef.h>
#include <stdint.h>
#include "ClipOps.h"
#include "HolofanStatus.h"

namespace fanlogic {

constexpr uint32_t LEASE_TRY_MS = 1000;      // one lease attempt (D11)
constexpr uint32_t LEASE_RETRY_MS = 30000;   // then BUSY
constexpr uint32_t SESSION_WAIT_MS = 100;    // runSession's poll while waiting
constexpr uint32_t CHUNK_STALL_MS = 3000;    // a chunk handler gives up its socket (D10); bench 2026-10-07: 10 s blocked
                                             // httpd (single task) from serving the client's reconnect

enum class UploadReq { OPEN, CHUNK, ABORT };
enum class QueryErr { OK, NAME, LENGTH, QUERY };
const char* queryErrName(QueryErr e);        // "ok", "name", "length", "query"

constexpr uint32_t SESSION_DRY_MS = 30000;   // a burst session's default dry limit (ChunkFeed::DRY_MS is the lower-level default)
// 0.8.2: a reserve session fails fast: the MCU aborts after ~3-5 s of SPI
// silence, so waiting longer than this for the body only wastes the file.
constexpr uint32_t SESSION_RESERVE_DRY_MS = 4000;
constexpr size_t   SESSION_PRE_MAX = 617;    // the fan's ring (637 frames) minus one window
inline PaceConfig reservePace() {
  PaceConfig p;
  p.pace = Pace::RESERVE;
  return p;
}

struct UploadQuery {
  char      name[holofan::CLIP_NAME_MAX + 1] = {0};
  size_t    size = 0;
  size_t    off = 0;
  uint32_t  id = 0;
  bool      hasId = false;
  SpiConfig spi;
  // Timing knobs (OPEN): pace reserve|burst (default reserve), igap
  // 0..60000 ms, keep 1000..10000 ms (reserve), pre 20..617 frames (0 = the
  // ring minus one window), dry 1000..600000 ms (default 4000 for reserve,
  // else 30000).
  PaceConfig pace = reservePace();
  size_t     pre = 0;
  uint32_t   dryMs = SESSION_RESERVE_DRY_MS;
};

// The query of an /api/upload request (the text after '?', up to a '#';
// null or "" = none): '&'-separated key=value pairs, each key at most once.
//   OPEN   name (percent-decoded; isUploadName) and size (decimal,
//          1..CLIP_MAX_BYTES) required; mhz and clk optional (as on the
//          single PUT); pace=reserve|burst (trickle: QUERY since 0.9.1,
//          it waits inside a window), igap=<0..60000>,
//          keep=<1000..10000>, pre=<20..617>, dry=<1000..600000> and
//          dark=0|1 (0.9.8: PaceConfig::dark) optional (each once); igap must be below dry, and for reserve
//          keep too (dry defaults to 4000 for reserve, else 30000), else
//          QUERY.
//   CHUNK  id (decimal, 1..2147483647) and off (decimal, 0..CLIP_MAX_BYTES)
//          required.
//   ABORT  id optional.
// NAME: missing or bad name. LENGTH: missing or bad size or off. QUERY:
// anything else (an unknown, repeated or empty pair, a key this kind does
// not take, a bad id or clock). `out` is written only on OK.
QueryErr parseUploadQuery(UploadReq kind, const char* query, UploadQuery& out);

// POST /api/upload while `active` (a session in prefill or sending): the
// same name and size is the same session (a retried POST), anything else
// BUSY. Not active: NEW.
enum class OpenDecision { NEW, SAME, BUSY };
OpenDecision openDecision(bool active, const char* activeName, size_t activeSize, const char* name, size_t size);

// `s` as the inside of a JSON string ('"' and '\' escaped, bytes below
// 0x20 as '.') into out. False, with out = "", if it does not fit.
bool jsonString(const char* s, char* out, size_t size);

// The MCU link for one session (firmware: the ClipHooks of MaintIdf).
class McuLease {
 public:
  virtual ~McuLease() = default;
  virtual McuLink* lease(uint32_t timeoutMs) = 0;     // null: busy or no link (waits up to timeoutMs)
  virtual ClipSpi* spi(const SpiConfig& cfg) = 0;     // lease holder; null: driver failed
  virtual void diag(char* out, size_t size) = 0;      // lease holder: SPI and MCU fields for "last"
  virtual void release(bool refresh) = 0;
};

// The upload task's body (D5, D6, D11): wait until `src` is prefilled or
// its body has ended (a failed body returns its error and never touches
// the MCU); lease the link (LEASE_TRY_MS a try, BUSY after LEASE_RETRY_MS);
// the SPI with `cfg` (SPI on failure); uploadClip; the lease's diag into
// `diag` (empty if no lease); release (with a catalog refresh unless
// EXISTS). `frame` is one SPI_FRAME buffer (DMA-capable on the fan).
UploadResult runSession(McuLease& lease, FrameSource& src, Clock& clock, const char* name, size_t size,
                        const SpiConfig& cfg, const PaceConfig& pace, uint8_t* frame, char* scratch,
                        size_t scratchSize, char* diag, size_t diagSize);

}  // namespace fanlogic
#endif
