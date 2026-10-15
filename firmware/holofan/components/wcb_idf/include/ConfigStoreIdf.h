// Persists nodecfg::NodeConfig in NVS (nvs_* API), namespace "hlmesh", the
// same keys the HoloLifter uses. Missing or invalid keys fall back to the
// defaults. Never touches any other namespace (the stock fan's "phy",
// "softap", "wifi_cfg" and "nvs.net80211" stay as they are).
#ifndef CONFIG_STORE_IDF_H
#define CONFIG_STORE_IDF_H
#include "NodeConfig.h"

namespace configstore {
void load(nodecfg::NodeConfig& c, uint8_t defaultId, const char* defaultAlias);
bool save(const nodecfg::NodeConfig& c);
// Power-cycle count for maint::PowerCycleTrigger (key "pcyc"; 0 if unset).
uint8_t loadPowerCycles();
void savePowerCycles(uint8_t n);
// 0.9.9: the saved Bluetooth choice (#HLFA): false (off) when never saved.
bool loadBluetooth();
void saveBluetooth(bool on);
// 0.9.10: the mesh TX power in dBm (#HLFO, key "txp"): 0 when never saved
// (the default; fanlogic::txPowerDbm turns it into the power to use).
uint8_t loadTxPower();
void saveTxPower(uint8_t dbm);
}

#endif
