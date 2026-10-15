// HoloFan firmware for the 5D HoLo fan's ESP32-C2: a WCB mesh
// node (id 16 "HoloFan") that drives the fan's LED/SD MCU over UART1.
// Runs from ota_0/ota_1 under our rollback bootloader; updates arrive over
// Wi-Fi in maintenance mode (docs/MAINTENANCE_API.md).
#include "ConfigStoreIdf.h"
#include "Console.h"
#include "FanMcu.h"
#include "FanMesh.h"
#include "FanVersion.h"
#include "MaintIdf.h"
#include "nvs_flash.h"

extern "C" void app_main(void) {
  conStart();
  con("=== HoloFan %s (built %s %s) ===", HOLOFAN_VERSION, __DATE__, __TIME__);
  fanMcuStart();                  // GPIO/UART1 first, as stock, then the MCU handshake
  maintidf::setClipHooks({fanMcuReady, fanMcuLease, fanMcuRelease, fanMcuSpi, fanMcuClipDiag, fanMcuTrace});

  // Never erase NVS: it holds the stock PHY calibration and identity keys
  // (docs/HOW_IT_WORKS.md). If it will not mount we run on defaults.
  esp_err_t err = nvs_flash_init();
  if (err != ESP_OK) con("[nvs] init failed (%s): running on defaults, NOT erasing", esp_err_to_name(err));
  fanMeshStart();
}
