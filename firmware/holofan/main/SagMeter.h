// Bench supply instruments from the ESP32-C2's brownout comparator
// (lib/FanLogic/SagLog.h):
//   #HLFE<lo><hi><seconds> (0.9.6)  sag meter over time
//   #HLFJ<lo><hi><dBm>     (0.9.7)  dip per transmit burst: PROBE_FRAMES
//                                   broadcast probe frames per level at the
//                                   given TX power (restored afterwards)
// While either runs, the threshold is above the 2.51 V reset level and the
// brownout interrupt, flash close and RF power-down are off: there is NO
// brownout reset until it ends (a deep sag can crash the ESP instead; the
// panic handler restarts it). Every software restart keeps the analog
// threshold and the bootloader arms a direct brownout reset at it, so a
// shutdown handler and a panic-handler wrapper (0.9.7) put 2.51 V back first.
// At the end IDF's configuration is restored and the report goes to RTERM.
#ifndef FAN_SAG_METER_H
#define FAN_SAG_METER_H
#include <stddef.h>
#include <stdint.h>

// param = lo * 10000 + hi * 1000 + seconds (CmdType::SAG_METER). False if
// an instrument is running, the range is bad, or the task could not start.
bool sagStart(int param);
// param = lo * 10000 + hi * 1000 + dBm (CmdType::TX_PROBE); `send` puts one
// probe frame on the air (any task). False as sagStart.
bool sagStartProbe(int param, bool (*send)(const uint8_t* data, size_t len));
// An instrument holds the brownout detector (no brownout reset).
bool sagRunning();
// The running instrument is #HLFJ (it changes the TX power and sends).
bool sagProbeRunning();

#endif
