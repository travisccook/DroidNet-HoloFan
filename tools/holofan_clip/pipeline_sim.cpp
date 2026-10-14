// Offline: push a real clip through uploadClip + FrameRing (fan-sized ring, NOR
// flash semantics), random body chunking and stalls; compare SPI bytes to the file.
// 0.7.2: prints the CRC32 the fan reports in "last" (crc=), and puts a
// two-buffer MCU model behind GPIO18: it stores each window in <store ms>
// (default 30, a guess), one after the other, and holds GPIO18 LOW while a
// window is unstored; a window arriving while two are unstored is lost
// (overrun). Run once with the stock credit check (0.7.2, hiw= counts the
// frames that waited for HIGH) and once without it (as 0.7.1).
// 0.8.0: adds session mode with four scenarios (outages, kill-resume, dry run):
// prints PUT lines for the body mode runs, and session mode lines with frame
// counts, error, body end, worst gap, dup bytes, outage/kill details, and ring min.
// 0.8.2: the session scenarios use the session defaults (reserve pace, keep
// 2500, pre = ring minus one window, dry 4000) at 20 KB/s: the 30 s outage
// and the 60 s kill-resume are OK (byte-exact, crc right, igmax <= keep + 200
// ms), the 3 min and 12 min outages end err=recv end=d (the reserve covers
// ~31 windows x 2.5 s ~ 78 s), and "slow-link" (6 KB/s, no outage; below the
// MCU's ~11.5 KB/s once the reserve drains) must end recv d: it prints where.
// 0.8.1: the session scenarios ran BURST (windows back to back, a whole
// window buffered first) with pre 320, igap 0 and the dry limit given per
// scenario (default 30000); they print wmax/igmax from UploadResult, and an
// OK scenario (outages that outlast the 30 s default dry limit run with
// dry=600000, the knob's maximum) must be byte-exact with the crc right and wmax <= 100 ms.
//   sim <clip.bin> [store ms]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include "ChunkFeed.h"
#include "ClipOps.h"
#include "Crc32.h"
#include "UploadSession.h"
using namespace fanlogic;

// One clock for everything, in ns: body reads (~20 KB/s and stalls), a frame
// 300 us (1440 B at 40 MHz), a GPIO18 sample 300 ns (bench: 3M samples in
// ~0.9 s), delays.
static uint64_t ns = 0;
constexpr uint64_t MS = 1000000, FRAME_NS = 300000, SAMPLE_NS = 300;
static uint64_t storeNs = 30 * MS;

struct Clk : Clock { uint32_t nowMs() override { return (uint32_t)(ns / MS); } } clk;
struct Nor : FlashIo {
  std::vector<uint8_t> m; bool dirty = false;
  explicit Nor(size_t n) : m(n, 0) {}
  size_t size() const override { return m.size(); }
  bool erase(size_t o, size_t l) override { memset(&m[o], 0xFF, l); return true; }
  bool write(size_t o, const uint8_t* b, size_t l) override {
    for (size_t i = 0; i < l; i++) { if (m[o + i] != 0xFF) dirty = true; m[o + i] &= b[i]; } return true; }
  bool read(size_t o, uint8_t* b, size_t l) override { memcpy(b, &m[o], l); return true; }
};
struct Link : McuLink {
  void wake() override {}
  bool send(const char*) override { return true; }
  int transact(const char*, const char*, uint32_t, char* r, size_t n) override { snprintf(r, n, "no file!\n"); return 9; }
  int expect(const char*, const char* const*, int n, uint32_t) override { return n ? 0 : -1; }   // pause, CT, RT
  int collect(char*, size_t, uint32_t, uint32_t) override { return 0; }
};
struct Body : BodyReader {
  const std::vector<uint8_t>& d; size_t pos = 0; std::mt19937 rng{12345};
  explicit Body(const std::vector<uint8_t>& d) : d(d) {}
  int read(uint8_t* b, size_t n) override {
    if (rng() % 50 == 0) { ns += 1000 * MS; return 0; }          // short stall
    if (rng() % 4000 == 0) { ns += 1000 * MS; stall = 25; return 0; }
    if (stall > 0) { stall--; ns += 1000 * MS; return 0; }       // ~25 s stall
    size_t k = 1 + rng() % 1460; if (k > n) k = n; if (k > d.size() - pos) k = d.size() - pos;
    memcpy(b, &d[pos], k); pos += k; ns += (1 + k / 20) * MS; return (int)k;   // ~20 KB/s
  }
  int stall = 0;
};
struct Mcu : SyncLine {
  std::deque<uint64_t> done; int overruns = 0;   // when each unstored window will be stored
  void purge() { while (!done.empty() && done.front() <= ns) done.pop_front(); }
  void windowIn() {
    purge();
    if (done.size() >= 2) { overruns++; return; }
    done.push_back((done.empty() ? ns : done.back()) + storeNs);
  }
  bool high() override { ns += SAMPLE_NS; purge(); return done.empty(); }
  void yield() override {}
  void delayMs(uint32_t ms) override { ns += ms * MS; }
};
struct Spi : ClipSpi {
  Spi(std::function<bool()> allIn, bool credit) : allIn(allIn), credit(credit) {}
  std::function<bool()> allIn; bool credit; Mcu mcu; SyncCredit sync{mcu}; int flushWaits = 0;
  std::vector<uint8_t> out; std::vector<uint32_t> t;
  bool beforeFrame() override {
    if (!credit) return true;
    int before = sync.highWaits();
    bool ok = sync.beforeFrame();
    if (sync.highWaits() > before && allIn()) flushWaits++;   // body all in: the flush
    return ok;
  }
  bool sendFrame(const uint8_t* b, size_t l) override {
    out.insert(out.end(), b, b + l); t.push_back(clk.nowMs()); ns += FRAME_NS;
    if (out.size() % SPI_WINDOW == 0) mcu.windowIn();
    return true;
  }
  bool waitWindow() override { return sync.afterWindow(); }
};

// One upload; prints a line and returns true if the bytes and the CRC are right.
static bool run(const std::vector<uint8_t>& d, bool credit, int* overruns) {
  ns = 0;
  Nor nor(983040); FrameRing ring(nor); ring.begin();
  Link l; Body b(d); Spi s([&] { return b.pos == b.d.size(); }, credit); static uint8_t work[2 * SPI_FRAME]; static char scratch[8192];
  PutSource src(b, ring, clk, d.size(), work);
  UploadResult r = uploadClip(l, s, src, clk, "01-SW130.bin", d.size(), work + SPI_FRAME, scratch, sizeof(scratch));
  size_t padded = padSize(d.size());
  bool same = s.out.size() == padded && memcmp(s.out.data(), d.data(), d.size()) == 0;
  bool zeros = true; for (size_t i = d.size(); i < s.out.size(); i++) if (s.out[i]) zeros = false;
  uint32_t worst = 0; for (size_t i = 1; i < s.t.size(); i++) if (s.t[i] - s.t[i-1] > worst) worst = s.t[i] - s.t[i-1];
  uint32_t crc = crc32Update(0, s.out.data(), s.out.size());
  printf("%s: err=%s frames=%zu bytes=%zu padded=%zu identical=%d tailzeros=%d dirty=%d crc=%08x%s worstgap=%u ms "
         "sim=%u s hiw=%d (flush %d) overruns=%d\n",
         credit ? "0.7.2 credit" : "0.7.1 no credit", clipErrName(r.err), r.frames, s.out.size(), padded, same, zeros,
         nor.dirty, (unsigned)r.crc, r.crc == crc ? "" : " (MISMATCH)", worst, clk.nowMs() / 1000, s.sync.highWaits(),
         s.flushWaits, s.mcu.overruns);
  *overruns = s.mcu.overruns;
  return r.err == ClipErr::OK && same && zeros && !nor.dirty && r.crc == crc;
}

// ---- Session mode (0.8.0): a chunk client against ChunkFeed + runSession ----
// The client sends at 20 KB/s whenever simulated time advances, except in
// outages. A kill stops it; when it comes back it resends from 32 KB before
// where it was (a lost reply), which the feed must drop as duplicates.
struct SimClient {
  SimClient(const std::vector<uint8_t>& d, ChunkFeed& f, double rate = 20000.0) : d(d), feed(f), rate(rate) {}
  double rate;                                          // bytes/s
  const std::vector<uint8_t>& d;
  ChunkFeed& feed;
  size_t pos = 0;
  uint64_t lastNs = 0;
  double credit = 0;
  std::vector<std::pair<uint64_t, uint64_t>> outages;   // [from, to) ns
  uint64_t killFrom = UINT64_MAX, killTo = 0;
  bool rewound = false;
  void pump() {
    uint64_t dt = ns - lastNs;
    lastNs = ns;
    if (ns >= killFrom && ns < killTo) { credit = 0; return; }
    if (killTo && ns >= killTo && !rewound) { rewound = true; pos = pos > 32768 ? pos - 32768 : 0; }
    for (auto& o : outages) if (ns >= o.first && ns < o.second) { credit = 0; return; }
    credit += dt * rate / 1e9;
    if (credit > 65536) credit = 65536;
    size_t n = (size_t)credit;
    if (n > d.size() - pos) n = d.size() - pos;
    if (!n) return;
    size_t used = feed.write(pos, d.data() + pos, n);
    pos += used;
    credit -= used;
  }
};
struct SimSource : FrameSource {
  SimSource(ChunkFeed& f, SimClient& c) : f(f), c(c) {}
  ChunkFeed& f; SimClient& c;
  size_t count() override { return f.count(); }
  bool full() override { return f.full(); }
  bool prefilled() override { return f.prefilled(); }
  BodyState state() override { c.pump(); return f.state(); }   // catch up with time spent sending
  ClipErr failErr() override { return f.failErr(); }
  bool pop(uint8_t* fr) override { return f.pop(fr); }
  void wait(uint32_t ms) override { ns += (uint64_t)(ms == 0 ? 1 : ms < 10 ? ms : 10) * MS; c.pump(); }
  size_t bodyBytes() override { return f.bodyBytes(); }
  char bodyEnd() override { return f.bodyEnd(); }
};
struct SimLease : McuLease {
  SimLease(Link& l, Spi& s) : l(l), s(s) {}
  Link& l; Spi& s;
  McuLink* lease(uint32_t) override { return &l; }
  ClipSpi* spi(const SpiConfig&) override { return &s; }
  void diag(char* o, size_t n) override { if (n) o[0] = 0; }
  void release(bool) override {}
};

// One session on the fan-sized ring (the 0xF0000 slot); true if it ended as
// expected: ok, byte-exact, crc right and no frame gap above 1.1 s, or the
// expected error and body end.
static bool session(const char* label, const std::vector<uint8_t>& d, std::vector<std::pair<uint64_t, uint64_t>> outages,
                    uint64_t killFrom, uint64_t killTo, ClipErr wantErr, char wantEnd, double rate = 20000.0) {
  ns = 0;
  Nor nor(983040); FrameRing ring(nor); ring.begin();
  ChunkFeed feed(ring, clk);
  static uint8_t asmBuf[SPI_FRAME], frame[SPI_FRAME]; static char scratch[8192]; char diag[16];
  const PaceConfig pace = reservePace();
  feed.begin(d.size(), asmBuf, 0, SESSION_RESERVE_DRY_MS);
  SimClient c(d, feed, rate); c.outages = outages; c.killFrom = killFrom; c.killTo = killTo;
  SimSource src(feed, c);
  Link l; Spi s([&] { return feed.staged() == d.size(); }, true);
  SimLease lease(l, s);
  UploadResult r = runSession(lease, src, clk, "01-SW130.bin", d.size(), SpiConfig(), pace, frame, scratch,
                              sizeof(scratch), diag, sizeof(diag));
  bool same = s.out.size() == padSize(d.size()) && memcmp(s.out.data(), d.data(), d.size()) == 0;
  uint32_t worst = 0; for (size_t i = 1; i < s.t.size(); i++) if (s.t[i] - s.t[i-1] > worst) worst = s.t[i] - s.t[i-1];
  uint32_t crc = crc32Update(0, s.out.data(), s.out.size());
  size_t rmin = feed.ringMin();
  printf("session %-14s err=%s end=%c frames=%zu identical=%d crc=%08x%s worstgap=%u ms wmax=%u ms igmax=%u ms sim=%u s dup=%zu "
         "quiet=%u ms ringmin=%d dirty=%d\n",
         label, clipErrName(r.err), r.bodyEnd, r.frames, same, (unsigned)r.crc, r.crc == crc ? "" : " (MISMATCH)",
         worst, (unsigned)r.wmax, (unsigned)r.igmax, clk.nowMs() / 1000, feed.dupBytes(), feed.longestQuietMs(), rmin == SIZE_MAX ? -1 : (int)rmin,
         nor.dirty);
  if (wantErr != ClipErr::OK) printf("  %s failed with %zu of %zu body bytes staged (frame ~%zu of %zu), stg=%u B/s\n", label,
                                     feed.staged(), d.size(), feed.staged() / SPI_FRAME, (d.size() + SPI_FRAME - 1) / SPI_FRAME,
                                     (unsigned)feed.stagingRate());
  if (wantErr == ClipErr::OK)
    return r.err == ClipErr::OK && same && r.crc == crc && !nor.dirty && r.wmax <= 100 && r.igmax <= pace.keepMs + 200;
  return r.err == wantErr && r.bodyEnd == wantEnd;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <clip.bin> [mcu store ms per window, default 30]\n", argv[0]); return 2; }
  if (argc > 2) storeNs = (uint64_t)atoi(argv[2]) * MS;
  FILE* f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
  std::vector<uint8_t> d; int c; while ((c = fgetc(f)) != EOF) d.push_back((uint8_t)c); fclose(f);
  int withCredit = 0, without = 0;
  bool ok = run(d, true, &withCredit);
  run(d, false, &without);
  const uint64_t S = 1000 * MS;
  bool sess = session("30s-outage", d, {{60 * S, 90 * S}}, UINT64_MAX, 0, ClipErr::OK, '-');
  sess &= session("3min-outage", d, {{60 * S, 240 * S}}, UINT64_MAX, 0, ClipErr::RECV, 'd');
  sess &= session("kill-resume", d, {}, 100 * S, 160 * S, ClipErr::OK, '-');
  sess &= session("12min-outage", d, {{60 * S, 780 * S}}, UINT64_MAX, 0, ClipErr::RECV, 'd');
  sess &= session("slow-link", d, {}, UINT64_MAX, 0, ClipErr::RECV, 'd', 6000.0);
  return ok && withCredit == 0 && sess ? 0 : 1;
}
