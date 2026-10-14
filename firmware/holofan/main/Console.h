// The fan's console: UART0 (the 5-hole programming header, 115200). Lines
// printed with con() also go to the WCB that last sent us a command, as
// RTERM (docs/SERIAL_COMMANDS.md): con() queues them and the mesh task sends them. Lines
// typed on UART0 are handed to the mesh task (fanMeshConsoleLine).
#ifndef FAN_CONSOLE_H
#define FAN_CONSOLE_H
#include <stddef.h>

constexpr size_t CON_LINE_MAX = 160;   // one RTERM packet

// Create the RTERM queue and start the UART0 reader task.
void conStart();
// Print one line (a newline is added) and queue it for RTERM. Any task.
void con(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Mesh task: next line to mirror. False if none.
bool conTakeMirror(char* out, size_t size);

#endif
