// The MCU task: owns the UART1 link and the FanController, runs #HLF actions
// from a queue, polls the MCU and pulses the GPIO3 heartbeat. The mesh task
// talks to it only through fanMcuPost() and fanMcuSnapshot(), so a slow MCU
// transaction never delays mesh ACKs.
#ifndef FAN_MCU_H
#define FAN_MCU_H
#include "ClipOps.h"
#include "HolofanStatus.h"
#include "command.h"

void fanMcuStart();
// Queue one action (any task). False if the queue is full (dropped).
bool fanMcuPost(const holofan::Cmd& c);
// Copy the latest status and catalog (any task; either pointer may be null).
void fanMcuSnapshot(holofan::Status* status, holofan::Catalog* catalog);
// The MCU has answered get_setting since boot (OTA validity gate).
bool fanMcuAnswered();
// Right before a planned esp_restart(): hold the MCU link lines idle-high
// through the reset (see McuUart::holdForRestart).
void fanMcuHoldForRestart();
// "#HL:FANDIAG ..." line for #HLFQ (any task): state, UART1 bytes in, lines
// out, unanswered queries, and the last bytes the MCU sent (printable).
void fanMcuDiag(char* out, size_t size);
// Bench/debug: send one console-typed line ("<cmd> [args]") to the MCU and
// print its raw reply. Goes through buildLine(), so `v` and unsafe
// arguments are refused. False if the queue is full.
bool fanMcuRaw(const char* line);

// Bench (#HLFY): ask the MCU task to set volume v (1-3) and
// read it back (FanController::setVolumeChecked). Replaces a request the
// MCU task has not taken yet. Returns the request id (never 0), or 0 if the
// MCU task is not running.
uint32_t fanMcuVolumeChecked(int v);
// The read-back for request `id`: -1 while it is pending (or if a later
// request replaced it), else the volume the MCU reported (0 = none).
int fanMcuVolumeResult(uint32_t id);

// 0.9.8: the "[fan] MCU at connect: ..." line (settings at connect, first
// clip), once, when it exists. False until then and after it was taken.
bool fanMcuTakeBootLine(char* out, size_t size);
// UNPROMPTED and after-a-clip-lease reports raised while no RTERM line could
// go out (maintenance, uploads, before the first mesh command), joined, each
// with its label; false if none.
bool fanMcuTakeLatchedUnprompted(char* out, size_t size);

// Clip transfer (HTTP task): the MCU link is up and the MCU is READY.
bool fanMcuReady();
// Take the MCU link for a whole clip operation: the MCU task pauses its
// UART traffic and heartbeat until fanMcuRelease(). Null if the UART never
// came up or the lease was not free within timeoutMs.
fanlogic::McuLink* fanMcuLease(uint32_t timeoutMs);
// Give the link back; `refresh` queues a catalog refresh (as #HLFC).
void fanMcuRelease(bool refresh);
// The SPI2 path, set up on first use with `cfg` (this upload's clock: the
// device is re-added when it differs from the last one); null if the
// driver failed. Lease holder only.
fanlogic::ClipSpi* fanMcuSpi(const fanlogic::SpiConfig& cfg);
// Lease holder: SPI handshake stats and the MCU's last bytes, for /api/info.
void fanMcuClipDiag(char* out, size_t size);
// The MCU conversation of the last clip lease (bench trace; any task, racy by design).
const char* fanMcuTrace();

#endif
