#include "McuProto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fanlogic {

static bool safeArg(const char* s) {
  if (!s || !*s) return false;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x21 || c > 0x7E) return false;
  }
  return true;
}

int buildLine(const char* cmd, const char* const* args, int argc, char* out, size_t outSize) {
  if (!cmd || !*cmd || cmd[0] == 'v' || argc < 0) return -1;
  if (cmd[0] < 'a' || cmd[0] > 'z') return -1;
  for (const char* p = cmd; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return -1;
  size_t limit = outSize < MCU_LINE_MAX ? outSize : MCU_LINE_MAX;
  size_t n = strlen(cmd);
  if (n + 2 > limit) return -1;
  memcpy(out, cmd, n);
  for (int i = 0; i < argc; i++) {
    if (!safeArg(args[i])) return -1;
    size_t m = strlen(args[i]);
    if (n + 1 + m + 2 > limit) return -1;
    out[n++] = ' ';
    memcpy(out + n, args[i], m);
    n += m;
  }
  out[n++] = '\n';
  out[n] = '\0';
  return (int)n;
}

bool rawMeshAllowed(const char* cmd) {
  static const char* const OK[] = {"get_setting", "get_playinfo", "s", "t", "md5", "a", "b", "c", "d",
                                   "e", "f", "g", "h", "i", "j", "k", "m", "o"};
  if (!cmd) return false;
  for (const char* ok : OK)
    if (strcmp(cmd, ok) == 0) return true;
  return false;
}

bool rawMeshLineAllowed(const char* line) {
  if (!line) return false;
  while (*line == ' ') line++;
  char cmd[16];
  size_t n = 0;
  while (line[n] && line[n] != ' ' && n < sizeof(cmd) - 1) { cmd[n] = line[n]; n++; }
  if (line[n] && line[n] != ' ') return false;           // longer than any allowed command
  cmd[n] = '\0';
  if (!rawMeshAllowed(cmd)) return false;
  if (strcmp(cmd, "m") != 0) return true;
  const char* a = line + n;
  while (*a == ' ') a++;
  if (*a < '1' || *a > '0' + VOLUME_MAX) return false;   // one digit, 1..VOLUME_MAX
  for (a++; *a == ' '; a++) {}
  return *a == '\0';
}

bool rawLineMayChange(const char* line) {
  if (!line) return true;
  while (*line == ' ') line++;
  size_t n = 0;
  while (line[n] && line[n] != ' ') n++;
  static const char* const QUERIES[] = {"get_setting", "get_playinfo", "s", "md5"};
  for (const char* q : QUERIES)
    if (strlen(q) == n && strncmp(line, q, n) == 0) return false;
  return true;
}

void McuLogLines::feed(const uint8_t* b, size_t n) {
  static const char PREFIX[] = "mculog:";
  const size_t plen = sizeof(PREFIX) - 1;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = b[i];
    if (c == '\r') continue;
    if (c == '\n') {
      cur_[len_] = '\0';
      if (len_ >= plen && memcmp(cur_, PREFIX, plen) == 0) {
        if (count_ == QUEUE) dropped_++;
        else { memcpy(q_[(head_ + count_) % QUEUE], cur_, len_ + 1); count_++; }
      }
      len_ = 0;
      continue;
    }
    if (len_ < MAX_CHARS) cur_[len_++] = (c < 0x20 || c > 0x7E || c == '^') ? '.' : (char)c;
  }
}

bool McuLogLines::take(char* out, size_t size) {
  if (count_ == 0 || size == 0) return false;
  strncpy(out, q_[head_], size - 1);
  out[size - 1] = '\0';
  head_ = (head_ + 1) % QUEUE;
  count_--;
  return true;
}

void SlotFinder::feed(const char* b, size_t n) {
  for (size_t i = 0; i < n && !found_; i++) {
    char c = b[i];
    if (c == '\n' || c == '\r') { endLine(); continue; }
    if (len_ < holofan::CLIP_NAME_MAX) cur_[len_++] = c;
    else over_ = true;
  }
}

void SlotFinder::finish() { endLine(); }

void SlotFinder::endLine() {
  cur_[len_] = '\0';
  if (!found_ && !over_ && exact_ && strcmp(cur_, exact_) == 0) {
    memcpy(name_, cur_, len_ + 1);
    found_ = true;
  }
  if (!found_ && !over_ && len_ >= 3 && slot_ >= 0 && slot_ <= 99 && cur_[0] == '0' + slot_ / 10 &&
      cur_[1] == '0' + slot_ % 10 && (cur_[2] == '-' || cur_[2] == '_') && isClipName(cur_)) {
    memcpy(name_, cur_, len_ + 1);
    found_ = true;
  }
  len_ = 0;
  over_ = false;
}

bool isClipName(const char* s) {
  if (!safeArg(s)) return false;
  size_t n = strlen(s);
  if (strchr(s, '^')) return false;       // '^' delimits mesh replies
  return n >= 5 && n <= holofan::CLIP_NAME_MAX && strcmp(s + n - 4, ".bin") == 0;
}

// Copy the next whitespace-delimited token of *p into out (truncating to
// size-1). Returns false if there is none on this line.
static bool token(const char*& p, char* out, size_t size) {
  while (*p == ' ' || *p == '\t') p++;
  if (!*p || *p == '\r' || *p == '\n') return false;
  size_t n = 0;
  while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
    if (n + 1 < size) out[n++] = *p;
    p++;
  }
  out[n] = '\0';
  return true;
}

static bool number(const char*& p, int& out) {
  char t[12];
  if (!token(p, t, sizeof(t))) return false;
  char* end = nullptr;
  long v = strtol(t, &end, 10);
  if (end == t || *end) return false;
  out = (int)v;
  return true;
}

bool parseSetting(const char* buf, Settings& out) {
  const char* p = buf ? strstr(buf, "setting ") : nullptr;
  if (!p) return false;
  p += 8;
  char ble[8], loop[8];
  Settings s;
  if (!token(p, ble, sizeof(ble)) || !token(p, loop, sizeof(loop))) return false;
  if (!number(p, s.brightness) || !number(p, s.volume) || !number(p, s.angle)) return false;
  if (!token(p, s.model, sizeof(s.model))) return false;
  s.ble = strcmp(ble, "on") == 0;
  s.loopAll = strcmp(loop, "one") != 0;    // stock: loop == "one" -> loop one, else all
  out = s;
  return true;
}

bool parsePlayInfo(const char* buf, PlayInfo& out) {
  const char* p = buf ? strstr(buf, "PlayInfo ") : nullptr;
  if (!p) return false;
  p += 9;
  PlayInfo pi;
  char state[16];
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '"') {                           // quoted: runs to the closing quote, may hold spaces
    p++;
    size_t n = 0;
    while (*p && *p != '"' && *p != '\r' && *p != '\n') {
      if (n + 1 < sizeof(pi.name)) pi.name[n++] = *p;
      p++;
    }
    if (*p != '"') return false;
    pi.name[n] = '\0';
    p++;
  } else if (!token(p, pi.name, sizeof(pi.name))) {
    return false;
  }
  if (!token(p, state, sizeof(state))) return false;
  if (!number(p, pi.progress) || !number(p, pi.total)) return false;
  pi.playing = strcmp(state, "playing") == 0;
  out = pi;
  return true;
}

bool nextClipName(const char*& p, char* out, size_t size) {
  if (!p || size == 0) return false;
  while (*p) {
    const char* e = p;
    while (*e && *e != '\n' && *e != '\r') e++;
    size_t n = (size_t)(e - p);
    const char* line = p;
    p = *e ? e + 1 : e;
    if (n > 0 && n <= holofan::CLIP_NAME_MAX && n < size) {
      memcpy(out, line, n);
      out[n] = '\0';
      if (isClipName(out)) return true;
    }
  }
  return false;
}

int parseList(const char* buf, holofan::Catalog& out) {
  out.count = 0;
  const char* p = buf;
  char name[holofan::CLIP_NAME_MAX + 1];
  while (out.count < holofan::CATALOG_MAX && nextClipName(p, name, sizeof(name)))
    strcpy(out.entries[out.count++].name, name);
  return out.count;
}

size_t traceAppend(char* buf, size_t size, size_t len, const char* s, size_t n) {
  if (size == 0) return 0;
  if (n > size / 2) { s += n - size / 2; n = size / 2; }
  if (len + n > size - 1) {               // full: drop the oldest half, keep the latest
    size_t drop = len / 2;
    // Half is not always enough (3071 used + 1536 in a 3072-byte buffer
    // left no room for the NUL): drop what the append needs.
    if (len - drop + n > size - 1) drop = len + n - (size - 1);
    memmove(buf, buf + drop, len - drop);
    len -= drop;
  }
  memcpy(buf + len, s, n);
  len += n;
  buf[len] = '\0';
  return len;
}

}  // namespace fanlogic
