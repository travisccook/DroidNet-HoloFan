#include "ConfigStoreIdf.h"
#include <string.h>
#include "nvs.h"

namespace configstore {

static const char* NS = "hlmesh";

static void getStr(nvs_handle_t h, const char* key, char* out, size_t size) {
  char tmp[72] = {0};
  size_t len = sizeof(tmp);
  if (nvs_get_str(h, key, tmp, &len) != ESP_OK) return;
  strncpy(out, tmp, size - 1);
  out[size - 1] = '\0';
}

static uint8_t getU8(nvs_handle_t h, const char* key, uint8_t dflt) {
  uint8_t v = dflt;
  return nvs_get_u8(h, key, &v) == ESP_OK ? v : dflt;
}

void load(nodecfg::NodeConfig& c, uint8_t defaultId, const char* defaultAlias) {
  nodecfg::setDefaults(c, defaultId, defaultAlias);
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;    // first boot: nothing stored yet
  uint8_t id = getU8(h, "id", defaultId);
  if (nodecfg::validId(id)) c.id = id;
  char alias[25] = {0};
  getStr(h, "alias", alias, sizeof(alias));
  if (nodecfg::validAlias(alias)) strcpy(c.alias, alias);
  c.replies = getU8(h, "reply", 1) != 0;
  uint8_t q = getU8(h, "q", 3);
  if (q <= 20) c.floorQ = q;
  uint8_t ch = getU8(h, "ch", 1);
  if (nodecfg::validChannel(ch)) c.channel = ch;
  c.oct2 = getU8(h, "oct2", 0);
  c.oct3 = getU8(h, "oct3", 0);
  char pw[40] = {0};
  getStr(h, "pw", pw, sizeof(pw));
  if (nodecfg::validPassword(pw)) strcpy(c.password, pw);
  c.checksum = getU8(h, "crc", 1) != 0;
  const char* sk[2] = {"s1", "s2"};
  const char* pk[2] = {"p1", "p2"};
  for (int i = 0; i < 2; i++) {
    nodecfg::WifiProfile w;
    getStr(h, sk[i], w.ssid, sizeof(w.ssid));
    getStr(h, pk[i], w.pass, sizeof(w.pass));
    if (nodecfg::validSsid(w.ssid) && nodecfg::validPass(w.pass)) c.profile[i] = w;
  }
  nvs_close(h);
}

bool save(const nodecfg::NodeConfig& c) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  bool ok = true;
  ok &= nvs_set_u8(h, "id", c.id) == ESP_OK;
  ok &= nvs_set_str(h, "alias", c.alias) == ESP_OK;
  ok &= nvs_set_u8(h, "reply", c.replies ? 1 : 0) == ESP_OK;
  ok &= nvs_set_u8(h, "q", c.floorQ) == ESP_OK;
  ok &= nvs_set_u8(h, "ch", c.channel) == ESP_OK;
  ok &= nvs_set_u8(h, "oct2", c.oct2) == ESP_OK;
  ok &= nvs_set_u8(h, "oct3", c.oct3) == ESP_OK;
  ok &= nvs_set_str(h, "pw", c.password) == ESP_OK;         // may be empty (mesh off)
  ok &= nvs_set_u8(h, "crc", c.checksum ? 1 : 0) == ESP_OK;
  ok &= nvs_set_str(h, "s1", c.profile[0].ssid) == ESP_OK;
  ok &= nvs_set_str(h, "p1", c.profile[0].pass) == ESP_OK;
  ok &= nvs_set_str(h, "s2", c.profile[1].ssid) == ESP_OK;
  ok &= nvs_set_str(h, "p2", c.profile[1].pass) == ESP_OK;
  ok &= nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

uint8_t loadPowerCycles() {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
  uint8_t n = getU8(h, "pcyc", 0);
  nvs_close(h);
  return n;
}

bool loadBluetooth() {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  uint8_t on = getU8(h, "bt", 0);
  nvs_close(h);
  return on != 0;
}

void saveBluetooth(bool on) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "bt", on ? 1 : 0);
  nvs_commit(h);
  nvs_close(h);
}

uint8_t loadTxPower() {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
  uint8_t dbm = getU8(h, "txp", 0);
  nvs_close(h);
  return dbm;
}

void saveTxPower(uint8_t dbm) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "txp", dbm);
  nvs_commit(h);
  nvs_close(h);
}

void savePowerCycles(uint8_t n) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "pcyc", n);
  nvs_commit(h);
  nvs_close(h);
}

}  // namespace configstore
