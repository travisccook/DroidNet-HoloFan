// #HL lift-command parser and query-reply formatter (docs/SERIAL_COMMANDS.md).
// Pure C++: no Arduino headers, tested in the native env.
// Executing a command (MotionController calls) happens in src/HoloMesh.cpp.
#ifndef LIFT_COMMANDS_H
#define LIFT_COMMANDS_H
#include <stddef.h>
#include <stdint.h>

namespace lift {

enum class Cmd {
  NONE,
  UP, DOWN, STOP, HOME, GOTO, CLEAR,              // motion (CLEAR = #HLX)
  SPEED, MARGIN, MOTOR_ON, MOTOR_OFF,              // config
  SAVE, LOAD, CFG_RESET, REBOOT,
  FAN_ON, FAN_OFF,                                 // relay (#HLF1 / #HLF0)
  Q_STATUS, Q_POS, Q_LIMITS, Q_ENC, Q_INFO, Q_HELP // queries (reply)
};

struct LiftCmd {
  Cmd     cmd = Cmd::NONE;
  int32_t value = 0;      // GOTO position, SPEED 0-255, MARGIN >= 0
};

// Strip a leading SOH (0x01, WCB Wizard marker) and trailing CR/LF/space in
// place; returns the start of the cleaned line.
char* normalizeLine(char* line);

// Parse one normalized line. False if it is not a lift command (including
// every #HLF playback command and the #HLZ family, handled elsewhere).
bool parseLift(const char* line, LiftCmd& out);

bool isQuery(Cmd c);

// True for the commands that start motion (UP, DOWN, HOME, GOTO); false for STOP and all others.
bool isMotionStart(Cmd c);

// True if a raw (not yet normalized) line is exactly the stop command #HLS.
// Safe to call on the radio task: no writes, bounded reads.
bool isStopLine(const char* raw);

enum class State { IDLE, HOMING, MOVING, MOVING_UP, MOVING_DOWN, STOPPED, ERROR };

struct Status {
  State   state = State::IDLE;
  int32_t pos = 0;
  int32_t target = 0;
  bool    homed = false;
  uint8_t speed = 0;
  bool    upper = false;      // limit triggered
  bool    lower = false;
  bool    fan = false;        // relay on
  int64_t enc = 0;            // raw encoder count
  const char* error = "";     // MotionController error message
  const char* fw = "";
  const char* slot = "";      // running OTA slot label, e.g. "app0"
};

// Format the reply for a query command into out (no trailing CR; the WCB adds
// it). Returns the length, or -1 if `cmd` is not a query or out is too small.
int formatReply(Cmd cmd, const Status& s, char* out, size_t outSize);

}  // namespace lift
#endif
