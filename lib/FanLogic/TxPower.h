// The fan's mesh TX power (#HLFO<dd>, HoloFan 0.9.10). At volume 3 the ESP's
// own transmit bursts at 20 dBm dip the shared supply and start ~10 s audio
// silences (bench 2026-10-13, docs/HOW_IT_WORKS.md).
#ifndef FANLOGIC_TXPOWER_H
#define FANLOGIC_TXPOWER_H

namespace fanlogic {

constexpr int TX_DBM_MIN = 2;      // the radio's floor
constexpr int TX_DBM_MAX = 20;     // the default, and the power before 0.9.10

// The dBm to transmit at for a saved #HLFO value. 0 (never set, or #HLFO00)
// and anything outside 2-20 (a damaged NVS byte) mean the default.
constexpr int txPowerDbm(int saved) { return (saved >= TX_DBM_MIN && saved <= TX_DBM_MAX) ? saved : TX_DBM_MAX; }

// The same in esp_wifi_set_max_tx_power's 0.25 dBm steps.
constexpr int txPowerQuarterDbm(int saved) { return txPowerDbm(saved) * 4; }

} // namespace fanlogic
#endif
