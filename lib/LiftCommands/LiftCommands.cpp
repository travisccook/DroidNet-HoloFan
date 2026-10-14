#include "LiftCommands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace lift {

char* normalizeLine(char* line) {
  if (line[0] == '\x01') line++;
  size_t n = strlen(line);
  while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = '\0';
  return line;
}

// Parse an optionally signed decimal that must run to the end of the string.
static bool parseInt(const char* s, bool allowNeg, int32_t lo, int32_t hi, int32_t& out) {
  if (!*s) return false;
  const char* p = s;
  if (*p == '-') { if (!allowNeg) return false; p++; }
  if (!*p) return false;
  for (const char* q = p; *q; q++) if (*q < '0' || *q > '9') return false;
  if (strlen(p) > 9) return false;
  long v = strtol(s, nullptr, 10);
  if (v < lo || v > hi) return false;
  out = (int32_t)v;
  return true;
}

bool parseLift(const char* line, LiftCmd& out) {
  if (strncmp(line, "#HL", 3) != 0) return false;
  const char* r = line + 3;
  struct Word { const char* text; Cmd cmd; };
  static const Word words[] = {
    {"SAVE", Cmd::SAVE}, {"LOAD", Cmd::LOAD}, {"CFGRST", Cmd::CFG_RESET},
    {"ON", Cmd::MOTOR_ON}, {"OF", Cmd::MOTOR_OFF},
    {"F1", Cmd::FAN_ON}, {"F0", Cmd::FAN_OFF},
    {"U", Cmd::UP}, {"D", Cmd::DOWN}, {"S", Cmd::STOP}, {"H", Cmd::HOME},
    {"X", Cmd::CLEAR}, {"R", Cmd::REBOOT},
    {"Q", Cmd::Q_STATUS}, {"P", Cmd::Q_POS}, {"L", Cmd::Q_LIMITS},
    {"E", Cmd::Q_ENC}, {"I", Cmd::Q_INFO}, {"?", Cmd::Q_HELP},
  };
  for (const Word& w : words) {
    if (strcmp(r, w.text) == 0) { out.cmd = w.cmd; out.value = 0; return true; }
  }
  int32_t v;
  if (r[0] == 'G' && parseInt(r + 1, true, -10000000, 10000000, v)) { out.cmd = Cmd::GOTO; out.value = v; return true; }
  if (r[0] == 'V' && parseInt(r + 1, false, 0, 255, v)) { out.cmd = Cmd::SPEED; out.value = v; return true; }
  if (r[0] == 'M' && parseInt(r + 1, false, 0, 1000000, v)) { out.cmd = Cmd::MARGIN; out.value = v; return true; }
  return false;
}

bool isQuery(Cmd c) {
  return c == Cmd::Q_STATUS || c == Cmd::Q_POS || c == Cmd::Q_LIMITS || c == Cmd::Q_ENC ||
         c == Cmd::Q_INFO || c == Cmd::Q_HELP;
}

bool isMotionStart(Cmd c) {
  return c == Cmd::UP || c == Cmd::DOWN || c == Cmd::HOME || c == Cmd::GOTO;
}

bool isStopLine(const char* raw) {
  if (raw[0] == '\x01') raw++;
  if (strncmp(raw, "#HLS", 4) != 0) return false;
  for (const char* p = raw + 4; *p; p++)
    if (*p != '\r' && *p != '\n' && *p != ' ') return false;
  return true;
}

static const char* stateName(State s) {
  switch (s) {
    case State::IDLE:        return "IDLE";
    case State::HOMING:      return "HOMING";
    case State::MOVING:      return "MOVING";
    case State::MOVING_UP:   return "MOVING_UP";
    case State::MOVING_DOWN: return "MOVING_DOWN";
    case State::STOPPED:     return "STOPPED";
    case State::ERROR:       return "ERROR";
  }
  return "?";
}

// Copy an error message as one token: spaces -> '_', and drop characters
// the WCB treats specially (^ , { ;) so the reply survives routing.
static void tokenize(const char* in, char* out, size_t outSize) {
  size_t o = 0;
  for (const char* p = in; *p && o + 1 < outSize; p++) {
    char c = *p;
    if (c == ' ') c = '_';
    if (c == '^' || c == ',' || c == '{' || c == ';' || (unsigned char)c < 0x20) continue;
    out[o++] = c;
  }
  out[o] = '\0';
}

int formatReply(Cmd cmd, const Status& s, char* out, size_t outSize) {
  int n = -1;
  switch (cmd) {
    case Cmd::Q_STATUS: {
      n = snprintf(out, outSize, "#HL:STATUS state=%s pos=%ld target=%ld homed=%d speed=%u limits=%d,%d fan=%d",
                   stateName(s.state), (long)s.pos, (long)s.target, s.homed ? 1 : 0, (unsigned)s.speed,
                   s.upper ? 1 : 0, s.lower ? 1 : 0, s.fan ? 1 : 0);
      if (n >= 0 && (size_t)n < outSize && s.state == State::ERROR && s.error[0]) {
        char tok[64];
        tokenize(s.error, tok, sizeof(tok));
        n += snprintf(out + n, outSize - (size_t)n, " error=%s", tok);
      }
      break;
    }
    case Cmd::Q_POS:    n = snprintf(out, outSize, "#HL:POS %ld", (long)s.pos); break;
    case Cmd::Q_LIMITS: n = snprintf(out, outSize, "#HL:LIMITS upper=%d lower=%d", s.upper ? 1 : 0, s.lower ? 1 : 0); break;
    case Cmd::Q_ENC:    n = snprintf(out, outSize, "#HL:ENC %lld", (long long)s.enc); break;
    case Cmd::Q_INFO:   n = snprintf(out, outSize, "#HL:INFO ver=%s board=ESP32ThingPlus slot=%s", s.fw, s.slot); break;
    case Cmd::Q_HELP:
      n = snprintf(out, outSize, "#HL:HELP U D S H X G<pos> V<0-255> M<n> ON OF SAVE LOAD CFGRST R Q P L E I ? F1 F0 Z<P|I|N|KS|KP|X|Q>");
      break;
    default: return -1;
  }
  if (n < 0 || (size_t)n >= outSize) return -1;
  return n;
}

}  // namespace lift
