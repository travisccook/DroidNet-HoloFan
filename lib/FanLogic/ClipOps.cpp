#include "ClipOps.h"
#include <stdio.h>
#include <string.h>
#include "Crc32.h"

namespace fanlogic {

const char* clipErrName(ClipErr e) {
  switch (e) {
    case ClipErr::OK: return "ok";
    case ClipErr::NAME: return "name";
    case ClipErr::LENGTH: return "length";
    case ClipErr::EXISTS: return "exists";
    case ClipErr::NOT_FOUND: return "notfound";
    case ClipErr::STILL: return "still";
    case ClipErr::CF: return "cf";
    case ClipErr::RF: return "rf";
    case ClipErr::SYNC: return "sync";
    case ClipErr::SPI: return "spi";
    case ClipErr::RECV: return "recv";
    case ClipErr::NOVERDICT: return "noverdict";
    case ClipErr::FLASH: return "flash";
    case ClipErr::ABORT: return "abort";
    case ClipErr::BUSY: return "busy";
  }
  return "?";
}

bool isUploadName(const char* s) {
  if (!isClipName(s) || strpbrk(s, "/\\:")) return false;   // FAT path separators
  return s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' && (s[2] == '-' || s[2] == '_') && strlen(s) > 3 + 4;
}

size_t padSize(size_t size) { return (size + SPI_WINDOW - 1) / SPI_WINDOW * SPI_WINDOW; }

const char* spiClkName(SpiClk c) { return c == SpiClk::XTAL ? "xtal" : "pll"; }

// The pair [p, end) equals "<key>=<value>" exactly.
static bool isPair(const char* p, const char* end, const char* key, const char* value) {
  size_t k = strlen(key), v = strlen(value);
  return (size_t)(end - p) == k + 1 + v && memcmp(p, key, k) == 0 && p[k] == '=' && memcmp(p + k + 1, value, v) == 0;
}

bool parseSpiPair(const char* p, const char* end, SpiConfig& cfg, bool& haveMhz, bool& haveClk) {
  if (!haveMhz && (isPair(p, end, "mhz", "10") || isPair(p, end, "mhz", "20") || isPair(p, end, "mhz", "40"))) {
    cfg.hz = (uint32_t)(p[4] - '0') * 10000000u;   // "mhz=X0": X * 10 MHz
    haveMhz = true;
    return true;
  }
  if (!haveClk && (isPair(p, end, "clk", "pll") || isPair(p, end, "clk", "xtal"))) {
    cfg.clk = p[4] == 'x' ? SpiClk::XTAL : SpiClk::PLL;
    haveClk = true;
    return true;
  }
  return false;                                       // unknown, repeated, empty or another value
}

bool parseSpiQuery(const char* query, SpiConfig& out) {
  SpiConfig cfg;
  if (!query) query = "";
  const char* stop = query + strcspn(query, "#");
  if (query == stop) { out = cfg; return true; }   // no parameters
  bool haveMhz = false, haveClk = false;
  for (const char* p = query; p <= stop;) {
    const char* end = p + strcspn(p, "&#");
    if (end > stop) end = stop;
    if (!parseSpiPair(p, end, cfg, haveMhz, haveClk)) return false;
    p = end + 1;
  }
  out = cfg;
  return true;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool decodeClipPath(const char* path, char* out, size_t size) {
  if (!path || size == 0) return false;
  size_t o = 0;
  for (const char* p = path; *p && *p != '?' && *p != '#'; p++) {
    char c = *p;
    if (c == '%') {
      int hi = hexVal(p[1]), lo = hi < 0 ? -1 : hexVal(p[2]);
      if (lo < 0 || (hi | lo) == 0) return false;   // bad escape or %00
      c = (char)(hi * 16 + lo);
      p += 2;
    }
    if (o + 1 >= size) return false;
    out[o++] = c;
  }
  out[o] = '\0';
  return o > 0;
}

bool listHas(const char* list, const char* name) {
  const char* p = list;
  char n[holofan::CLIP_NAME_MAX + 1];
  while (nextClipName(p, n, sizeof(n)))
    if (strcmp(n, name) == 0) return true;
  return false;
}

int clipJsonEntry(const char* name, bool first, char* out, size_t size) {
  size_t o = 0;
  auto put = [&](char c) { if (o + 1 < size) out[o] = c; o++; };
  if (!first) put(',');
  for (const char* t = "{\"name\":\""; *t; t++) put(*t);
  for (const char* p = name; *p; p++) {
    if (*p == '"' || *p == '\\') put('\\');
    put(*p);
  }
  put('"');
  put('}');
  if (o + 1 > size) return -1;
  out[o] = '\0';
  return (int)o;
}

static bool deleteLine(const char* name, char* line, size_t size) {
  const char* args[1] = {name};
  return buildLine("u", args, 1, line, size) > 0;
}

int listClips(McuLink& link, char* scratch, size_t scratchSize) {
  int n = link.transact("s\n", nullptr, LIST_TIMEOUT_MS, scratch, scratchSize);
  if (n < 0) n = 0;
  return n + link.collect(scratch, scratchSize, LIST_QUIET_MS, LIST_MAX_MS);
}

ClipErr deleteClip(McuLink& link, const char* name, char* scratch, size_t scratchSize, bool force) {
  char line[MCU_LINE_MAX];
  if (!isClipName(name) || !deleteLine(name, line, sizeof(line))) return ClipErr::NAME;
  listClips(link, scratch, scratchSize);
  if (!force && !listHas(scratch, name)) return ClipErr::NOT_FOUND;
  link.expect(line, nullptr, 0, DELETE_READ_MS);   // settle: the MCU never ACKs `u`
  listClips(link, scratch, scratchSize);
  return listHas(scratch, name) ? ClipErr::STILL : ClipErr::OK;
}

// After a failed upload: "u <name>", then list. True if the file may still
// be there (still listed, or the MCU gave no list at all).
static bool cleanup(McuLink& link, const char* name, char* scratch, size_t scratchSize) {
  char line[MCU_LINE_MAX];
  if (!deleteLine(name, line, sizeof(line))) return true;
  link.expect(line, nullptr, 0, DELETE_READ_MS);   // settle: the MCU never ACKs `u`
  int n = listClips(link, scratch, scratchSize);
  return n <= 0 || listHas(scratch, name);
}

PutSource::PutSource(BodyReader& body, FrameRing& ring, Clock& clock, size_t size, uint8_t* asmBuf)
    : body_(body), ring_(ring), clock_(clock), size_(size), asm_(asmBuf),
      frames_((size + SPI_FRAME - 1) / SPI_FRAME), lastByteMs_(clock.nowMs()) {}

size_t PutSource::count() { return ring_.count(); }
bool PutSource::full() { return ring_.full(); }
bool PutSource::prefilled() { return ring_.full() || pushed_ >= frames_; }
BodyState PutSource::state() {
  if (!ok_) return BodyState::FAILED;
  return pushed_ >= frames_ ? BodyState::COMPLETE : BodyState::ARRIVING;
}
ClipErr PutSource::failErr() { return flash_ ? ClipErr::FLASH : ClipErr::RECV; }
bool PutSource::pop(uint8_t* frame) { return ring_.pop(frame); }
void PutSource::wait(uint32_t) { pump(); }
char PutSource::bodyEnd() { return flash_ ? 'f' : stalled_ ? 's' : !ok_ ? 'e' : '-'; }

void PutSource::pump() {
  if (!ok_ || pushed_ >= frames_ || ring_.full()) return;
  size_t want = SPI_FRAME - fill_;
  if (want > size_ - got_) want = size_ - got_;
  int n = body_.read(asm_ + fill_, want);
  if (n < 0) { ok_ = false; return; }
  if (n == 0) {
    if ((uint32_t)(clock_.nowMs() - lastByteMs_) >= BODY_STALL_MS) { ok_ = false; stalled_ = true; }
    return;
  }
  lastByteMs_ = clock_.nowMs();
  fill_ += (size_t)n;
  got_ += (size_t)n;
  if (fill_ == SPI_FRAME || got_ == size_) {
    memset(asm_ + fill_, 0, SPI_FRAME - fill_);
    if (!ring_.push(asm_)) { ok_ = false; flash_ = true; return; }
    pushed_++;
    fill_ = 0;
  }
}

namespace {

UploadResult withBody(UploadResult r, FrameSource& src) {
  r.bodyBytes = src.bodyBytes();
  r.bodyEnd = src.bodyEnd();
  return r;
}

}  // namespace

UploadResult uploadClip(McuLink& link, ClipSpi& spi, FrameSource& src, Clock& clock,
                        const char* name, size_t size, uint8_t* frame, char* scratch, size_t scratchSize,
                        const PaceConfig& pace) {
  if (!isUploadName(name)) return {ClipErr::NAME, false};
  if (size == 0 || size > CLIP_MAX_BYTES) return {ClipErr::LENGTH, false};
  const size_t padded = padSize(size);
  char sz[12];
  snprintf(sz, sizeof(sz), "%lu", (unsigned long)padded);
  const char* args[2] = {name, sz};
  char line[MCU_LINE_MAX];
  if (buildLine("w", args, 2, line, sizeof(line)) < 0) return {ClipErr::NAME, false};

  listClips(link, scratch, scratchSize);
  if (listHas(scratch, name)) return {ClipErr::EXISTS, false};
  const bool haveBlack = pace.dark && listHas(scratch, BLACK_CLIP);

  // Prefill: nothing exists on the card yet, so a body failure here is clean.
  while (src.state() == BodyState::ARRIVING && !src.prefilled()) src.wait(TRICKLE_MS);
  if (src.state() == BodyState::FAILED) return withBody({src.failErr(), false}, src);

  if (pace.dark) {                                         // dark and silent from here (0.9.8)
    char dl[MCU_LINE_MAX];
    const char* blackArgs[1] = {BLACK_CLIP};
    const char* dimArgs[1] = {"1"};
    int len = haveBlack ? buildLine("t", blackArgs, 1, dl, sizeof(dl)) : buildLine("k", dimArgs, 1, dl, sizeof(dl));
    if (len > 0 && link.send(dl)) link.collect(nullptr, 0, SETTLE_QUIET_MS, SETTLE_MAX_MS);
  }

  // Bench 2026-10-06: pause first ("h", as #HLFP; not resumed afterwards),
  // and let the MCU go quiet before each next step: it drops input while busy.
  static const char* const PAUSED[1] = {"pause"};
  link.expect("h\n", PAUSED, 1, PAUSE_TIMEOUT_MS);
  link.collect(nullptr, 0, SETTLE_QUIET_MS, SETTLE_MAX_MS);

  static const char* const CREATE[2] = {"CT", "CF"};
  int created = link.expect(line, CREATE, 2, CREATE_TIMEOUT_MS);
  if (created == 1) return withBody({ClipErr::CF, false}, src);
  if (created < 0) return withBody({ClipErr::CF, cleanup(link, name, scratch, scratchSize)}, src);   // it may exist
  link.collect(nullptr, 0, SETTLE_QUIET_MS, SETTLE_MAX_MS);   // let it finish printing first

  ClipErr err = ClipErr::OK;
  const size_t frames = padded / SPI_FRAME;
  size_t sent = 0;
  uint32_t crc = 0;                                        // of every frame sent (diagnostics)
  uint32_t lastSend = clock.nowMs();
  uint32_t winStart = 0, wmax = 0, igmax = 0;
  bool inWindow = false;                                  // between windowBegin() and windowEnd()
  const bool burst = pace.pace == Pace::BURST;
  const bool reserve = pace.pace == Pace::RESERVE;
  while (sent < frames && err == ClipErr::OK) {
    BodyState st = src.state();
    if (st == BodyState::FAILED && src.failErr() == ClipErr::FLASH) { err = ClipErr::FLASH; break; }
    bool bodyOver = st != BodyState::ARRIVING;             // no more body frames will come
    bool ready = src.count() > 0;
    bool have = ready || bodyOver;                         // past the body: zero frames
    uint32_t since = clock.nowMs() - lastSend;
    bool due;
    uint32_t waitMs;
    if (reserve && !bodyOver) {
      // Whole windows only; the next one when the ring is full (nothing to
      // wait for) or keepMs after the previous one (just in time for the MCU).
      bool boundary = sent % SPI_WINDOW_FRAMES == 0;
      bool whole = src.count() >= (size_t)SPI_WINDOW_FRAMES;
      bool timeOk = sent == 0 || src.full() || since >= pace.keepMs;
      due = boundary ? whole && timeOk : true;
      waitMs = boundary && whole && !timeOk ? pace.keepMs - since : TRICKLE_MS;
    } else if (burst && !bodyOver) {
      // A whole window buffered and igapMs since the previous window's last
      // frame; inside a window the frames are there already.
      bool boundary = sent % SPI_WINDOW_FRAMES == 0;
      bool gapOk = sent == 0 || since >= pace.igapMs;
      due = boundary ? src.count() >= (size_t)SPI_WINDOW_FRAMES && gapOk : true;
      waitMs = boundary && src.count() >= (size_t)SPI_WINDOW_FRAMES && !gapOk ? pace.igapMs - since : TRICKLE_MS;
    } else {
      due = bodyOver || src.full() || since >= TRICKLE_MS;
      waitMs = since < TRICKLE_MS ? TRICKLE_MS - since : TRICKLE_MS;
    }
    if (!have || !due) {
      src.wait(waitMs);                                    // refill (PutSource: one read)
      continue;
    }
    if (sent % SPI_WINDOW_FRAMES == 0 && !inWindow) {
      src.windowBegin();
      inWindow = true;
    }
    if (ready) {
      if (!src.pop(frame)) { err = ClipErr::FLASH; break; }
    } else {
      memset(frame, 0, SPI_FRAME);
    }
    if (!spi.beforeFrame()) { err = ClipErr::SYNC; break; }   // GPIO18 credit, tail included (SyncCredit)
    if (!spi.sendFrame(frame, SPI_FRAME)) { err = ClipErr::SPI; break; }
    crc = crc32Update(crc, frame, SPI_FRAME);
    const uint32_t prevSend = lastSend;
    const bool first = sent % SPI_WINDOW_FRAMES == 0;
    sent++;
    lastSend = clock.nowMs();
    if (first) {
      winStart = lastSend;
      if (sent > 1 && (uint32_t)(lastSend - prevSend) > igmax) igmax = lastSend - prevSend;
    }
    link.collect(nullptr, 0, 1, 1);     // pick up MCU output after every frame (its abort timing)
    if (sent % SPI_WINDOW_FRAMES == 0) {
      if ((uint32_t)(lastSend - winStart) > wmax) wmax = lastSend - winStart;
      if (!spi.waitWindow()) err = ClipErr::SYNC;
      link.collect(nullptr, 0, 1, 1);   // and after the window's GPIO18 wait
      src.windowEnd();
      inWindow = false;
    }
  }
  if (inWindow) src.windowEnd();
  auto result = [&](ClipErr e, bool partial) {
    UploadResult r = withBody({e, partial, sent}, src);
    r.crc = crc;
    r.wmax = wmax;
    r.igmax = igmax;
    return r;
  };
  if (err == ClipErr::OK) {
    static const char* const VERDICT[2] = {"RT\n", "RF\n"};
    int v = link.expect(nullptr, VERDICT, 2, VERDICT_TIMEOUT_MS);
    if (src.state() == BodyState::FAILED) err = src.failErr();
    else if (v == 1) err = ClipErr::RF;
    else if (v < 0) err = ClipErr::NOVERDICT;
    else return result(ClipErr::OK, false);
  }
  return result(err, cleanup(link, name, scratch, scratchSize));
}

void SyncCredit::reset() { pending_ = highWaits_ = lastLow_ = lastSpins_ = 0; }

bool SyncCredit::beforeFrame() {
  if (pending_ <= 0) return true;
  if (line_.high()) pending_--;
  if (pending_ <= 1) return true;
  highWaits_++;
  int spins = 0;
  while (pending_ > 1) {
    while (!line_.high()) {
      if (++spins >= SYNC_SPIN_CAP) return false;
      line_.yield();
    }
    line_.yield();
    pending_--;
  }
  line_.delayMs(SYNC_REST_MS);
  return true;
}

bool SyncCredit::afterWindow() {
  pending_++;
  int low = 0, it = 0;
  bool ok = false;
  for (; it < SYNC_SPIN_CAP; it++) {
    if (!line_.high() && ++low >= SYNC_LOW_TARGET) { ok = true; break; }
    if ((it & 0x3ff) == 0x3ff) line_.yield();
  }
  lastLow_ = low;
  lastSpins_ = it;
  return ok || low >= SYNC_LOW_MIN;
}

ClipErr formatCard(McuLink& link, const char* confirm, char* scratch, size_t scratchSize) {
  if (!confirm || strcmp(confirm, FORMAT_CONFIRM) != 0) return ClipErr::NAME;
  static const char* const PAUSED[1] = {"pause"};
  link.expect("h\n", PAUSED, 1, PAUSE_TIMEOUT_MS);
  link.collect(nullptr, 0, SETTLE_QUIET_MS, SETTLE_MAX_MS);
  // The one deliberate "v". The MCU logs "mculog:format." only when done
  // (well after 3 s of quiet on the bench), so wait for that, then settle.
  static const char* const FORMATTED[1] = {"format"};
  link.expect("v\n", FORMATTED, 1, FORMAT_MAX_MS);
  link.collect(nullptr, 0, SETTLE_QUIET_MS, SETTLE_MAX_MS);
  if (scratchSize) scratch[0] = '\0';
  listClips(link, scratch, scratchSize);
  return ClipErr::OK;
}

}  // namespace fanlogic
