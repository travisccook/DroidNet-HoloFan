#include "MaintIdf.h"
#include "MaintShared.h"
#include <atomic>
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "esp_wifi.h"
#include "mdns.h"

namespace maintidf {

static const char* TAG = "maint";
static esp_netif_t*       s_netif = nullptr;
static httpd_handle_t     s_http = nullptr;
static std::atomic<bool>  s_gotIp{false};
static std::atomic<bool>  s_joining{false};   // reconnect on disconnect only while in maintenance
static std::atomic<bool>  s_done{false};
static std::atomic<bool>  s_reboot{false};
static char               s_node[25] = {0};
static char               s_fw[28] = {0};
static ClipHooks          s_clips = {};
static char               s_scratch[4096];        // "s" replies; HTTP task only (0.8.6: was 8 KB; heap is tight)
static char               s_last[608] = "none";   // the last clip PUT/DELETE, for /api/info
// WIFI_EVENT_STA_DISCONNECTED since maintenance started, and the last
// reason code (wifi_event_sta_disconnected_t.reason). Written by the event
// task only; word-sized reads elsewhere (no atomics on the C2).
static volatile uint32_t s_disc = 0;
static volatile uint32_t s_reason = 0;
static constexpr const char* CLIPS_PREFIX = "/api/clips/";

static void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) s_gotIp = true;
  else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) s_gotIp = false;
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    s_gotIp = false;
    s_disc = s_disc + 1;
    if (data) s_reason = ((const wifi_event_sta_disconnected_t*)data)->reason;
    if (s_joining) esp_wifi_connect();
  }
}

bool init() {
  s_netif = esp_netif_create_default_wifi_sta();
  if (!s_netif) return false;
  esp_err_t err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr);
  if (err == ESP_OK) err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, onEvent, nullptr);
  if (err != ESP_OK) ESP_LOGE(TAG, "event handler: %s", esp_err_to_name(err));
  return err == ESP_OK;
}

void join(const nodecfg::WifiProfile& p, const char* hostname) {
  wifi_config_t wc = {};
  strncpy((char*)wc.sta.ssid, p.ssid, sizeof(wc.sta.ssid));
  strncpy((char*)wc.sta.password, p.pass, sizeof(wc.sta.password));
  wc.sta.threshold.authmode = p.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
  wc.sta.pmf_cfg.capable = true;
  s_gotIp = false;
  s_disc = 0; s_reason = 0;
  s_joining = true;
  esp_wifi_set_mode(WIFI_MODE_STA);                    // the mesh may never have started Wi-Fi
  esp_wifi_start();                                    // ESP_OK if already started
  esp_wifi_set_ps(WIFI_PS_NONE);                       // no modem sleep while serving uploads
  if (s_netif && hostname) esp_netif_set_hostname(s_netif, hostname);
  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
  if (err == ESP_OK) err = esp_wifi_connect();
  if (err != ESP_OK) ESP_LOGE(TAG, "join: %s", esp_err_to_name(err));
}

bool hasIp() { return s_gotIp; }

void ipString(char* out, size_t size) {
  esp_netif_ip_info_t ip = {};
  if (s_netif) esp_netif_get_ip_info(s_netif, &ip);
  snprintf(out, size, IPSTR, IP2STR(&ip.ip));
}

esp_err_t sendJson(httpd_req_t* r, const char* status, const char* body) {
  httpd_resp_set_status(r, status);
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_sendstr(r, body);
}

static esp_err_t handleInfo(httpd_req_t* r) {
  const esp_partition_t* run = esp_ota_get_running_partition();
  wifi_ap_record_t ap = {};
  char wifi[192];
  wifi_ps_type_t ps = WIFI_PS_NONE;
  int8_t txp = 0;
  esp_wifi_get_ps(&ps);
  esp_wifi_get_max_tx_power(&txp);                     // 0.25 dBm units
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    snprintf(wifi, sizeof(wifi),
             "{\"rssi\":%d,\"ch\":%u,\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ps\":%d,\"txp\":%d,"
             "\"disc\":%lu,\"reason\":%lu}",
             ap.rssi, (unsigned)ap.primary, ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4],
             ap.bssid[5], (int)ps, (int)txp, (unsigned long)s_disc, (unsigned long)s_reason);
  else
    snprintf(wifi, sizeof(wifi), "{\"rssi\":null,\"ch\":null,\"disc\":%lu,\"reason\":%lu}", (unsigned long)s_disc,
             (unsigned long)s_reason);
  char body[1024];
  snprintf(body, sizeof(body),
           "{\"node\":\"%s\",\"fw\":\"%s\",\"slot\":\"%s\",\"mode\":\"maint\",\"spi\":%s,\"heap\":%u,\"hmin\":%u,\"blk\":%u,"
           "\"wifi\":%s,\"last\":\"%s\"}",
           s_node, s_fw, run ? run->label : "?", s_clips.spi ? "true" : "false",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA), wifi, s_last);
  return sendJson(r, "200 OK", body);
}

// Spec 2026-10-07 D7: while an upload session runs it owns the clip side
// and the staging slot. True (409 sent) if this request must be refused.
static bool refusedForUpload(httpd_req_t* r) {
  if (!uploadActive()) return false;
  sendJson(r, "409 Conflict", "{\"ok\":false,\"err\":\"upload\"}");
  return true;
}

// Streams the raw request body into the inactive OTA slot.
static esp_err_t handleOta(httpd_req_t* r) {
  if (refusedForUpload(r)) return ESP_FAIL;   // body unread: close
  if (r->content_len == 0) return sendJson(r, "400 Bad Request", "{\"ok\":false,\"err\":\"length\"}");
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (!part || r->content_len > part->size) return sendJson(r, "400 Bad Request", "{\"ok\":false,\"err\":\"length\"}");
  esp_ota_handle_t ota = 0;
  // OTA_WITH_SEQUENTIAL_WRITES: esp_ota_write() erases each sector as the
  // image reaches it, instead of esp_ota_begin() erasing the whole image
  // size up front. That up-front erase blocks for seconds, lwIP stops
  // ACKing and the sender's TCP backs off (bench 2026-10-06: OTA needed 2-4
  // tries; the clip staging ring had the same stall, 0.7.1).
  esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
  static char buf[2048];   // HTTP task only
  size_t left = r->content_len;
  int timeouts = 0;                                    // a silent link must not hang the server
  while (err == ESP_OK && left > 0) {
    int n = httpd_req_recv(r, buf, left < sizeof(buf) ? left : sizeof(buf));
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts >= 3) { err = ESP_ERR_TIMEOUT; break; }
      continue;
    }
    timeouts = 0;
    if (n <= 0) { err = ESP_FAIL; break; }
    err = esp_ota_write(ota, buf, (size_t)n);
    left -= (size_t)n;
  }
  if (err == ESP_OK) err = esp_ota_end(ota);          // verifies the image
  else if (ota) esp_ota_abort(ota);
  if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
  if (err != ESP_OK) {
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(err));
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    return sendJson(r, "500 Internal Server Error", body);   // docs/MAINTENANCE_API.md
  }
  ESP_LOGI(TAG, "OTA image (%u bytes) staged in %s", (unsigned)r->content_len, part->label);
  esp_err_t sent = sendJson(r, "200 OK", "{\"ok\":true}");
  s_reboot = true;
  return sent;
}

static esp_err_t handleDone(httpd_req_t* r) {
  if (refusedForUpload(r)) return ESP_OK;
  esp_err_t sent = sendJson(r, "200 OK", "{\"ok\":true}");
  s_done = true;
  return sent;
}

// The clip name after "/api/clips/", percent-decoded, up to a raw '?' or
// '#'. False if empty, badly escaped or too long (never truncated).
static bool clipName(httpd_req_t* r, char* out, size_t size) {
  return fanlogic::decodeClipPath(r->uri + strlen(CLIPS_PREFIX), out, size);
}

// "?key=0|1" on the request URL; `dflt` when absent or unreadable.
static bool queryFlag(httpd_req_t* r, const char* key, bool dflt) {
  char q[64], v[4];
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK) return dflt;
  if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return dflt;
  return v[0] == '1';
}

static esp_err_t sendClipErr(httpd_req_t* r, fanlogic::ClipErr e, bool partial) {
  using fanlogic::ClipErr;
  const char* status = "500 Internal Server Error";
  if (e == ClipErr::NAME || e == ClipErr::LENGTH) status = "400 Bad Request";
  else if (e == ClipErr::NOT_FOUND) status = "404 Not Found";
  else if (e == ClipErr::EXISTS) status = "409 Conflict";
  char body[80];
  if (status[0] == '5')
    snprintf(body, sizeof(body), "{\"ok\":false,\"err\":\"%s\",\"partial\":%s}", fanlogic::clipErrName(e),
             partial ? "true" : "false");
  else
    snprintf(body, sizeof(body), "{\"ok\":false,\"err\":\"%s\"}", fanlogic::clipErrName(e));
  return sendJson(r, status, body);
}

// Lease the MCU link or answer 503.
static fanlogic::McuLink* leaseOr503(httpd_req_t* r) {
  if (!s_clips.ready()) { sendJson(r, "503 Service Unavailable", "{\"ok\":false,\"err\":\"nomcu\"}"); return nullptr; }
  fanlogic::McuLink* link = s_clips.lease(LEASE_WAIT_MS);
  if (!link) sendJson(r, "503 Service Unavailable", "{\"ok\":false,\"err\":\"busy\"}");
  return link;
}

// "<op> <name> <err> f=<frames> <diag>" into s_last, with '"' and '\\' made
// safe for the JSON string in /api/info.
void noteLast(const char* op, const char* name, fanlogic::ClipErr e, size_t frames, const char* diag) {
  snprintf(s_last, sizeof(s_last), "%s %s %s f=%u %s", op, name, fanlogic::clipErrName(e), (unsigned)frames, diag);
  for (char* p = s_last; *p; p++)
    if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = '.';
}

// GET /api/trace: the MCU conversation of the last clip operation, as text
// (bytes outside 0x20..0x7E except '\n' shown as \xNN).
static esp_err_t handleTrace(httpd_req_t* r) {
  httpd_resp_set_type(r, "text/plain");
  const char* t = s_clips.trace ? s_clips.trace() : "";
  char out[96];
  size_t o = 0;
  for (const char* p = t;; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == 0 || o + 5 > sizeof(out)) {
      out[o] = '\0';
      httpd_resp_sendstr_chunk(r, out);
      o = 0;
      if (c == 0) break;
    }
    if (c == '\n' || (c >= 0x20 && c < 0x7f)) out[o++] = (char)c;
    else o += (size_t)snprintf(out + o, sizeof(out) - o, "\\x%02x", c);
  }
  return httpd_resp_sendstr_chunk(r, nullptr);
}

// POST /api/card/format?confirm=FORMAT-SD-CARD: erase the SD card (MCU "v").
// Owner-approved 2026-10-06; never reachable from the mesh. 400 without the
// exact token, then the clip list after the format.
static esp_err_t handleFormat(httpd_req_t* r) {
  if (refusedForUpload(r)) return ESP_OK;
  char q[64], confirm[24] = "";
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "confirm", confirm, sizeof(confirm));
  if (strcmp(confirm, fanlogic::FORMAT_CONFIRM) != 0)
    return sendJson(r, "400 Bad Request", "{\"ok\":false,\"err\":\"confirm\"}");
  fanlogic::McuLink* link = leaseOr503(r);
  if (!link) return ESP_OK;
  ESP_LOGW(TAG, "FORMAT SD card requested");
  fanlogic::formatCard(*link, confirm, s_scratch, sizeof(s_scratch));
  char diag[160] = "";
  if (s_clips.diag) s_clips.diag(diag, sizeof(diag));
  s_clips.release(true);
  noteLast("FMT", "card", fanlogic::ClipErr::OK, 0, diag);
  httpd_resp_set_type(r, "application/json");
  httpd_resp_sendstr_chunk(r, "{\"ok\":true,\"clips\":[");
  const char* p = s_scratch;
  char name[holofan::CLIP_NAME_MAX + 1], entry[2 * holofan::CLIP_NAME_MAX + 16];
  bool first = true;
  while (fanlogic::nextClipName(p, name, sizeof(name))) {
    if (fanlogic::clipJsonEntry(name, first, entry, sizeof(entry)) < 0) continue;
    httpd_resp_sendstr_chunk(r, entry);
    first = false;
  }
  httpd_resp_sendstr_chunk(r, "]}");
  return httpd_resp_sendstr_chunk(r, nullptr);
}

static esp_err_t handleClipList(httpd_req_t* r) {
  if (uploadActive()) return sendJson(r, "503 Service Unavailable", "{\"ok\":false,\"err\":\"busy\"}");
  fanlogic::McuLink* link = leaseOr503(r);
  if (!link) return ESP_OK;
  s_scratch[0] = '\0';
  int n = fanlogic::listClips(*link, s_scratch, sizeof(s_scratch));
  s_clips.release(false);
  if (n <= 0) s_scratch[0] = '\0';
  httpd_resp_set_type(r, "application/json");
  httpd_resp_sendstr_chunk(r, "[");
  const char* p = s_scratch;
  char name[holofan::CLIP_NAME_MAX + 1], entry[2 * holofan::CLIP_NAME_MAX + 16];
  bool first = true;
  while (fanlogic::nextClipName(p, name, sizeof(name))) {
    if (fanlogic::clipJsonEntry(name, first, entry, sizeof(entry)) < 0) continue;
    httpd_resp_sendstr_chunk(r, entry);
    first = false;
  }
  httpd_resp_sendstr_chunk(r, "]");
  return httpd_resp_sendstr_chunk(r, nullptr);
}

static esp_err_t handleClipDelete(httpd_req_t* r) {
  if (refusedForUpload(r)) return ESP_OK;
  char name[holofan::CLIP_NAME_MAX + 1];
  if (!clipName(r, name, sizeof(name)) || !fanlogic::isClipName(name))
    return sendClipErr(r, fanlogic::ClipErr::NAME, false);
  fanlogic::McuLink* link = leaseOr503(r);
  if (!link) return ESP_OK;
  fanlogic::ClipErr e = fanlogic::deleteClip(*link, name, s_scratch, sizeof(s_scratch), queryFlag(r, "force", false));
  char diag[160] = "";
  if (s_clips.diag) s_clips.diag(diag, sizeof(diag));
  s_clips.release(e == fanlogic::ClipErr::OK || e == fanlogic::ClipErr::STILL);
  noteLast("DEL", name, e, 0, diag);
  ESP_LOGI(TAG, "DELETE %s: %s", name, fanlogic::clipErrName(e));
  if (e != fanlogic::ClipErr::OK) return sendClipErr(r, e, e == fanlogic::ClipErr::STILL);
  return sendJson(r, "200 OK", "{\"ok\":true}");
}

// The request body for uploadClip: 10 s per recv (cfg.recv_wait_timeout),
// failure after 3 timeouts in a row, as /api/ota.
// The PUT body for uploadClip. The socket's receive timeout is cut to
// BODY_RECV_TIMEOUT_S for the upload (setRecvTimeout), so a Wi-Fi stall
// returns 0 ("nothing yet") about once a second and the uploader keeps
// pacing frames to the MCU; uploadClip decides when a stall is fatal.
void setRecvTimeout(httpd_req_t* r, int seconds) {
  struct timeval tv = {};
  tv.tv_sec = seconds;
  setsockopt(httpd_req_to_sockfd(r), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

class HttpBody : public fanlogic::BodyReader {
 public:
  explicit HttpBody(httpd_req_t* r) : r_(r) {}
  int read(uint8_t* buf, size_t n) override {
    int got = httpd_req_recv(r_, (char*)buf, n);
    if (got == HTTPD_SOCK_ERR_TIMEOUT) return 0;
    if (got > 0) consumed_ += (size_t)got;
    return got > 0 ? got : -1;   // 0 from recv = connection closed
  }
  size_t consumed() const { return consumed_; }
 private:
  httpd_req_t* r_;
  size_t consumed_ = 0;
};

// The spare OTA slot, if staging may use it: only once the running image is
// confirmed (not while a fresh OTA image is still proving itself, when the
// spare slot holds the rollback image). No up-front erase: erasing the
// whole slot blocks the CPU ~10 s, lwIP stops ACKing and the sender's TCP
// backs off past the stall limit (bench 2026-10-06). The ring erases one
// sector at a time instead, and its first push erases sector 0, so the old
// image header is gone and the bootloader can never boot the slot.
const esp_partition_t* stagingPartition() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) return nullptr;
  if (st != ESP_OTA_IMG_VALID && st != ESP_OTA_IMG_UNDEFINED) return nullptr;
  return esp_ota_get_next_update_partition(nullptr);
}

// Every PUT error returns ESP_FAIL after its reply: httpd then closes the
// socket instead of draining the unread body 32 bytes per recv
// (httpd_req_delete, CONFIG_HTTPD_PURGE_BUF_LEN), which took minutes on the
// bench and blocked the next request.
static esp_err_t failPut(esp_err_t) { return ESP_FAIL; }

static esp_err_t handleClipPut(httpd_req_t* r) {
  if (refusedForUpload(r)) return ESP_FAIL;   // body unread: close
  using fanlogic::ClipErr;
  char name[holofan::CLIP_NAME_MAX + 1];
  if (!clipName(r, name, sizeof(name)) || !fanlogic::isUploadName(name))
    return failPut(sendClipErr(r, ClipErr::NAME, false));
  if (r->content_len == 0 || r->content_len > fanlogic::CLIP_MAX_BYTES)
    return failPut(sendClipErr(r, ClipErr::LENGTH, false));
  // Bench switches, this upload only (0.7.2): ?mhz=10|20|40&clk=pll|xtal;
  // none = 40 MHz from the PLL, as before. r->uri holds the raw query
  // (httpd_req_get_url_query_str reads it from there too).
  fanlogic::SpiConfig spiCfg;
  const char* query = strchr(r->uri, '?');
  if (!fanlogic::parseSpiQuery(query ? query + 1 : nullptr, spiCfg))
    return failPut(sendJson(r, "400 Bad Request", "{\"ok\":false,\"err\":\"query\"}"));
  const esp_partition_t* part = stagingPartition();
  if (!part)
    return failPut(sendJson(r, "503 Service Unavailable", "{\"ok\":false,\"err\":\"verify\"}"));
  PartitionFlash flash(part);
  fanlogic::FrameRing ring(flash);
  if (!ring.begin()) return failPut(sendClipErr(r, ClipErr::FLASH, false));
  uint8_t* frame = (uint8_t*)heap_caps_malloc(2 * fanlogic::SPI_FRAME, MALLOC_CAP_DMA);
  if (!frame)
    return failPut(sendJson(r, "500 Internal Server Error", "{\"ok\":false,\"err\":\"nomem\",\"partial\":false}"));
  fanlogic::McuLink* link = leaseOr503(r);
  if (!link) {
    heap_caps_free(frame);
    return ESP_FAIL;                                   // 503 already sent
  }
  fanlogic::ClipSpi* spi = s_clips.spi(spiCfg);
  if (!spi) {
    s_clips.release(false);
    heap_caps_free(frame);
    return failPut(sendClipErr(r, ClipErr::SPI, false));
  }
  ESP_LOGI(TAG, "PUT %s: %u bytes, staging in %s (%u frames)", name, (unsigned)r->content_len, part->label,
           (unsigned)ring.capacity());
  HttpBody body(r);
  EspClock clock;
  setRecvTimeout(r, BODY_RECV_TIMEOUT_S);
  fanlogic::PutSource src(body, ring, clock, r->content_len, frame);
  fanlogic::UploadResult res = fanlogic::uploadClip(*link, *spi, src, clock, name, r->content_len,
                                                    frame + fanlogic::SPI_FRAME, s_scratch, sizeof(s_scratch),
                                                    fanlogic::PaceConfig());   // single PUT: TRICKLE
  setRecvTimeout(r, RECV_TIMEOUT_S);
  // crc: CRC32 of every byte sent; fl: flash erase ms / write ms / longest call ms.
  char diag[256];
  int dn = snprintf(diag, sizeof(diag), "b=%u%c crc=%08lx wmax=%lu igmax=%lu fl=%lu/%lu/%lu ", (unsigned)res.bodyBytes,
                    res.bodyEnd, (unsigned long)res.crc, (unsigned long)res.wmax, (unsigned long)res.igmax, flash.eraseMs(), flash.writeMs(), flash.longestMs());
  if (s_clips.diag && dn > 0 && (size_t)dn < sizeof(diag)) s_clips.diag(diag + dn, sizeof(diag) - (size_t)dn);
  s_clips.release(res.err != ClipErr::EXISTS);
  heap_caps_free(frame);
  noteLast("PUT", name, res.err, res.frames, diag);
  ESP_LOGI(TAG, "PUT %s: %s%s", name, fanlogic::clipErrName(res.err), res.partial ? " (partial file may remain)" : "");
  if (res.err != ClipErr::OK) {
    esp_err_t sent = sendClipErr(r, res.err, res.partial);
    return body.consumed() == r->content_len ? sent : failPut(sent);   // keep the reply when nothing is left unread
  }
  char ok[48];
  snprintf(ok, sizeof(ok), "{\"ok\":true,\"size\":%u}", (unsigned)r->content_len);
  return sendJson(r, "200 OK", ok);
}

void setClipHooks(const ClipHooks& hooks) { s_clips = hooks; }

const ClipHooks& clipHooks() { return s_clips; }
char* scratch() { return s_scratch; }
size_t scratchSize() { return sizeof(s_scratch); }

uint32_t wifiDisconnects() { return s_disc; }

bool startServer(const char* node, const char* fw) {
  strncpy(s_node, node, sizeof(s_node) - 1);
  strncpy(s_fw, fw, sizeof(s_fw) - 1);
  char host[25];
  nodecfg::hostnameFromAlias(node, host, sizeof(host));
  if (mdns_init() == ESP_OK) {
    mdns_hostname_set(host);
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
  } else {
    ESP_LOGW(TAG, "mDNS failed to start; use the IP address");
  }
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 6144;
  cfg.recv_wait_timeout = RECV_TIMEOUT_S;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.max_uri_handlers = 16;      // 3 + 5 clip routes + 4 /api/upload (the default is 8)
  cfg.lru_purge_enable = true;    // sockets of a vanished client never lock the resume out
  // Bench 2026-10-07 (0.8.4): each queued connection can hold a TCP window of
  // unread data in lwIP pbufs; with ~14 KB heap free during a session a
  // reconnect burst starved Wi-Fi RX (associated, but deaf for minutes).
  // Few sockets + LRU purge keep that memory bounded.
  cfg.max_open_sockets = 3;
  cfg.backlog_conn = 2;
  if (httpd_start(&s_http, &cfg) != ESP_OK) { s_http = nullptr; return false; }
  static const httpd_uri_t uris[] = {
    {"/api/info", HTTP_GET, handleInfo, nullptr},
    {"/api/ota", HTTP_POST, handleOta, nullptr},
    {"/api/done", HTTP_POST, handleDone, nullptr},
  };
  for (const httpd_uri_t& u : uris) httpd_register_uri_handler(s_http, &u);
  if (s_clips.lease) {
    static const httpd_uri_t clipUris[] = {
      {"/api/clips", HTTP_GET, handleClipList, nullptr},
      {"/api/trace", HTTP_GET, handleTrace, nullptr},
      {"/api/card/format", HTTP_POST, handleFormat, nullptr},
      {"/api/clips/*", HTTP_PUT, handleClipPut, nullptr},
      {"/api/clips/*", HTTP_DELETE, handleClipDelete, nullptr},
    };
    for (const httpd_uri_t& u : clipUris) httpd_register_uri_handler(s_http, &u);
    uploadRegister(s_http);
  }
  return true;
}

void stop() {
  if (s_http) { httpd_stop(s_http); s_http = nullptr; }
  mdns_free();
  s_joining = false;
  esp_wifi_disconnect();
  // Forget the network and stop Wi-Fi: this ends any scan or connect still
  // in flight, so the mesh comes back on its own channel with no late
  // reconnect. MeshRadioIdf::start() starts Wi-Fi again.
  wifi_config_t none = {};
  esp_wifi_set_config(WIFI_IF_STA, &none);
  esp_wifi_stop();
  s_gotIp = false;
}

bool takeDone() { return s_done.exchange(false); }
bool takeReboot() { return s_reboot.exchange(false); }

}  // namespace maintidf
