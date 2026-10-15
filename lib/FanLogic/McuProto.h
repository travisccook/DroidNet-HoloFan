// The text protocol between the fan's ESP32-C2 and its LED/SD MCU over UART1
// (docs/HOW_IT_WORKS.md, confirmed on hardware): one "<cmd>[ <arg>...]\n" line out, raw text back.
// Pure C++: no ESP-IDF headers, so the native env tests it.
#ifndef FAN_MCU_PROTO_H
#define FAN_MCU_PROTO_H
#include <stddef.h>
#include <stdint.h>
#include "HolofanStatus.h"

namespace fanlogic {

// Longest line we ever build ("t <63-char name>\n" is 66).
constexpr size_t MCU_LINE_MAX = 96;

// Build "<cmd>[ <arg>...]\n" into out. Returns the length, or -1 if refused:
//   - cmd empty, not starting with a lower-case letter, or not only
//     lower-case letters, digits and '_' (md5);
//   - cmd starts with 'v' (the MCU's SD-card FORMAT command: never sent);
//   - an arg is empty or has a byte outside 0x21..0x7E (NUL, CR, LF, space,
//     control or non-ASCII would break the framing);
//   - it does not fit out (or MCU_LINE_MAX).
int buildLine(const char* cmd, const char* const* args, int argc, char* out, size_t outSize);

// The MCU's volumes are 1 to 3 ("m <v>"; there is no 0). It powers up at 3.
constexpr int VOLUME_MAX = 3;

// Raw MCU lines that may be sent from the mesh debug command (#HLFX): only
// commands that cannot change or erase the SD card (never v, u, w, y).
bool rawMeshAllowed(const char* cmd);

// A whole #HLFX line ("<cmd> [args]", leading spaces skipped): cmd passes
// rawMeshAllowed, and the volume command only as "m <1..VOLUME_MAX>".
bool rawMeshLineAllowed(const char* line);

// A raw line ("<cmd> [args]") that may change the MCU's settings or clip:
// everything except the queries get_setting, get_playinfo, s and md5
// (0.9.8: only these keep FanController's UNPROMPTED baseline).
bool rawLineMayChange(const char* line);

// Unsolicited MCU output worth showing (0.9.2): complete "mculog:" lines,
// assembled as bytes arrive (CR dropped, LF ends a line, bytes outside
// 0x20..0x7E and '^' shown as '.', lines cut at MAX_CHARS). Other lines
// (replies, file lists, binary) are dropped. Up to QUEUE lines wait; more
// are counted in dropped(). Not thread-safe: feed and take under one lock.
class McuLogLines {
 public:
  static constexpr size_t MAX_CHARS = 64;
  static constexpr int QUEUE = 4;
  void feed(const uint8_t* b, size_t n);
  bool take(char* out, size_t size);     // the oldest waiting line
  uint32_t dropped() const { return dropped_; }

 private:
  char cur_[MAX_CHARS + 1] = {0};
  size_t len_ = 0;
  char q_[QUEUE][MAX_CHARS + 1] = {{0}};
  int head_ = 0, count_ = 0;
  uint32_t dropped_ = 0;
};

// #HLFT<NN> (0.9.3): the first clip name in the MCU's "s" listing that starts
// with NN- or NN_ (exactly two digits) and isClipName, fed in pieces as the
// listing arrives (lines end at LF or CR; finish() takes a last line
// without one). No buffer for the whole listing is needed.
class SlotFinder {
 public:
  explicit SlotFinder(int slot) : slot_(slot) {}
  // 0.9.8: find exactly this name instead (the restore after a clip lease).
  explicit SlotFinder(const char* exact) : slot_(-1), exact_(exact) {}
  void feed(const char* b, size_t n);
  void finish();
  bool found() const { return found_; }
  const char* name() const { return name_; }
  static void feedThunk(void* ctx, const char* b, size_t n) { static_cast<SlotFinder*>(ctx)->feed(b, n); }

 private:
  void endLine();
  int slot_;
  const char* exact_ = nullptr;
  char cur_[holofan::CLIP_NAME_MAX + 1] = {0};
  size_t len_ = 0;
  bool over_ = false;                  // the line is longer than a clip name may be
  char name_[holofan::CLIP_NAME_MAX + 1] = {0};
  bool found_ = false;
};

// A clip name we can put on the wire and keep in the catalog: 5..63 bytes of
// 0x21..0x7E except '^' (the mesh reply delimiter), ending in ".bin".
bool isClipName(const char* s);

// "setting <ble> <loop> <brightness> <volume> <angle> <model>" (e.g.
// "setting on all 2 3 164 F-S12"), anywhere in buf.
struct Settings {
  bool ble = false;
  bool loopAll = true;
  int  brightness = 0, volume = 0, angle = 0;
  char model[16] = {0};
};
bool parseSetting(const char* buf, Settings& out);

// "PlayInfo <name> <state> <progress> <total>" (e.g. PlayInfo "02-Name.bin"
// playing 8 24), anywhere in buf. The name may be quoted (then it runs to the
// closing quote and may hold spaces, or be empty when idle); state
// "playing" means playing, anything else means idle.
struct PlayInfo {
  char name[holofan::CLIP_NAME_MAX + 1] = {0};
  bool playing = false;
  int  progress = 0, total = 0;
};
bool parsePlayInfo(const char* buf, PlayInfo& out);

// The "s" (list) reply: one name per line, CR/LF tolerated. Keeps lines that
// pass isClipName() (so "mculog:" lines and headers drop out), in order, up
// to CATALOG_MAX. Returns the count kept.
int parseList(const char* buf, holofan::Catalog& out);

// Iterate the "s" reply: advance p past the next line that passes
// isClipName() and copy it into out (size >= holofan::CLIP_NAME_MAX + 1).
// False when no such line is left. No count cap (stock allows 100 clips).
bool nextClipName(const char*& p, char* out, size_t size);

// Append n bytes of s to the bench trace in buf (`size` bytes, `len` in use,
// kept NUL-terminated). When they do not fit, the oldest bytes go (at least
// half), so the latest stay; an append longer than size / 2 keeps only its
// last size / 2 bytes. Returns the new length (always < size; 0 if size is 0).
size_t traceAppend(char* buf, size_t size, size_t len, const char* s, size_t n);

}  // namespace fanlogic
#endif
