#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "ClipOps.h"
#include "Crc32.h"
#include "ChunkFeed.h"
#include "UploadSession.h"

using namespace fanlogic;

static char scratch[8192];

void setUp() {}
void tearDown() {}

void test_upload_name_rules() {
  TEST_ASSERT_TRUE(isUploadName("99-Test.bin"));
  TEST_ASSERT_TRUE(isUploadName("02-x.bin"));
  TEST_ASSERT_FALSE(isUploadName("99.test.bin"));     // separator must be - or _
  TEST_ASSERT_FALSE(isUploadName("9-Test.bin"));
  TEST_ASSERT_FALSE(isUploadName("ab-Test.bin"));
  TEST_ASSERT_FALSE(isUploadName("99-.bin"));          // needs at least one char after '-'
  TEST_ASSERT_FALSE(isUploadName("99-Te st.bin"));
  TEST_ASSERT_FALSE(isUploadName("99-Test.BIN"));
  TEST_ASSERT_FALSE(isUploadName("99-Te^st.bin"));
  TEST_ASSERT_FALSE(isUploadName(""));
  TEST_ASSERT_FALSE(isUploadName(nullptr));
}

void test_pad_size() {
  TEST_ASSERT_EQUAL_UINT32(28800, padSize(1));
  TEST_ASSERT_EQUAL_UINT32(28800, padSize(28800));
  TEST_ASSERT_EQUAL_UINT32(57600, padSize(28801));
  TEST_ASSERT_EQUAL_UINT32(86400, padSize(79212));    // header + one frame
  TEST_ASSERT_EQUAL_UINT32(12729600, padSize(12701612)); // the vendor sample clip
}

void test_next_clip_name_skips_junk() {
  const char* p = "mculog:list\r\n\r\n02-Name.bin\r\nDCIM recv file list\r\n99_test.bin\n";
  char n[64];
  TEST_ASSERT_TRUE(nextClipName(p, n, sizeof(n)));
  TEST_ASSERT_EQUAL_STRING("02-Name.bin", n);
  TEST_ASSERT_TRUE(nextClipName(p, n, sizeof(n)));
  TEST_ASSERT_EQUAL_STRING("99_test.bin", n);
  TEST_ASSERT_FALSE(nextClipName(p, n, sizeof(n)));
}

void test_next_clip_name_no_cap() {
  static char buf[100 * 16 + 1];
  buf[0] = '\0';
  for (int i = 0; i < 100; i++) {
    char line[16];
    snprintf(line, sizeof(line), "%02d-C.bin\n", i);
    strcat(buf, line);
  }
  const char* p = buf;
  char n[64];
  int count = 0;
  while (nextClipName(p, n, sizeof(n))) count++;
  TEST_ASSERT_EQUAL(100, count);
}

void test_list_has_exact_line() {
  const char* list = "02-Name.bin\r\n03-Name.bin\n";
  TEST_ASSERT_TRUE(listHas(list, "02-Name.bin"));
  TEST_ASSERT_TRUE(listHas(list, "03-Name.bin"));
  TEST_ASSERT_FALSE(listHas(list, "2-Name.bin"));
  TEST_ASSERT_FALSE(listHas(list, "02-Name.bi"));
  TEST_ASSERT_FALSE(listHas("", "02-Name.bin"));
}

void test_json_entry() {
  char out[96];
  TEST_ASSERT_EQUAL(22, clipJsonEntry("02-Name.bin", true, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("{\"name\":\"02-Name.bin\"}", out);
  clipJsonEntry("03-Name.bin", false, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING(",{\"name\":\"03-Name.bin\"}", out);
}

void test_json_entry_escapes() {
  char out[96];
  clipJsonEntry("02-a\"b\\c.bin", true, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("{\"name\":\"02-a\\\"b\\\\c.bin\"}", out);
}

void test_json_entry_too_small() {
  char out[10];
  TEST_ASSERT_EQUAL(-1, clipJsonEntry("02-Name.bin", true, out, sizeof(out)));
}

void test_err_names() {
  TEST_ASSERT_EQUAL_STRING("ok", clipErrName(ClipErr::OK));
  TEST_ASSERT_EQUAL_STRING("cf", clipErrName(ClipErr::CF));
  TEST_ASSERT_EQUAL_STRING("noverdict", clipErrName(ClipErr::NOVERDICT));
  TEST_ASSERT_EQUAL_STRING("abort", clipErrName(ClipErr::ABORT));
  TEST_ASSERT_EQUAL_STRING("busy", clipErrName(ClipErr::BUSY));
}

// Scripted MCU: each "s\n" pops the next list (the last one repeats);
// each expect() pops the next scripted token index; records every line.
class FakeLink : public McuLink {
 public:
  std::deque<std::string> lists;
  std::deque<int> expects;
  std::vector<std::string> lines;
  // The MCU prints a log line after `u`: a quiet-ended transact then
  // returns after ~350 ms. uWaitMs is how long the link waited after `u`.
  uint32_t uWaitMs = 0;
  // Bytes the MCU sends after a quiet gap following an "s" reply.
  std::string lateTail;
  bool lastWasList = false;
  std::function<void(const std::string&)> onLine;
  void note(const char* line) { if (onLine) onLine(line); }
  void wake() override {}
  bool send(const char* line) override { lines.push_back(line); return true; }
  int transact(const char* line, const char*, uint32_t timeoutMs, char* reply, size_t size) override {
    lines.push_back(line);
    note(line);
    lastWasList = strcmp(line, "s\n") == 0;
    if (line[0] == 'u') uWaitMs = timeoutMs < 350 ? timeoutMs : 350;
    std::string r;
    if (strcmp(line, "s\n") == 0 && !lists.empty()) {
      r = lists.front();
      if (lists.size() > 1) lists.pop_front();
    }
    snprintf(reply, size, "%s", r.c_str());
    return (int)strlen(reply);
  }
  std::vector<std::string> expectFirstToken;   // tokens[0] of each expect() call ("" if none)
  int expect(const char* line, const char* const* tokens, int n, uint32_t timeoutMs) override {
    expectFirstToken.push_back(n > 0 ? tokens[0] : "");
    if (line) { lines.push_back(line); note(line); }
    lastWasList = false;
    if (line && line[0] == 'u' && n == 0) uWaitMs = timeoutMs;   // no token can end it
    if (n == 0) return -1;
    if (line && strcmp(line, "h\n") == 0) return 0;              // the MCU logs mculog:pause.
    if (expects.empty()) return -1;
    int v = expects.front();
    expects.pop_front();
    return v;
  }
  int collect(char* buf, size_t size, uint32_t, uint32_t) override {
    lines.push_back("<collect>");
    int n = 0;
    if (buf && lastWasList && !lateTail.empty()) {
      size_t len = strlen(buf);
      snprintf(buf + len, size - len, "%s", lateTail.c_str());
      n = (int)(strlen(buf) - len);
    }
    lastWasList = false;
    return n;
  }
  int count(const std::string& l) const {
    int n = 0;
    for (const auto& x : lines) if (x == l) n++;
    return n;
  }
};


void test_delete_ok() {
  FakeLink l;
  l.lists = {"02-Name.bin\n99-Test.bin\n", "02-Name.bin\n"};
  TEST_ASSERT_EQUAL(ClipErr::OK, deleteClip(l, "99-Test.bin", scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(1, l.count("u 99-Test.bin\n"));
  TEST_ASSERT_EQUAL(2, l.count("s\n"));
}

void test_delete_not_found() {
  FakeLink l;
  l.lists = {"02-Name.bin\n"};
  TEST_ASSERT_EQUAL(ClipErr::NOT_FOUND, deleteClip(l, "99-Test.bin", scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(0, l.count("u 99-Test.bin\n"));
}

void test_delete_still_there() {
  FakeLink l;
  l.lists = {"99-Test.bin\n"};
  TEST_ASSERT_EQUAL(ClipErr::STILL, deleteClip(l, "99-Test.bin", scratch, sizeof(scratch)));
}

// A delete is confirmed only by a listing the MCU really gave: the clip gone
// and every other clip still listed. The MCU drops lines while it is busy
// (deleting the clip it plays makes it switch clips), so an empty or cut-off
// reply is listed again; with no usable listing it is STILL, never OK.
void test_delete_empty_listing_after_is_not_ok() {
  FakeLink l;
  l.lists = {"02-Name.bin\n99-Test.bin\n", ""};
  TEST_ASSERT_EQUAL(ClipErr::STILL, deleteClip(l, "99-Test.bin", scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(1 + DELETE_LIST_TRIES, l.count("s\n"));
}

void test_delete_empty_then_full_listing_is_ok() {
  FakeLink l;
  l.lists = {"02-Name.bin\n03-Name.bin\n99-Test.bin\n", "", "02-Name.bin\n03-Name.bin\n"};
  TEST_ASSERT_EQUAL(ClipErr::OK, deleteClip(l, "99-Test.bin", scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(3, l.count("s\n"));
}

void test_delete_cut_off_listing_is_listed_again() {
  FakeLink l;
  l.lists = {"02-Name.bin\n03-Name.bin\n99-Test.bin\n", "02-Name.bin\n", "02-Name.bin\n03-Name.bin\n"};
  TEST_ASSERT_EQUAL(ClipErr::OK, deleteClip(l, "99-Test.bin", scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(3, l.count("s\n"));
}

// The only clip on the card: an empty listing is the right answer.
void test_delete_only_clip_empty_listing_is_ok() {
  FakeLink l;
  l.lists = {"99-Test.bin\n", ""};
  TEST_ASSERT_EQUAL(ClipErr::OK, deleteClip(l, "99-Test.bin", scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(2, l.count("s\n"));
}

void test_delete_bad_name_sends_nothing() {
  FakeLink l;
  l.lists = {"v\n"};
  TEST_ASSERT_EQUAL(ClipErr::NAME, deleteClip(l, "v", scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(ClipErr::NAME, deleteClip(l, "a b.bin", scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(0, (int)l.lines.size());
}

void test_delete_underscore_name_allowed() {
  FakeLink l;
  l.lists = {"99_test.bin\n", ""};
  TEST_ASSERT_EQUAL(ClipErr::OK, deleteClip(l, "99_test.bin", scratch, sizeof(scratch)));
}

class FakeClock : public Clock {
 public:
  uint32_t ms = 0;
  uint32_t nowMs() override { return ms; }
};
static FakeClock clk;

// RAM "flash" for the ring (erase = 0xFF; writes are plain copies here; the
// NOR semantics are tested in test_frame_ring).
class RamFlash : public FlashIo {
 public:
  explicit RamFlash(size_t n) : mem(n, 0xFF) {}
  std::vector<uint8_t> mem;
  size_t size() const override { return mem.size(); }
  bool erase(size_t off, size_t len) override { memset(&mem[off], 0xFF, len); return true; }
  bool write(size_t off, const uint8_t* b, size_t len) override { memcpy(&mem[off], b, len); return true; }
  bool read(size_t off, uint8_t* b, size_t len) override { memcpy(b, &mem[off], len); return true; }
};

class FakeSpi : public ClipSpi {
 public:
  std::vector<uint32_t> times;               // clk.ms at each frame
  std::vector<std::string>* log = nullptr;   // optional: note "<frame>" in the link's line log
  std::vector<uint8_t> bytes;
  int frames = 0, windows = 0;
  int failWindowAt = -1;      // waitWindow() index that fails
  int failFrameAt = -1;       // sendFrame() index that fails
  int credits = 0;
  int failCreditAt = -1;      // beforeFrame() index that fails (GPIO18 never HIGH)
  bool beforeFrame() override {
    if (log) log->push_back("<credit>");
    return credits++ != failCreditAt;
  }
  bool sendFrame(const uint8_t* buf, size_t len) override {
    if (frames == failFrameAt) return false;
    frames++;
    times.push_back(clk.ms);
    if (log) log->push_back("<frame>");
    bytes.insert(bytes.end(), buf, buf + len);
    return true;
  }
  bool waitWindow() override { return windows++ != failWindowAt; }
};

// Serves `data` in chunks of `chunk`; fails (returns -1) once `failAfter`
// bytes have been served.
class FakeBody : public BodyReader {
 public:
  std::vector<uint8_t> data;
  size_t pos = 0, chunk = 1000, failAfter = (size_t)-1;
  // Wi-Fi stall: once pos reaches stallAt, the next stallReads reads return
  // 0 ("nothing yet"), each taking 1000 ms.
  size_t stallAt = (size_t)-1;
  int stallReads = 0;
  int read(uint8_t* buf, size_t n) override {
    if (pos >= stallAt && stallReads > 0) { stallReads--; clk.ms += 1000; return 0; }
    if (pos >= failAfter || pos >= data.size()) return -1;
    size_t k = n < chunk ? n : chunk;
    if (k > data.size() - pos) k = data.size() - pos;
    if (k > failAfter - pos) k = failAfter - pos;
    memcpy(buf, data.data() + pos, k);
    pos += k;
    return (int)k;
  }
};

static uint8_t window[SPI_WINDOW];

// The upload under test with a RAM ring of `ringBytes` (default: big enough
// to hold any test clip, as the fan's 921,600-byte ring holds most clips).
// `work` holds two frames: PutSource assembles in the first, uploadClip
// sends from the second (as the fan's single PUT).
static UploadResult up(FakeLink& l, FakeSpi& s, FakeBody& b, const char* name, size_t size,
                       size_t ringBytes = RING_BLOCK * 5, uint8_t* work = window) {
  static RamFlash* fl = nullptr;
  delete fl;
  fl = new RamFlash(ringBytes);
  static FrameRing* ring = nullptr;
  delete ring;
  ring = new FrameRing(*fl);
  ring->begin();
  clk.ms = 0;
  static PutSource* src = nullptr;
  delete src;
  src = new PutSource(b, *ring, clk, size, work);
  return uploadClip(l, s, *src, clk, name, size, work + SPI_FRAME, scratch, sizeof(scratch));
}

// As up(), with this upload's pace options (0.9.8: pace.dark).
static UploadResult upPace(FakeLink& l, FakeSpi& s, FakeBody& b, const char* name, size_t size, const PaceConfig& p) {
  static RamFlash* fl = nullptr;
  delete fl;
  fl = new RamFlash(RING_BLOCK * 5);
  static FrameRing* ring = nullptr;
  delete ring;
  ring = new FrameRing(*fl);
  ring->begin();
  clk.ms = 0;
  static PutSource* src = nullptr;
  delete src;
  src = new PutSource(b, *ring, clk, size, window);
  return uploadClip(l, s, *src, clk, name, size, window + SPI_FRAME, scratch, sizeof(scratch), p);
}

static std::vector<uint8_t> pattern(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = (uint8_t)(i * 7 + 1);
  return v;
}

void test_upload_happy_path() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"02-Name.bin\n"};
  l.expects = {0, 0};                         // CT, RT
  b.data = pattern(79212);
  UploadResult r = up(l, s, b, "99-Test.bin", b.data.size());
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_FALSE(r.partial);
  TEST_ASSERT_EQUAL(1, l.count("w 99-Test.bin 86400\n"));
  TEST_ASSERT_EQUAL(60, s.frames);
  TEST_ASSERT_EQUAL(3, s.windows);
  TEST_ASSERT_EQUAL(86400, (int)s.bytes.size());
  TEST_ASSERT_EQUAL_MEMORY(b.data.data(), s.bytes.data(), b.data.size());
  for (size_t i = b.data.size(); i < s.bytes.size(); i++) TEST_ASSERT_EQUAL_UINT8(0, s.bytes[i]);
  TEST_ASSERT_EQUAL(0, l.count("u 99-Test.bin\n"));
}

void test_upload_exact_window_no_extra_window() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {""};
  l.expects = {0, 0};
  b.data = pattern(SPI_WINDOW);
  UploadResult r = up(l, s, b, "99-Test.bin", SPI_WINDOW);
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL(1, l.count("w 99-Test.bin 28800\n"));
  TEST_ASSERT_EQUAL(20, s.frames);
  TEST_ASSERT_EQUAL(1, s.windows);
}

void test_upload_exists() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"99-Test.bin\n"};
  b.data = pattern(100);
  UploadResult r = up(l, s, b, "99-Test.bin", 100);
  TEST_ASSERT_EQUAL(ClipErr::EXISTS, r.err);
  TEST_ASSERT_EQUAL(0, s.frames);
  TEST_ASSERT_EQUAL(0, l.count("w 99-Test.bin 28800\n"));
}

void test_upload_bad_name_and_length() {
  FakeLink l; FakeSpi s; FakeBody b;
  TEST_ASSERT_EQUAL(ClipErr::NAME, up(l, s, b, "99.test.bin", 100).err);
  TEST_ASSERT_EQUAL(ClipErr::LENGTH, up(l, s, b, "99-Test.bin", 0).err);
  TEST_ASSERT_EQUAL(ClipErr::LENGTH,
                    up(l, s, b, "99-Test.bin", CLIP_MAX_BYTES + 1).err);
  TEST_ASSERT_EQUAL(0, (int)l.lines.size());
}

void test_upload_create_refused() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {""};
  l.expects = {1};                            // CF
  b.data = pattern(100);
  UploadResult r = up(l, s, b, "99-Test.bin", 100);
  TEST_ASSERT_EQUAL(ClipErr::CF, r.err);
  TEST_ASSERT_FALSE(r.partial);
  TEST_ASSERT_EQUAL(0, s.frames);
  TEST_ASSERT_EQUAL(0, l.count("u 99-Test.bin\n"));
}

void test_upload_create_timeout_cleans_up() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"", "02-Name.bin\n"};            // before; after cleanup
  l.expects = {};                             // -1: silent
  b.data = pattern(100);
  UploadResult r = up(l, s, b, "99-Test.bin", 100);
  TEST_ASSERT_EQUAL(ClipErr::CF, r.err);
  TEST_ASSERT_FALSE(r.partial);
  TEST_ASSERT_EQUAL(1, l.count("u 99-Test.bin\n"));
  TEST_ASSERT_EQUAL(0, s.frames);
}

void test_upload_rf_deletes() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"", "02-Name.bin\n"};
  l.expects = {0, 1};                         // CT, RF
  b.data = pattern(100);
  UploadResult r = up(l, s, b, "99-Test.bin", 100);
  TEST_ASSERT_EQUAL(ClipErr::RF, r.err);
  TEST_ASSERT_FALSE(r.partial);
  TEST_ASSERT_EQUAL(1, l.count("u 99-Test.bin\n"));
}

void test_upload_no_verdict() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"", "99-Test.bin\n"};            // cleanup did not take
  l.expects = {0};
  b.data = pattern(100);
  UploadResult r = up(l, s, b, "99-Test.bin", 100);
  TEST_ASSERT_EQUAL(ClipErr::NOVERDICT, r.err);
  TEST_ASSERT_TRUE(r.partial);
}

void test_upload_sync_failure_stops_sending() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"", "02-Name.bin\n"};
  l.expects = {0};
  s.failWindowAt = 1;                         // second window's handshake fails
  b.data = pattern(79212);
  UploadResult r = up(l, s, b, "99-Test.bin", b.data.size());
  TEST_ASSERT_EQUAL(ClipErr::SYNC, r.err);
  TEST_ASSERT_EQUAL(40, s.frames);            // no third window
  TEST_ASSERT_EQUAL(1, l.count("u 99-Test.bin\n"));
}

void test_upload_spi_failure() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"", ""};
  l.expects = {0};
  s.failFrameAt = 5;
  b.data = pattern(100);
  UploadResult r = up(l, s, b, "99-Test.bin", 100);
  TEST_ASSERT_EQUAL(ClipErr::SPI, r.err);
  TEST_ASSERT_EQUAL(5, s.frames);
  TEST_ASSERT_EQUAL(0, s.windows);
  TEST_ASSERT_TRUE(r.partial);                // empty re-list: cannot confirm it is gone
}

// The body is buffered before "w": if it fails then, nothing is created.
void test_upload_body_fails_during_prefill_creates_nothing() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {""};
  l.expects = {0, 0};
  b.data = pattern(79212);
  b.failAfter = 30000;
  UploadResult r = up(l, s, b, "99-Test.bin", b.data.size());
  TEST_ASSERT_EQUAL(ClipErr::RECV, r.err);
  TEST_ASSERT_FALSE(r.partial);
  TEST_ASSERT_EQUAL(0, s.frames);
  TEST_ASSERT_EQUAL(0, l.count("w 99-Test.bin 86400\n"));
  TEST_ASSERT_EQUAL(0, l.count("h\n"));
  TEST_ASSERT_EQUAL_UINT32(30000, (uint32_t)r.bodyBytes);   // diagnostics: how far it got
  TEST_ASSERT_EQUAL('e', r.bodyEnd);                         // and why: read error
}

// A clip larger than the ring: the body dies after "w"; the MCU still gets
// its declared size (zeros), then the partial file is deleted.
void test_upload_body_fails_midstream_pads_zeros() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"", "02-Name.bin\n"};
  l.expects = {0, 0};
  b.data = pattern(SPI_FRAME * 200);          // ring below holds ~125 frames
  b.failAfter = SPI_FRAME * 150;
  UploadResult r = up(l, s, b, "99-Test.bin", b.data.size(), RING_BLOCK);
  TEST_ASSERT_EQUAL(ClipErr::RECV, r.err);
  TEST_ASSERT_EQUAL(200, s.frames);           // padSize(288000) = 288000 = 200 frames
  TEST_ASSERT_EQUAL_MEMORY(b.data.data(), s.bytes.data(), SPI_FRAME * 150);
  for (size_t i = SPI_FRAME * 150; i < s.bytes.size(); i++) TEST_ASSERT_EQUAL_UINT8(0, s.bytes[i]);
  TEST_ASSERT_EQUAL(1, l.count("u 99-Test.bin\n"));
}

void test_delete_waits_full_settle_time() {
  FakeLink l;
  l.lists = {"99-Test.bin\n", ""};
  deleteClip(l, "99-Test.bin", scratch, sizeof(scratch));
  TEST_ASSERT_EQUAL_UINT32(DELETE_READ_MS, l.uWaitMs);
}

void test_upload_cleanup_waits_full_settle_time() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"", "02-Name.bin\n"};
  l.expects = {0, 1};                         // CT, RF
  b.data = pattern(100);
  up(l, s, b, "99-Test.bin", 100);
  TEST_ASSERT_EQUAL_UINT32(DELETE_READ_MS, l.uWaitMs);
}

void test_decode_clip_path() {
  char n[64];
  TEST_ASSERT_TRUE(decodeClipPath("99-Test.bin", n, sizeof(n)));
  TEST_ASSERT_EQUAL_STRING("99-Test.bin", n);
  TEST_ASSERT_TRUE(decodeClipPath("01-a%22b.bin?x=1", n, sizeof(n)));
  TEST_ASSERT_EQUAL_STRING("01-a\"b.bin", n);
  TEST_ASSERT_TRUE(decodeClipPath("01-q%3F.bin#frag", n, sizeof(n)));
  TEST_ASSERT_EQUAL_STRING("01-q?.bin", n);
  TEST_ASSERT_TRUE(decodeClipPath("01-%2a.bin", n, sizeof(n)));
  TEST_ASSERT_EQUAL_STRING("01-*.bin", n);
  TEST_ASSERT_FALSE(decodeClipPath("", n, sizeof(n)));
  TEST_ASSERT_FALSE(decodeClipPath("?x", n, sizeof(n)));
  TEST_ASSERT_FALSE(decodeClipPath("01-%00.bin", n, sizeof(n)));
  TEST_ASSERT_FALSE(decodeClipPath("01-%g1.bin", n, sizeof(n)));
  TEST_ASSERT_FALSE(decodeClipPath("01-%2", n, sizeof(n)));
  char small[8];
  TEST_ASSERT_FALSE(decodeClipPath("01-Test.bin", small, sizeof(small)));   // never truncated
  char exact[9];
  TEST_ASSERT_TRUE(decodeClipPath("01-T.bin", exact, sizeof(exact)));      // 8 chars + NUL fits
}

void test_upload_name_rejects_path_chars() {
  TEST_ASSERT_FALSE(isUploadName("99-a/b.bin"));
  TEST_ASSERT_FALSE(isUploadName("99-a\\b.bin"));
  TEST_ASSERT_FALSE(isUploadName("99-a:b.bin"));
}

void test_upload_needs_only_two_frames_of_ram() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {""};
  l.expects = {0, 0};
  b.data = pattern(79212);
  static uint8_t guarded[2 * SPI_FRAME + 64];
  memset(guarded + 2 * SPI_FRAME, 0xA5, 64);   // canary right after two frames
  UploadResult r = up(l, s, b, "99-Test.bin", b.data.size(), RING_BLOCK * 5, guarded);
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  for (int i = 0; i < 64; i++) TEST_ASSERT_EQUAL_HEX8(0xA5, guarded[2 * SPI_FRAME + i]);
  TEST_ASSERT_EQUAL(60, s.frames);
  TEST_ASSERT_EQUAL_MEMORY(b.data.data(), s.bytes.data(), b.data.size());
}

static int indexOf(const FakeLink& l, const std::string& line) {
  for (size_t i = 0; i < l.lines.size(); i++) if (l.lines[i] == line) return (int)i;
  return -1;
}

// Bench 2026-10-06: an upload while a clip plays fails the GPIO18 wait (sync).
void test_upload_pauses_playback_before_create() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {""};
  l.expects = {0, 0};
  b.data = pattern(100);
  up(l, s, b, "99-Test.bin", 100);
  int h = indexOf(l, "h\n"), w = indexOf(l, "w 99-Test.bin 28800\n");
  TEST_ASSERT_TRUE(h >= 0);
  TEST_ASSERT_TRUE(h < w);
}

// 0.9.8 (by design, a dark fan during uploads is fine): pace.dark switches to the
// black clip when the card has it, else dims to brightness 1, and lets the
// MCU settle, all before the pause.
void test_dark_upload_plays_the_black_clip_first() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {std::string("02-Name.bin\n") + BLACK_CLIP + "\n"};
  l.expects = {0, 0};
  b.data = pattern(100);
  PaceConfig p;
  p.dark = true;
  TEST_ASSERT_EQUAL(ClipErr::OK, upPace(l, s, b, "99-Test.bin", 100, p).err);
  int t = indexOf(l, std::string("t ") + BLACK_CLIP + "\n"), h = indexOf(l, "h\n");
  TEST_ASSERT_TRUE(t >= 0);
  TEST_ASSERT_TRUE(t < h);
  TEST_ASSERT_EQUAL_STRING("<collect>", l.lines[t + 1].c_str());
  TEST_ASSERT_EQUAL(0, l.count("k 1\n"));
}

void test_dark_upload_without_the_black_clip_dims() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"02-Name.bin\n"};
  l.expects = {0, 0};
  b.data = pattern(100);
  PaceConfig p;
  p.dark = true;
  TEST_ASSERT_EQUAL(ClipErr::OK, upPace(l, s, b, "99-Test.bin", 100, p).err);
  int k = indexOf(l, "k 1\n"), h = indexOf(l, "h\n");
  TEST_ASSERT_TRUE(k >= 0);
  TEST_ASSERT_TRUE(k < h);
  TEST_ASSERT_EQUAL_STRING("<collect>", l.lines[k + 1].c_str());
}

void test_normal_upload_does_not_go_dark() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {std::string("02-Name.bin\n") + BLACK_CLIP + "\n"};
  l.expects = {0, 0};
  b.data = pattern(100);
  up(l, s, b, "99-Test.bin", 100);
  TEST_ASSERT_EQUAL(-1, indexOf(l, std::string("t ") + BLACK_CLIP + "\n"));
  TEST_ASSERT_EQUAL(0, l.count("k 1\n"));
}

void test_upload_reports_frames_sent() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"", ""};
  l.expects = {0};
  s.failWindowAt = 1;
  b.data = pattern(79212);
  UploadResult r = up(l, s, b, "99-Test.bin", b.data.size());
  TEST_ASSERT_EQUAL(ClipErr::SYNC, r.err);
  TEST_ASSERT_EQUAL_UINT32(40, (uint32_t)r.frames);
}

// Bench trace 2026-10-06: a "w" sent 5 ms after "h" is dropped while the MCU
// pauses, and SPI data sent right after CT is lost while it is still printing.
void test_upload_settles_after_pause_and_create() {
  FakeLink l; FakeSpi s; FakeBody b;
  s.log = &l.lines;
  l.lists = {""};
  l.expects = {0, 0};
  b.data = pattern(100);
  TEST_ASSERT_EQUAL(ClipErr::OK, up(l, s, b, "99-Test.bin", 100).err);
  int h = indexOf(l, "h\n"), w = indexOf(l, "w 99-Test.bin 28800\n"), f = indexOf(l, "<frame>");
  TEST_ASSERT_TRUE(h >= 0 && w > h && f > w);
  TEST_ASSERT_EQUAL_STRING("<collect>", l.lines[h + 1].c_str());
  TEST_ASSERT_EQUAL_STRING("<collect>", l.lines[w + 1].c_str());
}

// The "s" reply can arrive with a gap; a name in the late part still counts.
void test_list_reads_a_late_tail() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"02-Name.bin\n"};
  l.lateTail = "99-Test.bin\n";
  b.data = pattern(100);
  TEST_ASSERT_EQUAL(ClipErr::EXISTS, up(l, s, b, "99-Test.bin", 100).err);
}

// The vendor app names clips NN_<name>.bin; accept both separators.
void test_upload_name_accepts_underscore() {
  TEST_ASSERT_TRUE(isUploadName("09_Test.bin"));
  TEST_ASSERT_TRUE(isUploadName("09-Test.bin"));
  TEST_ASSERT_FALSE(isUploadName("09.Test.bin"));
  TEST_ASSERT_FALSE(isUploadName("09_.bin"));
}

// Bench: an upload can leave a file the MCU's "s" list never shows; force
// sends "u" without the list check (the name is still validated).
void test_delete_force_skips_list_check() {
  FakeLink l;
  l.lists = {"02-Name.bin\n"};
  TEST_ASSERT_EQUAL(ClipErr::OK, deleteClip(l, "98-Fresh.bin", scratch, sizeof(scratch), true));
  TEST_ASSERT_EQUAL(1, l.count("u 98-Fresh.bin\n"));
  TEST_ASSERT_EQUAL(ClipErr::NAME, deleteClip(l, "a b.bin", scratch, sizeof(scratch), true));
}

// The SD format (MCU "v") is sent only with the exact confirmation token,
// after pausing; buildLine() still refuses "v" everywhere else.
void test_format_needs_exact_token() {
  FakeLink l;
  l.lists = {""};
  TEST_ASSERT_EQUAL(ClipErr::NAME, formatCard(l, nullptr, scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(ClipErr::NAME, formatCard(l, "format-sd-card", scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(ClipErr::NAME, formatCard(l, "FORMAT-SD-CARD ", scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(0, l.count("v\n"));
  TEST_ASSERT_EQUAL(0, (int)l.lines.size());
  char out[MCU_LINE_MAX];
  TEST_ASSERT_EQUAL(-1, buildLine("v", nullptr, 0, out, sizeof(out)));
}

void test_format_sends_v_once_after_pause() {
  FakeLink l;
  l.lists = {""};
  TEST_ASSERT_EQUAL(ClipErr::OK, formatCard(l, FORMAT_CONFIRM, scratch, sizeof(scratch)));
  TEST_ASSERT_EQUAL(1, l.count("v\n"));
  int h = indexOf(l, "h\n"), v = indexOf(l, "v\n"), s = -1;
  for (size_t i = (size_t)v + 1; i < l.lines.size(); i++) if (l.lines[i] == "s\n") { s = (int)i; break; }
  TEST_ASSERT_TRUE(h >= 0 && v > h && s > v);
}

// Bench 2026-10-06: "mculog:format." arrives well after 3 s of quiet; an
// upload started before it saw the old files. Wait for the token itself.
void test_format_waits_for_format_log() {
  FakeLink l;
  l.lists = {""};
  formatCard(l, FORMAT_CONFIRM, scratch, sizeof(scratch));
  bool waited = false;
  for (const auto& t : l.expectFirstToken) if (t == "format") waited = true;
  TEST_ASSERT_TRUE(waited);
}

// Bench: MCU output during streaming was only read at cleanup, so its
// timing was lost. Poll the UART after every frame (0.8.1) and again after
// every window's GPIO18 wait.
void test_upload_polls_mcu_after_each_frame() {
  FakeLink l; FakeSpi s; FakeBody b;
  s.log = &l.lines;
  l.lists = {""};
  l.expects = {0, 0};
  b.data = pattern(79212);                    // 3 windows = 60 frames
  TEST_ASSERT_EQUAL(ClipErr::OK, up(l, s, b, "99-Test.bin", b.data.size()).err);
  int polls = 0, frames = 0;
  for (size_t i = 0; i < l.lines.size(); i++) {
    if (l.lines[i] != "<frame>") continue;
    frames++;
    TEST_ASSERT_TRUE(i + 1 < l.lines.size());
    if (l.lines[i + 1] == "<collect>") polls++;
  }
  TEST_ASSERT_EQUAL(60, frames);
  TEST_ASSERT_EQUAL(60, polls);
}

// The body (up to the ring's size) is buffered before "w", so Wi-Fi stalls
// cannot starve the MCU at the start.
void test_upload_prefills_before_create() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {""};
  l.expects = {0, 0};
  b.data = pattern(79212);
  size_t posAtCreate = 0;
  l.onLine = [&](const std::string& line) { if (line[0] == 'w') posAtCreate = b.pos; };
  TEST_ASSERT_EQUAL(ClipErr::OK, up(l, s, b, "99-Test.bin", b.data.size()).err);
  TEST_ASSERT_EQUAL_UINT32(79212, (uint32_t)posAtCreate);
}

// Bench 2026-10-06: the MCU aborts if SPI data stops for 2.6-5 s. A 40 s
// Wi-Fi stall mid-clip must reach the MCU as frames at most ~2 s apart.
void test_upload_trickles_through_a_long_stall() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {""};
  l.expects = {0, 0};
  b.data = pattern(SPI_FRAME * 300);          // more than the ring below
  b.stallAt = SPI_FRAME * 200;
  b.stallReads = 40;
  UploadResult r = up(l, s, b, "99-Test.bin", b.data.size(), RING_BLOCK);
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL(300, s.frames);
  TEST_ASSERT_EQUAL_MEMORY(b.data.data(), s.bytes.data(), b.data.size());
  uint32_t worst = 0;
  for (size_t i = 1; i < s.times.size(); i++)
    if (s.times[i] - s.times[i - 1] > worst) worst = s.times[i] - s.times[i - 1];
  TEST_ASSERT_TRUE(worst <= TRICKLE_MS + 1000);
  TEST_ASSERT_TRUE(clk.ms >= 40000);          // the stall really happened
}

// A stall longer than BODY_STALL_MS is a failed body: zeros, RECV, cleanup.
void test_upload_body_stall_too_long_fails() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"", ""};
  l.expects = {0, 0};
  b.data = pattern(SPI_FRAME * 300);
  b.stallAt = SPI_FRAME * 200;
  b.stallReads = 1000;
  UploadResult r = up(l, s, b, "99-Test.bin", b.data.size(), RING_BLOCK);
  TEST_ASSERT_EQUAL(ClipErr::RECV, r.err);
  TEST_ASSERT_EQUAL(300, s.frames);
  TEST_ASSERT_EQUAL(1, l.count("u 99-Test.bin\n"));
  TEST_ASSERT_EQUAL('s', r.bodyEnd);                         // stalled out
  TEST_ASSERT_EQUAL_UINT32(SPI_FRAME * 200, (uint32_t)r.bodyBytes);
}

// Bench diagnostic: the CRC32 (zlib) of every byte handed to sendFrame,
// body frames and zero tail alike, so /api/info can prove what left the C2.
void test_upload_reports_crc_of_every_frame_sent() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {""};
  l.expects = {0, 0};
  b.data = pattern(79212);                    // 55 body frames + 5 zero frames
  UploadResult r = up(l, s, b, "99-Test.bin", b.data.size());
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL(86400, (int)s.bytes.size());
  TEST_ASSERT_EQUAL_HEX32(crc32Update(0, s.bytes.data(), s.bytes.size()), r.crc);
  TEST_ASSERT_NOT_EQUAL(crc32Update(0, b.data.data(), b.data.size()), r.crc);   // the zero tail counts
}

// A failed upload's CRC covers exactly the `frames` frames it sent.
void test_upload_crc_covers_frames_sent_on_failure() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"", ""};
  l.expects = {0};
  s.failWindowAt = 1;
  b.data = pattern(79212);
  UploadResult r = up(l, s, b, "99-Test.bin", b.data.size());
  TEST_ASSERT_EQUAL(ClipErr::SYNC, r.err);
  TEST_ASSERT_EQUAL_UINT32(40, (uint32_t)r.frames);
  TEST_ASSERT_EQUAL_HEX32(crc32Update(0, b.data.data(), 40 * SPI_FRAME), r.crc);
}

// The GPIO18 credit check runs right before every frame, body and zero tail
// alike (stock skips it for the tail and sends it blind; see SyncCredit).
void test_upload_checks_credit_before_every_frame() {
  FakeLink l; FakeSpi s; FakeBody b;
  s.log = &l.lines;
  l.lists = {""};
  l.expects = {0, 0};
  b.data = pattern(79212);                    // 60 frames
  TEST_ASSERT_EQUAL(ClipErr::OK, up(l, s, b, "99-Test.bin", b.data.size()).err);
  TEST_ASSERT_EQUAL(60, s.credits);
  int framed = 0;
  for (size_t i = 0; i < l.lines.size(); i++)
    if (l.lines[i] == "<frame>") {
      TEST_ASSERT_EQUAL_STRING("<credit>", l.lines[i - 1].c_str());
      framed++;
    }
  TEST_ASSERT_EQUAL(60, framed);
}

// The capped HIGH wait expiring is a sync failure: no more frames, cleanup.
void test_upload_credit_timeout_is_sync() {
  FakeLink l; FakeSpi s; FakeBody b;
  l.lists = {"", "02-Name.bin\n"};
  l.expects = {0};
  s.failCreditAt = 25;
  b.data = pattern(79212);
  UploadResult r = up(l, s, b, "99-Test.bin", b.data.size());
  TEST_ASSERT_EQUAL(ClipErr::SYNC, r.err);
  TEST_ASSERT_EQUAL(25, s.frames);
  TEST_ASSERT_EQUAL_UINT32(25, (uint32_t)r.frames);
  TEST_ASSERT_EQUAL(1, l.count("u 99-Test.bin\n"));
}

// ---- Resumable session: runSession over a ChunkFeed ----

// A chunk client: every wait() moves the clock up to 10 ms and the client
// writes bytesPerTick bytes, except during [outFrom, outTo); at abortAt it
// aborts the feed; at rewindAt it steps back rewindBy bytes (a lost reply:
// it resends what the fan already has).
class ScriptedSession : public FrameSource {
 public:
  ScriptedSession(ChunkFeed& f, const std::vector<uint8_t>& d) : feed(f), data(d) {}
  std::string gate;                                    // 'B' windowBegin, 'E' windowEnd
  bool inGate = false;
  int waitsInWindow = 0;                               // wait() calls between windowBegin and windowEnd
  bool gateBlocksClient = false;                       // as on the fan: the chunk handler cannot write while the gate is held
  void windowBegin() override { gate += 'B'; inGate = true; }
  void windowEnd() override { gate += 'E'; inGate = false; }
  ChunkFeed& feed;
  const std::vector<uint8_t>& data;
  size_t pos = 0, bytesPerTick = 300;                  // 30 KB/s
  uint32_t outFrom = UINT32_MAX, outTo = 0, abortAt = UINT32_MAX, rewindAt = UINT32_MAX;
  size_t rewindBy = 0;
  void client() {
    if (clk.ms >= abortAt) { feed.abort(); abortAt = UINT32_MAX; }
    if (clk.ms >= rewindAt) { pos = pos > rewindBy ? pos - rewindBy : 0; rewindAt = UINT32_MAX; }
    if (clk.ms >= outFrom && clk.ms < outTo) return;
    size_t n = bytesPerTick < data.size() - pos ? bytesPerTick : data.size() - pos;
    if (n) pos += feed.write(pos, data.data() + pos, n);
  }
  size_t count() override { return feed.count(); }
  bool full() override { return feed.full(); }
  bool prefilled() override { return feed.prefilled(); }
  BodyState state() override { return feed.state(); }
  ClipErr failErr() override { return feed.failErr(); }
  std::vector<size_t> countAtPop;                      // count() before each pop
  std::vector<bool> arrivingAtPop;                     // body still arriving at that pop
  std::vector<bool> fullAtPop;                         // ring full at that pop
  bool pop(uint8_t* f) override {
    fullAtPop.push_back(feed.full());
    countAtPop.push_back(feed.count());
    arrivingAtPop.push_back(feed.state() == BodyState::ARRIVING);
    return feed.pop(f);
  }
  void wait(uint32_t ms) override {
    clk.ms += ms == 0 ? 1 : ms < 10 ? ms : 10;
    if (inGate) waitsInWindow++;
    if (inGate && gateBlocksClient) return;
    client();
  }
  size_t bodyBytes() override { return feed.bodyBytes(); }
  char bodyEnd() override { return feed.bodyEnd(); }
};

class FakeLease : public fanlogic::McuLease {
 public:
  FakeLease(FakeLink& l, FakeSpi& s) : link_(l), spi_(s) {}
  int failFirst = 0;                                   // lease() returns null this often, each after its timeout
  int leases = 0, releases = 0;
  bool lastRefresh = false;
  bool spiFails = false;                               // spi() returns nullptr when set
  McuLink* lease(uint32_t timeoutMs) override {
    leases++;
    if (failFirst > 0) { failFirst--; clk.ms += timeoutMs; return nullptr; }
    return &link_;
  }
  ClipSpi* spi(const SpiConfig&) override { return spiFails ? nullptr : &spi_; }
  void diag(char* out, size_t size) override { snprintf(out, size, "diag"); }
  void release(bool refresh) override { releases++; lastRefresh = refresh; }
 private:
  FakeLink& link_;
  FakeSpi&  spi_;
};

// One session: a RAM ring of ringBytes, a clip of `size` pattern bytes.
struct SessionRig {
  RamFlash flash;
  FrameRing ring;
  ChunkFeed feed;
  std::vector<uint8_t> data;
  ScriptedSession src;
  uint8_t asmBuf[SPI_FRAME], frame[SPI_FRAME];
  char diag[32];
  SessionRig(size_t ringBytes, size_t size, size_t pre = 0, uint32_t dryMs = DRY_MS)
      : flash(ringBytes), ring(flash), feed(ring, clk), data(pattern(size)), src(feed, data) {
    ring.begin();
    clk.ms = 0;
    feed.begin(size, asmBuf, pre, dryMs);
  }
  UploadResult run(FakeLease& lease, const PaceConfig& pace = PaceConfig()) {
    return fanlogic::runSession(lease, src, clk, "99-Test.bin", data.size(), SpiConfig(), pace, frame, scratch,
                                sizeof(scratch), diag, sizeof(diag));
  }
};

static uint32_t worstGap(const FakeSpi& s) {
  uint32_t worst = 0;
  for (size_t i = 1; i < s.times.size(); i++)
    if (s.times[i] - s.times[i - 1] > worst) worst = s.times[i] - s.times[i - 1];
  return worst;
}

// ---- 0.8.1: BURST pacing (window-burst), wmax / igmax ----

static PaceConfig burst(uint32_t igap = 0) {
  PaceConfig p;
  p.pace = Pace::BURST;
  p.igapMs = igap;
  return p;
}

// Slow client (30 KB/s, ~0.96 s per window): every window goes out back to
// back, and only once 20 frames are buffered.
void test_burst_windows_are_back_to_back_and_full() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300, 20, 30000);
  UploadResult r = rig.run(lease, burst());
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL(300, s.frames);
  TEST_ASSERT_EQUAL_MEMORY(rig.data.data(), s.bytes.data(), rig.data.size());
  uint32_t igmax = 0;
  for (size_t w = 0; w < 15; w++) {
    for (size_t i = 1; i < 20; i++) TEST_ASSERT_EQUAL_UINT32(s.times[w * 20], s.times[w * 20 + i]);
    if (w > 0 && s.times[w * 20] - s.times[w * 20 - 1] > igmax) igmax = s.times[w * 20] - s.times[w * 20 - 1];
    if (rig.src.arrivingAtPop[w * 20]) TEST_ASSERT_TRUE(rig.src.countAtPop[w * 20] >= 20);
  }
  TEST_ASSERT_TRUE(igmax > 500);                       // it did wait for the slow client
  TEST_ASSERT_EQUAL_UINT32(0, r.wmax);
  TEST_ASSERT_EQUAL_UINT32(igmax, r.igmax);
}

void test_burst_igap_is_honoured() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300, 62, 30000);
  rig.src.bytesPerTick = 3000;                         // fast: only igap limits
  UploadResult r = rig.run(lease, burst(3000));
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL(300, s.frames);
  int checked = 0;
  for (size_t w = 1; w < 15; w++) {
    if (!rig.src.arrivingAtPop[w * 20]) continue;      // once the body is over, everything goes at once
    TEST_ASSERT_TRUE(s.times[w * 20] - s.times[w * 20 - 1] >= 3000);
    checked++;
  }
  TEST_ASSERT_TRUE(checked >= 3);
  TEST_ASSERT_TRUE(r.igmax >= 3000);
  TEST_ASSERT_EQUAL_MEMORY(rig.data.data(), s.bytes.data(), rig.data.size());
}

// An outage longer than dryMs after the first pop: RECV 'd', the rest zeros.
void test_burst_outage_longer_than_dry_ends_d() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {"", "02-Name.bin\n"};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300, 62, 5000);
  rig.src.outFrom = 3000;
  rig.src.outTo = UINT32_MAX;
  UploadResult r = rig.run(lease, burst());
  TEST_ASSERT_EQUAL(ClipErr::RECV, r.err);
  TEST_ASSERT_EQUAL('d', r.bodyEnd);
  TEST_ASSERT_EQUAL(300, s.frames);
  TEST_ASSERT_EQUAL(1, l.count("u 99-Test.bin\n"));
}

// The same outage, shorter than dryMs: the session survives, byte-exact.
void test_burst_outage_shorter_than_dry_survives() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300, 62, 60000);
  rig.src.outFrom = 5000;
  rig.src.outTo = 45000;
  UploadResult r = rig.run(lease, burst());
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL_MEMORY(rig.data.data(), s.bytes.data(), rig.data.size());
}

// BURST never sends trickle frames: nothing goes out while < 20 buffered.
void test_burst_sends_no_trickle_frames() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300, 20, 60000);
  rig.src.outFrom = 1500;
  rig.src.outTo = 20000;
  rig.run(lease, burst());
  TEST_ASSERT_EQUAL(0, s.frames % 20);
  for (size_t i = 0; i + 1 < s.times.size(); i++)
    if (s.times[i] != s.times[i + 1]) TEST_ASSERT_EQUAL(0, (i + 1) % 20);   // time only moves between windows
}

// ---- 0.8.2: RESERVE pacing (the MCU needs whole windows <= ~3 s apart) ----

static PaceConfig reserve(uint32_t keep = 2500) {
  PaceConfig p;
  p.pace = Pace::RESERVE;
  p.keepMs = keep;
  return p;
}

void test_reserve_config_defaults() {
  PaceConfig p;
  TEST_ASSERT_EQUAL_UINT32(KEEP_DEFAULT_MS, p.keepMs);
  TEST_ASSERT_EQUAL(Pace::TRICKLE, p.pace);            // the PaceConfig default is unchanged
}

// Fast client (300 KB/s): the ring is full at every window boundary, so the
// windows go back to back (only the refill time between them), byte-exact.
void test_reserve_fast_client_windows_back_to_back() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300, 0, 4000);
  rig.src.bytesPerTick = 3000;
  UploadResult r = rig.run(lease, reserve());
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL(300, s.frames);
  TEST_ASSERT_EQUAL_MEMORY(rig.data.data(), s.bytes.data(), rig.data.size());
  TEST_ASSERT_TRUE(r.igmax < 1000);                    // no keepMs wait anywhere
  for (size_t w = 0; w < 15; w++)
    for (size_t i = 1; i < 20; i++) TEST_ASSERT_EQUAL_UINT32(s.times[w * 20], s.times[w * 20 + i]);
}

// Slow client (30 KB/s): a window leaves only when 20 frames are buffered
// AND (the ring is full OR keepMs since the previous window's last frame);
// never a partial window while the body arrives.
void test_reserve_slow_client_windows_every_keep_never_partial() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300, 20, 4000);
  UploadResult r = rig.run(lease, reserve());
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL(300, s.frames);
  TEST_ASSERT_EQUAL_MEMORY(rig.data.data(), s.bytes.data(), rig.data.size());
  int paced = 0;
  for (size_t w = 0; w < 15; w++) {
    for (size_t i = 1; i < 20; i++) TEST_ASSERT_EQUAL_UINT32(s.times[w * 20], s.times[w * 20 + i]);   // whole
    if (!rig.src.arrivingAtPop[w * 20]) continue;
    TEST_ASSERT_TRUE(rig.src.countAtPop[w * 20] >= 20);
    if (w == 0) continue;                              // the first window: as soon as 20 are buffered
    uint32_t gap = s.times[w * 20] - s.times[w * 20 - 1];
    TEST_ASSERT_TRUE(rig.src.fullAtPop[w * 20] || gap >= 2500);
    if (!rig.src.fullAtPop[w * 20]) { TEST_ASSERT_TRUE(gap <= 2500 + 20); paced++; }
  }
  TEST_ASSERT_TRUE(paced >= 3);                        // keepMs, not the client, set the pace
}

// A client slower than the MCU's average need (6 KB/s: a window takes 4.8 s
// to arrive, more than dry 4000; the MCU needs ~11.5 KB/s at keep 2500): the
// reserve drains, then the window is late > dry -> RECV 'd'. (At 8 KB/s a
// window takes 3.6 s < dry, so that session lives on at the MCU's edge.)
void test_reserve_slow_client_below_rate_ends_d_after_reserve_drains() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {"", "02-Name.bin\n"};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK * 5, SPI_FRAME * 3000, 0, 4000);
  rig.src.bytesPerTick = 60;                           // 6 KB/s
  UploadResult r = rig.run(lease, reserve());
  TEST_ASSERT_EQUAL(ClipErr::RECV, r.err);
  TEST_ASSERT_EQUAL('d', r.bodyEnd);
  TEST_ASSERT_EQUAL(3000, s.frames);                   // finished with zeros
  TEST_ASSERT_EQUAL(1, l.count("u 99-Test.bin\n"));
  TEST_ASSERT_TRUE(r.igmax >= 4000 && r.igmax <= 4050);   // the last wait ended at dry
  TEST_ASSERT_TRUE(rig.feed.stagingRate() > 5500 && rig.feed.stagingRate() < 6500);
}

// An outage inside the reserve (full ring ~ 31 windows x 2.5 s): survives.
void test_reserve_outage_inside_the_cover_survives() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK * 5, SPI_FRAME * 1200, 0, 4000);
  rig.src.bytesPerTick = 150;                          // 15 KB/s: prefill ~60 s, then the ring stays full
  rig.src.outFrom = 70000;
  rig.src.outTo = 110000;                              // 40 s < the ~78 s of cover
  UploadResult r = rig.run(lease, reserve());
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL_MEMORY(rig.data.data(), s.bytes.data(), rig.data.size());
  TEST_ASSERT_TRUE(r.igmax <= 2500 + 200);
}

// An outage beyond the cover: 'd' (not a hang).
void test_reserve_outage_beyond_the_cover_ends_d() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {"", "02-Name.bin\n"};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK * 5, SPI_FRAME * 1200, 0, 4000);
  rig.src.bytesPerTick = 150;
  rig.src.outFrom = 70000;
  rig.src.outTo = UINT32_MAX;
  UploadResult r = rig.run(lease, reserve());
  TEST_ASSERT_EQUAL(ClipErr::RECV, r.err);
  TEST_ASSERT_EQUAL('d', r.bodyEnd);
}

// Once the body is complete, the rest goes out at once (no keepMs waits).
void test_reserve_body_over_sends_everything_at_once() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK * 5, SPI_FRAME * 100, 0, 4000);   // fits the ring: complete before any send
  rig.src.bytesPerTick = 3000;
  UploadResult r = rig.run(lease, reserve());
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL_UINT32(s.times[0], s.times[s.times.size() - 1]);
}

// TRICKLE reports its own window / gap timing too.
void test_trickle_reports_wmax_igmax() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300);
  UploadResult r = rig.run(lease);
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_TRUE(r.wmax <= 20 * TRICKLE_MS);
  TEST_ASSERT_TRUE(r.wmax > 0 || r.igmax > 0);
}

void test_session_happy_path_byte_exact() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};                                  // CT, RT
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300);
  UploadResult r = rig.run(lease);
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL(300, s.frames);
  TEST_ASSERT_EQUAL_MEMORY(rig.data.data(), s.bytes.data(), rig.data.size());
  TEST_ASSERT_EQUAL(1, lease.releases);
  TEST_ASSERT_TRUE(lease.lastRefresh);
  TEST_ASSERT_EQUAL_STRING("diag", rig.diag);
}

// Review focus 3.
void test_session_tiny_clip() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK * 5, 100);
  UploadResult r = rig.run(lease);
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL(20, s.frames);                     // one window
  TEST_ASSERT_EQUAL_MEMORY(rig.data.data(), s.bytes.data(), 100);
}

void test_session_prefill_abort_never_touches_mcu() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300);
  rig.src.abortAt = 500;                               // prefill (64 frames) needs ~3 s
  UploadResult r = rig.run(lease);
  TEST_ASSERT_EQUAL(ClipErr::ABORT, r.err);
  TEST_ASSERT_FALSE(r.partial);
  TEST_ASSERT_EQUAL('a', r.bodyEnd);
  TEST_ASSERT_EQUAL(0, lease.leases);
  TEST_ASSERT_EQUAL(0, (int)l.lines.size());
  TEST_ASSERT_EQUAL(0, s.frames);
}

void test_session_lease_retries_then_succeeds() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  lease.failFirst = 3;
  SessionRig rig(RING_BLOCK, SPI_FRAME * 40);
  TEST_ASSERT_EQUAL(ClipErr::OK, rig.run(lease).err);
  TEST_ASSERT_EQUAL(4, lease.leases);
}

void test_session_lease_gives_up_busy() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  lease.failFirst = 1000;
  SessionRig rig(RING_BLOCK, SPI_FRAME * 40);
  UploadResult r = rig.run(lease);
  TEST_ASSERT_EQUAL(ClipErr::BUSY, r.err);
  TEST_ASSERT_FALSE(r.partial);
  TEST_ASSERT_EQUAL(0, (int)l.lines.size());
  TEST_ASSERT_EQUAL(0, lease.releases);
  TEST_ASSERT_TRUE(clk.ms >= fanlogic::LEASE_RETRY_MS);
}

void test_session_exists_releases_without_refresh() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {"99-Test.bin\n"};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 40);
  TEST_ASSERT_EQUAL(ClipErr::EXISTS, rig.run(lease).err);
  TEST_ASSERT_EQUAL(1, lease.releases);
  TEST_ASSERT_FALSE(lease.lastRefresh);
}

// A 40 s outage once frames are flowing: the trickle keeps the MCU fed.
void test_session_outage_keeps_trickle() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300);
  rig.src.outFrom = 5000;
  rig.src.outTo = 45000;
  UploadResult r = rig.run(lease);
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL_MEMORY(rig.data.data(), s.bytes.data(), rig.data.size());
  TEST_ASSERT_TRUE(worstGap(s) <= TRICKLE_MS + 20);
  TEST_ASSERT_TRUE(clk.ms >= 45000);
}

// The client vanishes: the ring drains at the trickle, then runs dry.
void test_session_dry_ends_in_zero_tail_and_cleanup() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {"", "02-Name.bin\n"};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300, 64);
  rig.src.outFrom = 5000;
  rig.src.outTo = UINT32_MAX;
  UploadResult r = rig.run(lease);
  TEST_ASSERT_EQUAL(ClipErr::RECV, r.err);
  TEST_ASSERT_EQUAL('d', r.bodyEnd);
  TEST_ASSERT_EQUAL(300, s.frames);                    // completed with zeros
  TEST_ASSERT_EQUAL(1, l.count("u 99-Test.bin\n"));
}

void test_session_abort_mid_send_cleans_up() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {"", "02-Name.bin\n"};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300);
  rig.src.abortAt = 8000;
  UploadResult r = rig.run(lease);
  TEST_ASSERT_EQUAL(ClipErr::ABORT, r.err);
  TEST_ASSERT_EQUAL('a', r.bodyEnd);
  TEST_ASSERT_EQUAL(300, s.frames);
  TEST_ASSERT_EQUAL(1, l.count("u 99-Test.bin\n"));
}

// Review focus 2: a lost reply makes the client resend 20,000 bytes.
void test_session_resend_overlap_is_byte_exact() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300);
  rig.src.rewindAt = 6000;
  rig.src.rewindBy = 20000;
  UploadResult r = rig.run(lease);
  TEST_ASSERT_EQUAL(ClipErr::OK, r.err);
  TEST_ASSERT_EQUAL_MEMORY(rig.data.data(), s.bytes.data(), rig.data.size());
  TEST_ASSERT_EQUAL(20000, rig.feed.dupBytes());
}

// Body abort during lease retry: no MCU contact, no release.
void test_session_abort_during_lease_retry() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  lease.failFirst = 1000;                              // lease always fails
  SessionRig rig(RING_BLOCK, SPI_FRAME * 40);
  // Prefill of 40 frames at 300 bytes/tick takes ~1.9 s; abort at 5 s (within retry window)
  rig.src.abortAt = 5000;
  UploadResult r = rig.run(lease);
  TEST_ASSERT_EQUAL(ClipErr::ABORT, r.err);
  TEST_ASSERT_EQUAL('a', r.bodyEnd);
  TEST_ASSERT_EQUAL(0, (int)l.lines.size());          // no MCU contact
  TEST_ASSERT_EQUAL(0, s.frames);                      // no SPI
  TEST_ASSERT_EQUAL(0, lease.releases);
}

// SPI initialization fails: release without refresh.
void test_session_spi_fails() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  lease.spiFails = true;
  SessionRig rig(RING_BLOCK, SPI_FRAME * 40);
  UploadResult r = rig.run(lease);
  TEST_ASSERT_EQUAL(ClipErr::SPI, r.err);
  TEST_ASSERT_EQUAL(0, (int)l.lines.size());          // uploadClip never ran
  TEST_ASSERT_EQUAL(0, s.frames);
  TEST_ASSERT_EQUAL(1, lease.releases);
  TEST_ASSERT_FALSE(lease.lastRefresh);
  TEST_ASSERT_EQUAL_STRING("diag", rig.diag);
}


// Bench 2026-10-07 (0.8.6): a concurrent producer's flash erase swallowed the
// MCU's GPIO18 ack. uploadClip brackets every window so the source can pause
// its flash writes: one begin/end pair per window, closed on failure too.
void test_window_gate_brackets_every_window() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300);
  TEST_ASSERT_EQUAL(ClipErr::OK, rig.run(lease).err);
  std::string want;
  for (int w = 0; w < 15; w++) want += "BE";
  TEST_ASSERT_EQUAL_STRING(want.c_str(), rig.src.gate.c_str());
}

void test_window_gate_closed_on_sync_failure() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {"", ""};
  l.expects = {0};
  s.failWindowAt = 1;                                  // the second window's GPIO18 wait fails
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300);
  TEST_ASSERT_EQUAL(ClipErr::SYNC, rig.run(lease).err);
  TEST_ASSERT_EQUAL_STRING("BEBE", rig.src.gate.c_str());
}

// Review 0.9.0: a wait inside a window holds the gate while the only
// producer (the chunk handler) is blocked on it. Reserve and burst start a
// window only with 20 frames buffered, so they never wait inside one.
static void expectNoWaitInsideWindows(const PaceConfig& pace, uint32_t dryMs) {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {""};
  l.expects = {0, 0};
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300, 0, dryMs);
  rig.src.gateBlocksClient = true;
  TEST_ASSERT_EQUAL(ClipErr::OK, rig.run(lease, pace).err);
  TEST_ASSERT_EQUAL(0, rig.src.waitsInWindow);
}

void test_window_gate_reserve_never_waits_inside_a_window() { expectNoWaitInsideWindows(reservePace(), 4000); }

void test_window_gate_burst_never_waits_inside_a_window() {
  PaceConfig burst;
  burst.pace = Pace::BURST;
  expectNoWaitInsideWindows(burst, 30000);
}

// Failures in the middle of a window (the post-loop windowEnd) close the gate
// too: an open gate would block the chunk handler forever.
void test_window_gate_closed_on_mid_window_spi_failure() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {"", ""};
  l.expects = {0};
  s.failFrameAt = 25;                                  // inside the second window
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300);
  TEST_ASSERT_EQUAL(ClipErr::SPI, rig.run(lease).err);
  TEST_ASSERT_EQUAL_STRING("BEBE", rig.src.gate.c_str());
}

void test_window_gate_closed_on_mid_window_credit_failure() {
  FakeLink l; FakeSpi s; FakeLease lease(l, s);
  l.lists = {"", ""};
  l.expects = {0};
  s.failCreditAt = 27;                                 // inside the second window
  SessionRig rig(RING_BLOCK, SPI_FRAME * 300);
  TEST_ASSERT_EQUAL(ClipErr::SYNC, rig.run(lease).err);
  TEST_ASSERT_EQUAL_STRING("BEBE", rig.src.gate.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_dark_upload_plays_the_black_clip_first);
  RUN_TEST(test_dark_upload_without_the_black_clip_dims);
  RUN_TEST(test_normal_upload_does_not_go_dark);
  RUN_TEST(test_window_gate_reserve_never_waits_inside_a_window);
  RUN_TEST(test_window_gate_burst_never_waits_inside_a_window);
  RUN_TEST(test_window_gate_closed_on_mid_window_spi_failure);
  RUN_TEST(test_window_gate_closed_on_mid_window_credit_failure);
  RUN_TEST(test_upload_checks_credit_before_every_frame);
  RUN_TEST(test_upload_credit_timeout_is_sync);
  RUN_TEST(test_upload_reports_crc_of_every_frame_sent);
  RUN_TEST(test_upload_crc_covers_frames_sent_on_failure);
  RUN_TEST(test_upload_name_rules);
  RUN_TEST(test_pad_size);
  RUN_TEST(test_next_clip_name_skips_junk);
  RUN_TEST(test_next_clip_name_no_cap);
  RUN_TEST(test_list_has_exact_line);
  RUN_TEST(test_json_entry);
  RUN_TEST(test_json_entry_escapes);
  RUN_TEST(test_json_entry_too_small);
  RUN_TEST(test_err_names);
  RUN_TEST(test_delete_ok);
  RUN_TEST(test_delete_not_found);
  RUN_TEST(test_delete_still_there);
  RUN_TEST(test_delete_empty_listing_after_is_not_ok);
  RUN_TEST(test_delete_empty_then_full_listing_is_ok);
  RUN_TEST(test_delete_cut_off_listing_is_listed_again);
  RUN_TEST(test_delete_only_clip_empty_listing_is_ok);
  RUN_TEST(test_delete_bad_name_sends_nothing);
  RUN_TEST(test_delete_underscore_name_allowed);
  RUN_TEST(test_upload_happy_path);
  RUN_TEST(test_upload_exact_window_no_extra_window);
  RUN_TEST(test_upload_exists);
  RUN_TEST(test_upload_bad_name_and_length);
  RUN_TEST(test_upload_create_refused);
  RUN_TEST(test_upload_create_timeout_cleans_up);
  RUN_TEST(test_upload_rf_deletes);
  RUN_TEST(test_upload_no_verdict);
  RUN_TEST(test_upload_sync_failure_stops_sending);
  RUN_TEST(test_upload_spi_failure);
  RUN_TEST(test_upload_body_fails_during_prefill_creates_nothing);
  RUN_TEST(test_upload_body_fails_midstream_pads_zeros);
  RUN_TEST(test_delete_waits_full_settle_time);
  RUN_TEST(test_upload_cleanup_waits_full_settle_time);
  RUN_TEST(test_decode_clip_path);
  RUN_TEST(test_upload_name_rejects_path_chars);
  RUN_TEST(test_upload_needs_only_two_frames_of_ram);
  RUN_TEST(test_upload_pauses_playback_before_create);
  RUN_TEST(test_upload_reports_frames_sent);
  RUN_TEST(test_upload_settles_after_pause_and_create);
  RUN_TEST(test_list_reads_a_late_tail);
  RUN_TEST(test_upload_name_accepts_underscore);
  RUN_TEST(test_delete_force_skips_list_check);
  RUN_TEST(test_format_needs_exact_token);
  RUN_TEST(test_format_sends_v_once_after_pause);
  RUN_TEST(test_format_waits_for_format_log);
  RUN_TEST(test_upload_polls_mcu_after_each_frame);
  RUN_TEST(test_upload_prefills_before_create);
  RUN_TEST(test_upload_trickles_through_a_long_stall);
  RUN_TEST(test_upload_body_stall_too_long_fails);
  RUN_TEST(test_burst_windows_are_back_to_back_and_full);
  RUN_TEST(test_burst_igap_is_honoured);
  RUN_TEST(test_burst_outage_longer_than_dry_ends_d);
  RUN_TEST(test_burst_outage_shorter_than_dry_survives);
  RUN_TEST(test_burst_sends_no_trickle_frames);
  RUN_TEST(test_reserve_config_defaults);
  RUN_TEST(test_reserve_fast_client_windows_back_to_back);
  RUN_TEST(test_reserve_slow_client_windows_every_keep_never_partial);
  RUN_TEST(test_reserve_slow_client_below_rate_ends_d_after_reserve_drains);
  RUN_TEST(test_reserve_outage_inside_the_cover_survives);
  RUN_TEST(test_reserve_outage_beyond_the_cover_ends_d);
  RUN_TEST(test_reserve_body_over_sends_everything_at_once);
  RUN_TEST(test_trickle_reports_wmax_igmax);
  RUN_TEST(test_session_happy_path_byte_exact);
  RUN_TEST(test_session_tiny_clip);
  RUN_TEST(test_session_prefill_abort_never_touches_mcu);
  RUN_TEST(test_session_lease_retries_then_succeeds);
  RUN_TEST(test_session_lease_gives_up_busy);
  RUN_TEST(test_session_exists_releases_without_refresh);
  RUN_TEST(test_session_outage_keeps_trickle);
  RUN_TEST(test_session_dry_ends_in_zero_tail_and_cleanup);
  RUN_TEST(test_session_abort_mid_send_cleans_up);
  RUN_TEST(test_session_resend_overlap_is_byte_exact);
  RUN_TEST(test_session_abort_during_lease_retry);
  RUN_TEST(test_session_spi_fails);
  RUN_TEST(test_window_gate_brackets_every_window);
  RUN_TEST(test_window_gate_closed_on_sync_failure);
  return UNITY_END();
}
