// Maintenance mode transport for the HoloFan (docs/MAINTENANCE_API.md): join a stored
// Wi-Fi profile, announce <alias>.local over mDNS and serve the maintenance
// HTTP API (docs/MAINTENANCE_API.md) on port 80. The HTTP server runs on its
// own task; the mesh task only drives the MaintFsm and reads the flags below.
//
//   GET  /api/info  {"node":..,"fw":..,"slot":..,"mode":"maint","spi":true}
//   POST /api/ota   raw app image, Content-Length required; 200 then reboot
//   POST /api/done  200, then the node returns to the mesh
//   GET    /api/clips         the SD card's clip names
//   PUT    /api/clips/<name>[?mhz=10|20|40&clk=pll|xtal]  upload over SPI
//          (lib/FanLogic/ClipOps); the query picks this upload's SPI clock
//   DELETE /api/clips/<name>  delete
//   POST/PUT/GET/DELETE /api/upload   resumable clip upload session (UploadIdf.cpp)
// The clip routes never check the maintenance state: they serve whenever
// the HTTP server runs.
#ifndef MAINT_IDF_H
#define MAINT_IDF_H
#include "ClipOps.h"
#include "NodeConfig.h"

namespace maintidf {

// Once per boot, after esp_wifi_init(): the STA netif (DHCP) and the IP
// event handlers. False on failure (maintenance mode then stays refused).
bool init();
// Start joining `p` (the mesh must already be stopped); `hostname` is the
// DHCP host name.
void join(const nodecfg::WifiProfile& p, const char* hostname);
bool hasIp();
// Dotted IP of the station, or "0.0.0.0".
void ipString(char* out, size_t size);
// Start mDNS + HTTP. `node` is the alias, `fw` the firmware version.
bool startServer(const char* node, const char* fw);
// Stop HTTP + mDNS, forget the network and stop Wi-Fi (the mesh restarts it).
void stop();
// One-shot flags set by the HTTP task.
bool takeDone();
bool takeReboot();   // a verified OTA image is staged; reboot now
// A resumable upload session (POST /api/upload) is in
// prefill or sending: maintenance must hold (MaintFsm poll(..., hold)).
bool uploadActive();

// The fan's MCU link, provided by main (FanMcu) so this component does not
// depend on the app. Without hooks the clip routes are not registered.
struct ClipHooks {
  bool (*ready)();                                  // MCU link up and READY
  fanlogic::McuLink* (*lease)(uint32_t timeoutMs);  // null: busy / no link
  void (*release)(bool refresh);
  fanlogic::ClipSpi* (*spi)(const fanlogic::SpiConfig& cfg);   // this upload's clock; null: SPI driver failed
  void (*diag)(char* out, size_t size);             // lease holder: SPI + MCU diagnostics
  const char* (*trace)();                           // last clip lease's MCU conversation
};
// Call before startServer().
void setClipHooks(const ClipHooks& hooks);

}  // namespace maintidf

#endif
