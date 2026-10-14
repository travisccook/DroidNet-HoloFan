#include "MeshRadioIdf.h"
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "mesh";
static wcb::WcbNode* s_node = nullptr;

// Radio diagnostics, written on the Wi-Fi task (word-sized, read racily for display).
static volatile uint32_t s_rawRx = 0, s_rawUnicast = 0;
static volatile int      s_lastLen = 0, s_lastRssi = 0, s_minRssi = 0, s_maxRssi = -127;
static volatile uint8_t  s_lastSrc[6] = {0};
static volatile uint8_t  s_lastDst[6] = {0};

// Runs on the Wi-Fi task: copy into the node's ring and return.
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  s_rawRx = s_rawRx + 1;
  if (info) {
    if (info->src_addr) for (int i = 0; i < 6; i++) s_lastSrc[i] = info->src_addr[i];
    if (info->des_addr) {
      for (int i = 0; i < 6; i++) s_lastDst[i] = info->des_addr[i];
      if (!(info->des_addr[0] & 1)) s_rawUnicast = s_rawUnicast + 1;
    }
    if (info->rx_ctrl) {
      int r = info->rx_ctrl->rssi;
      s_lastRssi = r;
      if (r < s_minRssi) s_minRssi = r;
      if (r > s_maxRssi) s_maxRssi = r;
    }
  }
  s_lastLen = len;
  wcb::WcbNode* n = s_node;
  if (n && info && info->src_addr && len > 0) n->ingest(info->src_addr, data, (size_t)len);
}

int MeshRadioIdf::formatDiag(char* out, size_t size) const {
  uint8_t primary = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&primary, &second);
  return snprintf(out, size,
                  "radio: ch=%u rawrx=%lu unicast=%lu lastlen=%d rssi last/min/max=%d/%d/%d "
                  "src=%02X:%02X:%02X:%02X:%02X:%02X dst=%02X:%02X:%02X:%02X:%02X:%02X txfail=%lu",
                  primary, (unsigned long)s_rawRx, (unsigned long)s_rawUnicast, s_lastLen, s_lastRssi, s_minRssi,
                  s_maxRssi, s_lastSrc[0], s_lastSrc[1], s_lastSrc[2], s_lastSrc[3], s_lastSrc[4], s_lastSrc[5],
                  s_lastDst[0], s_lastDst[1], s_lastDst[2], s_lastDst[3], s_lastDst[4], s_lastDst[5],
                  (unsigned long)sendFails_);
}

bool MeshRadioIdf::initWifi() {
  esp_err_t err = esp_netif_init();
  if (err == ESP_OK) err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {      // INVALID_STATE: loop already exists
    ESP_LOGE(TAG, "netif/event loop init failed: %s", esp_err_to_name(err));
    return false;
  }
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);   // never write Wi-Fi settings to flash
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool MeshRadioIdf::start(const nodecfg::NodeConfig& cfg, wcb::WcbNode& node) {
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) ESP_LOGW(TAG, "set_mode(STA) failed: %s", esp_err_to_name(err));

  // The MAC can only be set while the interface is disabled (esp_wifi.h),
  // so before esp_wifi_start(). On a return from maintenance Wi-Fi is
  // already started with our MAC: then it is left as it is.
  uint8_t mac[6], readBack[6] = {0};
  wcb::nodeMac(cfg.oct2, cfg.oct3, cfg.id, mac);
  esp_wifi_get_mac(WIFI_IF_STA, readBack);
  if (memcmp(mac, readBack, 6) != 0) {
    esp_wifi_stop();
    err = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_mac failed: %s", esp_err_to_name(err));
  }

  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
    return false;
  }
  esp_wifi_disconnect();                                    // no AP association while on the mesh
  esp_wifi_set_ps(WIFI_PS_NONE);                            // modem sleep drops ESP-NOW frames
  err = esp_wifi_set_channel(cfg.channel, WIFI_SECOND_CHAN_NONE);
  if (err != ESP_OK) ESP_LOGW(TAG, "set_channel(%u) failed: %s", cfg.channel, esp_err_to_name(err));
  uint8_t primary = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&primary, &second);
  if (primary != cfg.channel) {                             // retry once, as the Arduino adapter does
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_set_channel(cfg.channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_get_channel(&primary, &second);
    if (primary != cfg.channel) ESP_LOGW(TAG, "radio on channel %u, mesh is on %u", primary, cfg.channel);
  }

  esp_wifi_get_mac(WIFI_IF_STA, readBack);
  macOk_ = memcmp(mac, readBack, 6) == 0;
  ESP_LOGI(TAG, "STA MAC %02X:%02X:%02X:%02X:%02X:%02X %s", readBack[0], readBack[1], readBack[2], readBack[3],
           readBack[4], readBack[5], macOk_ ? "OK" : "(set_mac FAILED: WCBs will drop our frames)");

  err = esp_now_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
    return false;
  }
  version_ = 0;
  esp_now_get_version(&version_);
  ESP_LOGI(TAG, "ESP-NOW version %lu", (unsigned long)version_);
  if (version_ < 2) {
    ESP_LOGE(TAG, "ESP-NOW v1 cannot carry 252-byte WCB frames: mesh not started");
    esp_now_deinit();
    return false;
  }
  s_node = &node;
  esp_now_register_recv_cb(onRecv);
  return true;
}

void MeshRadioIdf::stop() {
  esp_now_unregister_recv_cb();
  s_node = nullptr;
  esp_now_deinit();
}

// IDF ESP-NOW guide: de-initialise ESP-NOW before stopping Wi-Fi (stop()
// does), start Wi-Fi before initialising it (start() does).
bool MeshRadioIdf::powerOff() {
  stop();
  esp_err_t err = esp_wifi_stop();
  if (err != ESP_OK) ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
  return err == ESP_OK;
}

uint32_t MeshRadioIdf::unicastRx() { return s_rawUnicast; }

uint32_t MeshRadioIdf::nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

bool MeshRadioIdf::send(const uint8_t mac[6], const uint8_t* data, size_t len) {
  esp_err_t err = esp_now_send(mac, data, len);
  if (err == ESP_OK) return true;
  if (sendFails_++ % 100 == 0)
    ESP_LOGW(TAG, "esp_now_send(%u bytes) failed: %s (%lu failures)", (unsigned)len, esp_err_to_name(err),
             (unsigned long)sendFails_);
  return false;
}

bool MeshRadioIdf::addPeer(const uint8_t mac[6]) {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;          // the current channel
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;      // WCB meshes are unencrypted
  esp_err_t err = esp_now_add_peer(&peer);
  if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) ESP_LOGW(TAG, "add_peer failed: %s", esp_err_to_name(err));
  return err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST;
}
