// The fan's mesh TX power (#HLFO<dd>, HoloFan 0.9.10; default 8 dBm since
// 0.9.13). The fan's radio shares its supply with the audio: at volume 3, 20 dBm
// bursts dipped it into brownout resets on the bench, 8 dBm ran clean.
#ifndef FANLOGIC_TXPOWER_H
#define FANLOGIC_TXPOWER_H

namespace fanlogic {

constexpr int TX_DBM_MIN = 2;      // the radio's floor
constexpr int TX_DBM_MAX = 20;     // full power: maintenance (Wi-Fi) always uses it
constexpr int TX_DBM_DEFAULT = 8;  // the mesh power when #HLFO saved none

// The dBm to transmit at on the mesh for a saved #HLFO value. 0 (never set,
// or #HLFO00) and anything outside 2-20 (a damaged NVS byte) mean the default.
constexpr int txPowerDbm(int saved) { return (saved >= TX_DBM_MIN && saved <= TX_DBM_MAX) ? saved : TX_DBM_DEFAULT; }

// The same in esp_wifi_set_max_tx_power's 0.25 dBm steps.
constexpr int txPowerQuarterDbm(int saved) { return txPowerDbm(saved) * 4; }

// Maintenance mode (uploads and OTA over Wi-Fi): full power, whatever #HLFO saved.
constexpr int maintTxQuarterDbm() { return TX_DBM_MAX * 4; }

} // namespace fanlogic
#endif
