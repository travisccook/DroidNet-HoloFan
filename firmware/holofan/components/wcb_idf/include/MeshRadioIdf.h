// ESP-IDF (5.4) adapter between the ESP-NOW radio and wcb::WcbNode for the
// HoloFan: implements wcb::WcbPlatform and brings the radio up as a WCB mesh
// node (STA, power save off, fixed channel, MAC 02:oct2:oct3:00:00:id).
// Same responsibilities as the HoloLifter's lib/WcbArduino/MeshRadio. Only one
// MeshRadioIdf may exist (the ESP-NOW receive callback is a plain function).
#ifndef MESH_RADIO_IDF_H
#define MESH_RADIO_IDF_H
#include <stddef.h>
#include <stdint.h>
#include "NodeConfig.h"
#include "WcbNode.h"

class MeshRadioIdf : public wcb::WcbPlatform {
 public:
  // Once per boot, before start(): netif, default event loop, esp_wifi_init
  // with RAM-only Wi-Fi storage. False if Wi-Fi could not be initialised.
  static bool initWifi();
  // Bring the radio up for `cfg` and route received frames to node.ingest().
  // Does not call node.begin(). False if ESP-NOW could not start or is not
  // version 2 (WCB frames are 252 bytes; v1 caps a frame at 250).
  // Also used to return from maintenance mode (after stop()).
  bool start(const nodecfg::NodeConfig& cfg, wcb::WcbNode& node);
  // Leave the mesh: unregister the callback and de-initialise ESP-NOW.
  // Wi-Fi stays started in STA mode so maintenance mode can join a network.
  void stop();
  // stop(), then esp_wifi_stop(): RF, PHY and baseband off, no receive or
  // transmit current (bench #HLFY radio-off). start() brings it back. False
  // if Wi-Fi did not stop (it is then still on).
  bool powerOff();
  bool v2() const { return version_ >= 2; }
  // Unicast ESP-NOW frames received since boot (each one was ACKed on air).
  static uint32_t unicastRx();
  bool macOk() const { return macOk_; }
  // One-line radio diagnostics (bench): channel, raw receive count before any
  // gating, unicast count, last length/RSSI/source/destination, send failures.
  int formatDiag(char* out, size_t size) const;

  // wcb::WcbPlatform (called only from the mesh task)
  uint32_t nowMs() override;
  bool send(const uint8_t mac[6], const uint8_t* data, size_t len) override;
  bool addPeer(const uint8_t mac[6]) override;  // ESP_ERR_ESPNOW_EXIST counts as success

 private:
  uint32_t version_ = 0;
  bool     macOk_ = false;
  uint32_t sendFails_ = 0;
};

#endif
