#include "command.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

namespace holofan {

bool parseCommand(const char* line, Cmd& out) {
  if (!line || strncmp(line, "#HLF", 4) != 0) return false;
  const char* p = line + 4;
  if (*p == '\0') return false;
  char a = *p;
  const char* rest = p + 1;
  switch (a) {
    case '1': out = {CmdType::POWER_ON, 0}; return true;
    case '0': out = {CmdType::POWER_OFF, 0}; return true;
    case 'P': out = {CmdType::PAUSE, 0}; return true;
    case 'N': out = {CmdType::NEXT, 0}; return true;
    case 'R': out = {CmdType::PREV, 0}; return true;
    case 'M': out = {CmdType::MUTE, 0}; return true;
    case 'U': out = {CmdType::UNMUTE, 0}; return true;
    case 'C': out = {CmdType::CATALOG_REFRESH, 0}; return true;
    case 'D': out = {CmdType::CATALOG_DUMP, 0}; return true;
    case 'S': out = {CmdType::STATUS, 0}; return true;
    case 'Q': out = {CmdType::DIAG, 0}; return true;
    case 'G': { int n = atoi(rest); if (n < 1) return false; out = {CmdType::PLAY_INDEX, n}; return true; }
    case 'T': {                                       // #HLFT<NN>: one or two digits, nothing after
      if (rest[0] < '0' || rest[0] > '9') return false;
      int n = rest[0] - '0';
      const char* q = rest + 1;
      if (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
      if (*q != '\0') return false;
      out = {CmdType::PLAY_SLOT, n};
      return true;
    }
    case 'Y': {                                       // #HLFY<r><seconds>: r 0|1|2, 10-300 s, nothing after
      if (rest[0] < '0' || rest[0] > '2') return false;
      int s = 0, digits = 0;
      const char* q = rest + 1;
      for (; *q >= '0' && *q <= '9' && digits < 4; q++, digits++) s = s * 10 + (*q - '0');
      if (digits == 0 || *q != '\0' || s < 10 || s > 300) return false;
      out = {CmdType::LOUD_TEST, (rest[0] - '0') * 1000 + s};
      return true;
    }
    case 'E': {                                       // #HLFE<lo><hi><seconds>: levels 3-7, lo <= hi, 10-300 s
      if (rest[0] < '3' || rest[0] > '7' || rest[1] < '3' || rest[1] > '7' || rest[0] > rest[1]) return false;
      int s = 0, digits = 0;
      const char* q = rest + 2;
      for (; *q >= '0' && *q <= '9' && digits < 4; q++, digits++) s = s * 10 + (*q - '0');
      if (digits < 2 || *q != '\0' || s < 10 || s > 300) return false;
      out = {CmdType::SAG_METER, (rest[0] - '0') * 10000 + (rest[1] - '0') * 1000 + s};
      return true;
    }
    case 'J': {                                       // #HLFJ<lo><hi><dBm>: levels 3-7, lo <= hi, 02-20 dBm
      if (rest[0] < '3' || rest[0] > '7' || rest[1] < '3' || rest[1] > '7' || rest[0] > rest[1]) return false;
      if (rest[2] < '0' || rest[2] > '9' || rest[3] < '0' || rest[3] > '9' || rest[4] != '\0') return false;
      int dbm = (rest[2] - '0') * 10 + (rest[3] - '0');
      if (dbm < 2 || dbm > 20) return false;
      out = {CmdType::TX_PROBE, (rest[0] - '0') * 10000 + (rest[1] - '0') * 1000 + dbm};
      return true;
    }
    case 'A':                                         // #HLFA<0|1>: Bluetooth off / on
      if ((rest[0] != '0' && rest[0] != '1') || rest[1] != '\0') return false;
      out = {CmdType::BLUETOOTH, rest[0] - '0'};
      return true;
    case 'K': {                                       // #HLFK<ss>: two digits, 00 or 02-50
      if (rest[0] < '0' || rest[0] > '9' || rest[1] < '0' || rest[1] > '9' || rest[2] != '\0') return false;
      int n = (rest[0] - '0') * 10 + (rest[1] - '0');
      if (n == 1 || n > 25) return false;
      out = {CmdType::HEARTBEAT, n};
      return true;
    }
    case 'O': {                                       // #HLFO<dd>: two digits, 00 or 02-20
      if (rest[0] < '0' || rest[0] > '9' || rest[1] < '0' || rest[1] > '9' || rest[2] != '\0') return false;
      int n = (rest[0] - '0') * 10 + (rest[1] - '0');
      if (n == 1 || n > 20) return false;
      out = {CmdType::TX_POWER, n};
      return true;
    }
    case 'H':                                         // #HLFH<2|3>: the highest volume allowed
      if ((rest[0] != '2' && rest[0] != '3') || rest[1] != '\0') return false;
      out = {CmdType::VOLUME_CAP, rest[0] - '0'};
      return true;
    case 'B': { int n = atoi(rest); if (n < 1 || n > 3) return false; out = {CmdType::BRIGHTNESS, n}; return true; }
    case 'V': { int n = atoi(rest); if (n < 1 || n > 3) return false; out = {CmdType::VOLUME, n}; return true; }
    case 'L':
      if (*rest == 'A') { out = {CmdType::LOOP, 1}; return true; }
      if (*rest == 'O') { out = {CmdType::LOOP, 0}; return true; }
      return false;
    case 'W':
      if (*rest == '1') { out = {CmdType::SWITCHER, 1}; return true; }
      if (*rest == '0') { out = {CmdType::SWITCHER, 0}; return true; }
      return false;
    default:
      return false;
  }
}

static const char* playStr(Play p) {
  switch (p) {
    case Play::PLAYING: return "ing"; case Play::IDLE: return "idle"; case Play::UNKNOWN: return "?";
  }
  return "?";
}

int formatStatus(const Status& s, char* out, size_t outSize) {
  int n = snprintf(out, outSize,
    "#HL:FAN st=%s idx=%d name=%s play=%s prog=%d total=%d br=%d vol=%d loop=%s sw=%s err=%s",
    s.state == FanState::READY ? "READY" : "NOMCU", s.fileIndex,
    s.fileName[0] ? s.fileName : "-", playStr(s.play),
    s.progress, s.total, s.brightness, s.volume,
    s.loopAll ? "all" : "one", s.switcher < 0 ? "?" : (s.switcher ? "on" : "off"), s.err);
  return (n < 0 || (size_t)n >= outSize) ? -1 : n;
}

int formatCatalog(const Catalog& c, char* out, size_t outSize) {
  if (outSize == 0) return 0;
  size_t limit = outSize - 1 < REPLY_MAX ? outSize - 1 : REPLY_MAX;
  int n = snprintf(out, outSize, "#HL:FANDCIM n=%d", c.count);
  if (n < 0 || (size_t)n > limit) { out[0] = '\0'; return 0; }
  for (int i = 0; i < c.count; ++i) {
    char item[CLIP_NAME_MAX + 16];
    int m = snprintf(item, sizeof(item), " %d=%s", i + 1, c.entries[i].name);
    if (m < 0 || (size_t)(n + m) > limit) break;
    memcpy(out + n, item, (size_t)m + 1);
    n += m;
  }
  return n;
}

} // namespace holofan
