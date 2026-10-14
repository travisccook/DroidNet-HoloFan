#include "NodeConfig.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace nodecfg {

static void copyStr(char* dst, size_t dstSize, const char* src) {
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

void setDefaults(NodeConfig& c, uint8_t id, const char* alias) {
  c = NodeConfig();
  c.id = id;
  copyStr(c.alias, sizeof(c.alias), alias);
}

bool validId(int id) { return id >= 1 && id <= 19; }
bool validChannel(int ch) { return ch >= 1 && ch <= 11; }

bool validAlias(const char* s) {
  size_t n = strlen(s);
  if (n < 1 || n > 24 || isdigit((unsigned char)s[0])) return false;
  for (size_t i = 0; i < n; i++) {
    char ch = s[i];
    if (!(isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.')) return false;
  }
  return true;
}

static bool printable(const char* s) {
  for (; *s; s++) if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
  return true;
}

bool validSsid(const char* s) { size_t n = strlen(s); return n >= 1 && n <= 32 && printable(s); }
bool validPass(const char* s) { size_t n = strlen(s); return (n == 0 || (n >= 8 && n <= 63)) && printable(s); }
bool validPassword(const char* s) {
  size_t n = strlen(s);
  if (n < 1 || n > 39 || !printable(s)) return false;
  return strpbrk(s, "^,;") == nullptr;
}

void hostnameFromAlias(const char* alias, char* out, size_t outSize) {
  size_t o = 0;
  for (const char* p = alias; *p && o + 1 < outSize; p++) {
    char ch = (char)tolower((unsigned char)*p);
    out[o++] = (ch == '_' || ch == '.') ? '-' : ch;
  }
  out[o] = '\0';
}

static int hexVal(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool hexDecode(const char* hex, char* out, size_t outSize) {
  size_t n = strlen(hex);
  if (n % 2 != 0 || n / 2 + 1 > outSize) return false;
  for (size_t i = 0; i < n / 2; i++) {
    int hi = hexVal(hex[2 * i]), lo = hexVal(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    char ch = (char)(hi * 16 + lo);
    if (ch == '\0') return false;
    out[i] = ch;
  }
  out[n / 2] = '\0';
  return true;
}

static bool parseUInt(const char* s, int lo, int hi, int& out) {
  if (!*s || strlen(s) > 4) return false;
  for (const char* p = s; *p; p++) if (!isdigit((unsigned char)*p)) return false;
  int v = atoi(s);
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}

bool parseZ(const char* line, ZCmd& out) {
  if (strncmp(line, "#HLZ", 4) != 0) return false;
  const char* r = line + 4;
  out = ZCmd();
  switch (r[0]) {
    case 'P': out.type = ZType::REPLY; return parseUInt(r + 1, 0, 1, out.num);
    case 'I': out.type = ZType::ID;    return parseUInt(r + 1, 1, 19, out.num);
    case 'X': out.type = ZType::MAINT; return parseUInt(r + 1, 1, 2, out.num);
    case 'Q': out.type = ZType::QUERY; return r[1] == '\0';
    case 'N':
      if (!validAlias(r + 1)) return false;
      out.type = ZType::NAME;
      copyStr(out.text, sizeof(out.text), r + 1);
      return true;
    case 'K': {
      if ((r[1] != 'S' && r[1] != 'P') || (r[2] != '1' && r[2] != '2')) return false;
      out.num = r[2] - '0';
      if (!hexDecode(r + 3, out.text, sizeof(out.text))) return false;
      if (r[1] == 'S') { out.type = ZType::SSID; return validSsid(out.text); }
      out.type = ZType::PASS;
      return validPass(out.text);
    }
    default: return false;
  }
}

ZResult applyZ(const ZCmd& z, NodeConfig& c) {
  switch (z.type) {
    case ZType::REPLY: c.replies = z.num != 0; return ZResult::SAVED;
    case ZType::ID:   c.id = (uint8_t)z.num; return ZResult::SAVED_REBOOT;
    case ZType::NAME: copyStr(c.alias, sizeof(c.alias), z.text); return ZResult::SAVED_REBOOT;
    case ZType::SSID: copyStr(c.profile[z.num - 1].ssid, sizeof(c.profile[0].ssid), z.text); return ZResult::SAVED;
    case ZType::PASS: copyStr(c.profile[z.num - 1].pass, sizeof(c.profile[0].pass), z.text); return ZResult::SAVED;
    case ZType::MAINT: return ZResult::MAINT;
    case ZType::QUERY:
    case ZType::NONE: break;
  }
  return ZResult::QUERY;
}

int formatNodeReply(const NodeConfig& c, const NodeRuntime& rt, char* out, size_t outSize) {
  int n = snprintf(out, outSize,
                   "#HL:NODE id=%u name=%s reply=%s ch=%u q=%u v2=%d up=%d rx=%lu rej=%lu crc=%lu dup=%lu rt=%lu gu=%lu",
                   (unsigned)c.id, c.alias, c.replies ? "on" : "off", (unsigned)c.channel, (unsigned)c.floorQ,
                   rt.v2 ? 1 : 0, rt.meshUp ? 1 : 0,
                   (unsigned long)rt.rx, (unsigned long)rt.rej, (unsigned long)rt.crc, (unsigned long)rt.dup,
                   (unsigned long)rt.retry, (unsigned long)rt.gaveUp);
  if (n < 0 || (size_t)n >= outSize) return -1;
  return n;
}

// ---- console ----------------------------------------------------------------

// Split off the next space-delimited word of `p` into `word`; returns the rest
// (leading spaces skipped).
static const char* nextWord(const char* p, char* word, size_t wordSize) {
  while (*p == ' ') p++;
  size_t o = 0;
  while (*p && *p != ' ') { if (o + 1 < wordSize) word[o++] = *p; p++; }
  word[o] = '\0';
  while (*p == ' ') p++;
  return p;
}

static bool usage(char* err, size_t errSize, const char* msg) {
  snprintf(err, errSize, "%s", msg);
  return false;
}

static bool parseHexByte(const char* s, int& out) {
  if (strlen(s) != 2) return false;
  int hi = hexVal(s[0]), lo = hexVal(s[1]);
  if (hi < 0 || lo < 0) return false;
  out = hi * 16 + lo;
  return true;
}

bool parseUsb(const char* line, UsbCmd& out, char* err, size_t errSize) {
  out = UsbCmd();
  char word[16];
  const char* rest = nextWord(line, word, sizeof(word));
  if (strcmp(word, "mesh") != 0) return usage(err, errSize, "not a mesh command");
  rest = nextWord(rest, word, sizeof(word));
  if (word[0] == '\0' || strcmp(word, "show") == 0) { out.type = UsbType::SHOW; return true; }
  if (strcmp(word, "restart") == 0) { out.type = UsbType::RESTART; return true; }
  if (strcmp(word, "off") == 0) {
    if (*rest) return usage(err, errSize, "mesh off");
    out.type = UsbType::MESH_OFF; return true;
  }
  if (strcmp(word, "clear") == 0) {
    char arg[16]; const char* r2 = nextWord(rest, arg, sizeof(arg));
    if (*r2 || !parseUInt(arg, 1, 2, out.a)) return usage(err, errSize, "mesh clear <1|2>");
    out.type = UsbType::CLEAR; return true;
  }
  if (strcmp(word, "pw") == 0) {
    if (!validPassword(rest)) return usage(err, errSize, "mesh pw <1-39 chars, no ^ , ;>");
    out.type = UsbType::PASSWORD; copyStr(out.text, sizeof(out.text), rest); return true;
  }
  if (strcmp(word, "name") == 0) {
    if (!validAlias(rest)) return usage(err, errSize, "mesh name <1-24 of A-Z a-z 0-9 _ . -, not starting with a digit>");
    out.type = UsbType::NAME; copyStr(out.text, sizeof(out.text), rest); return true;
  }
  char a[16], b[16];
  if (strcmp(word, "oct") == 0) {
    rest = nextWord(rest, a, sizeof(a));
    rest = nextWord(rest, b, sizeof(b));
    if (*rest || !parseHexByte(a, out.a) || !parseHexByte(b, out.b)) return usage(err, errSize, "mesh oct <hh> <hh>");
    out.type = UsbType::OCTETS; return true;
  }
  if (strcmp(word, "crc") == 0) {
    if (strcmp(rest, "on") == 0) out.a = 1;
    else if (strcmp(rest, "off") == 0) out.a = 0;
    else return usage(err, errSize, "mesh crc on|off");
    out.type = UsbType::CHECKSUM; return true;
  }
  if (strcmp(word, "reply") == 0) {
    if (strcmp(rest, "on") == 0) out.a = 1;
    else if (strcmp(rest, "off") == 0) out.a = 0;
    else return usage(err, errSize, "mesh reply on|off");
    out.type = UsbType::REPLY; return true;
  }
  struct Num { const char* w; UsbType t; int lo, hi; const char* use; };
  static const Num nums[] = {
    {"ch", UsbType::CHANNEL, 1, 11, "mesh ch <1-11>"},
    {"q", UsbType::FLOOR, 0, 20, "mesh q <0-20>"},
    {"id", UsbType::ID, 1, 19, "mesh id <1-19>"},
    {"maint", UsbType::MAINT, 1, 2, "mesh maint <1|2>"},
  };
  for (const Num& n : nums) {
    if (strcmp(word, n.w) != 0) continue;
    if (!parseUInt(rest, n.lo, n.hi, out.a)) return usage(err, errSize, n.use);
    out.type = n.t; return true;
  }
  if (strcmp(word, "ssid") == 0 || strcmp(word, "pass") == 0) {
    bool ssid = word[1] == 's';
    rest = nextWord(rest, a, sizeof(a));
    if (!parseUInt(a, 1, 2, out.a)) return usage(err, errSize, ssid ? "mesh ssid <1|2> <name>" : "mesh pass <1|2> [passphrase]");
    if (ssid ? !validSsid(rest) : !validPass(rest))
      return usage(err, errSize, ssid ? "SSID must be 1-32 printable chars" : "passphrase must be empty or 8-63 printable chars");
    out.type = ssid ? UsbType::SSID : UsbType::PASS;
    copyStr(out.text, sizeof(out.text), rest);
    return true;
  }
  return usage(err, errSize, "mesh [show|pw|oct|ch|q|crc|id|name|reply|ssid|pass|clear|maint|off|restart]");
}

bool applyUsb(const UsbCmd& u, NodeConfig& c) {
  switch (u.type) {
    case UsbType::PASSWORD: copyStr(c.password, sizeof(c.password), u.text); return true;
    case UsbType::OCTETS:   c.oct2 = (uint8_t)u.a; c.oct3 = (uint8_t)u.b; return true;
    case UsbType::CHANNEL:  c.channel = (uint8_t)u.a; return true;
    case UsbType::FLOOR:    c.floorQ = (uint8_t)u.a; return true;
    case UsbType::CHECKSUM: c.checksum = u.a != 0; return true;
    case UsbType::ID:       c.id = (uint8_t)u.a; return true;
    case UsbType::NAME:     copyStr(c.alias, sizeof(c.alias), u.text); return true;
    case UsbType::REPLY:    c.replies = u.a != 0; return true;
    case UsbType::SSID:     copyStr(c.profile[u.a - 1].ssid, sizeof(c.profile[0].ssid), u.text); return true;
    case UsbType::MESH_OFF: c.password[0] = '\0'; return true;
    case UsbType::CLEAR:    c.profile[u.a - 1].ssid[0] = '\0'; c.profile[u.a - 1].pass[0] = '\0'; return true;
    case UsbType::PASS:     copyStr(c.profile[u.a - 1].pass, sizeof(c.profile[0].pass), u.text); return true;
    default: return false;
  }
}

int formatShow(const NodeConfig& c, const NodeRuntime& rt, char* out, size_t outSize) {
  int n = snprintf(out, outSize,
                   "mesh: %s  id=%u name=%s reply=%s\n"
                   "  ch=%u q=%u oct=%02X:%02X crc=%s pw=%s v2=%d\n"
                   "  wifi1=%s%s  wifi2=%s%s\n"
                   "  fw=%s slot=%s rx=%lu rej=%lu crc=%lu dup=%lu retry=%lu gaveup=%lu\n"
                   "  (id, name, ch, q, oct, pw, crc apply after 'mesh restart')",
                   rt.meshUp ? "UP" : (c.password[0] ? "DOWN" : "OFF (no password)"),
                   (unsigned)c.id, c.alias, c.replies ? "on" : "off",
                   (unsigned)c.channel, (unsigned)c.floorQ, (unsigned)c.oct2, (unsigned)c.oct3,
                   c.checksum ? "on" : "off", c.password[0] ? "***" : "(none)", rt.v2 ? 1 : 0,
                   c.profile[0].ssid[0] ? c.profile[0].ssid : "-", c.profile[0].pass[0] ? " (pass set)" : "",
                   c.profile[1].ssid[0] ? c.profile[1].ssid : "-", c.profile[1].pass[0] ? " (pass set)" : "",
                   rt.fw, rt.slot, (unsigned long)rt.rx, (unsigned long)rt.rej, (unsigned long)rt.crc,
                   (unsigned long)rt.dup, (unsigned long)rt.retry, (unsigned long)rt.gaveUp);
  if (n < 0 || (size_t)n >= outSize) return -1;
  return n;
}

}  // namespace nodecfg
