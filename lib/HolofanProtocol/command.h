#ifndef HOLOFAN_COMMAND_H
#define HOLOFAN_COMMAND_H
#include "HolofanStatus.h"

namespace holofan {

enum class CmdType {
  NONE, POWER_ON, POWER_OFF, PLAY_INDEX, PAUSE, NEXT, PREV,
  BRIGHTNESS, VOLUME, MUTE, UNMUTE, LOOP, SWITCHER,
  CATALOG_REFRESH, CATALOG_DUMP, STATUS,
  DIAG,             // #HLFQ: MCU link diagnostics (query)
  PLAY_SLOT,        // #HLFT<NN>: play the clip named NN-... / NN_... (0.9.3)
  LOUD_TEST,        // #HLFY<r><s>: bench: volume 3 for s seconds, radio on (r=1) / off (r=0) / receive only (r=2); param r*1000+s
  SAG_METER,        // #HLFE<lo><hi><s>: bench: brownout comparator levels lo..hi as a sag meter for s seconds; param lo*10000+hi*1000+s
  TX_PROBE,         // #HLFJ<lo><hi><dBm>: bench: dip per probe frame at levels lo..hi, TX power dBm; param lo*10000+hi*1000+dBm
  HEARTBEAT,        // #HLFK<ss>: bench: mesh heartbeat period ss seconds (2-25), 0 = default
  BLUETOOTH,        // #HLFA<0|1>: the MCU's Bluetooth off (default since 0.9.9) / on; saved
  TX_POWER          // #HLFO<dd>: mesh TX power dd dBm (2-20), 0 = default (8); saved (0.9.10)
};

struct Cmd {
  CmdType type = CmdType::NONE;
  int param = 0;   // PLAY_INDEX: 1-based index; PLAY_SLOT: NN 0..99; BRIGHTNESS/VOLUME: level; LOOP: 0=one/1=all; SWITCHER: 0/1
  Cmd() = default;
  Cmd(CmdType t, int p) : type(t), param(p) {}
};

// Parse a "#HLF<action>[param]" line into a Cmd. Returns false on unknown/invalid.
bool parseCommand(const char* line, Cmd& out);

// Longest reply that fits one mesh command with the |CRC suffix.
constexpr size_t REPLY_MAX = 187;

// "#HL:FAN st=READY|NOMCU idx=.. name=.. play=.. prog=.. total=.. br=.. vol=.. loop=.. sw=.. err=..".
// Returns the length, or -1 if it did not fit.
int formatStatus(const Status& s, char* out, size_t outSize);

// "#HL:FANDCIM n=<count> 1=<name> 2=<name> ...". n is always the full count;
// entries that would push the line past REPLY_MAX chars (or outSize) are left
// out. Returns the length.
int formatCatalog(const Catalog& c, char* out, size_t outSize);

} // namespace holofan
#endif
