// The HoloFan as a WCB mesh node: radio bring-up from the
// stored NodeConfig, #HLF / #HLZ dispatch, broadcast query replies, RTERM and
// the UART0 "mesh ..." / "hl ..." console commands. Everything runs on the
// mesh task except the ESP-NOW receive callback (MeshRadioIdf) and
// fanMeshConsoleLine (console task), which only copy.
#ifndef FAN_MESH_H
#define FAN_MESH_H

// Load the config and start the mesh task (the mesh stays off without a
// mesh password; the console keeps working).
void fanMeshStart();
// A line typed on UART0 (any task; copied into a queue).
void fanMeshConsoleLine(const char* line);
// A con() line now reaches a WCB as RTERM (an RTERM target exists and the
// node is on the mesh, or a #HLFY hold keeps lines for after it). Any task;
// a racy read is fine (0.9.8: the MCU task latches reports when false).
bool fanMeshCanSend();
// The mesh TX power in dBm that #HLFO saved (20 when never set). Any task.
int fanMeshTxDbm();

#endif
