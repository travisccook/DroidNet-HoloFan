// The resumable clip upload session (docs/MAINTENANCE_API.md): POST/PUT/GET/
// DELETE /api/upload. Chunk requests (httpd task) stage bytes into a ChunkFeed
// over the spare OTA slot; a background upload task (runSession) feeds the
// MCU from it, so a client can lose its connection, come back and resume
// at the fan's `staged` offset.
#include <atomic>
#include <stdio.h>
#include <string.h>
#include "ChunkFeed.h"
#include "MaintShared.h"
#include "UploadSession.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace maintidf {
namespace {

using fanlogic::BodyState;
using fanlogic::ClipErr;
using fanlogic::SPI_FRAME;

const char* TAG = "upload";
constexpr uint32_t    UPLOAD_STACK = 4096;   // bytes (StackType_t is uint8_t on the C2); bench HWM ~2.1 KB used (D13)
constexpr UBaseType_t UPLOAD_PRIO = 4;         // below httpd (5), as the MCU task
constexpr uint32_t    RSSI_EVERY_MS = 1000;

enum class St { NONE, PREFILL, SENDING, OK, FAILED };

const char* stName(St s) {
  switch (s) {
    case St::PREFILL: return "prefill";
    case St::SENDING: return "sending";
    case St::OK: return "ok";
    case St::FAILED: return "failed";
    default: return "none";
  }
}

struct Session {
  St       st = St::NONE;
  uint32_t id = 0;
  char     name[holofan::CLIP_NAME_MAX + 1] = {0};
  size_t   size = 0;
  fanlogic::SpiConfig spi;
  fanlogic::PaceConfig pace;                   // 0.8.1 timing knobs of this session
  size_t   pre = 0;
  uint32_t dryMs = 0;
  ClipErr  err = ClipErr::OK;
  bool     partial = false;
  uint32_t req = 0, recon = 0, gap = 0, stall = 0, sent = 0;
  uintptr_t lastConn = 0;                      // token of the HTTP connection that sent the last chunk
  bool     haveRssi = false;
  int      rssiMin = 0, rssiMax = 0;
  uint32_t discAtOpen = 0, discAtEnd = 0;
  uint32_t stackFree = 0;                      // the task's stack high-water mark, bytes (at its end)
};

SemaphoreHandle_t   s_mu = nullptr;            // guards s_sess, s_feed, s_ring and s_flash
// Held by the upload task for each window (first pop .. GPIO18 ack): the
// chunk handler takes it around feed writes, so no flash erase/write
// (cache off ~40 ms) can swallow the MCU's short LOW ack. Order: s_gate, then s_mu.
SemaphoreHandle_t   s_gate = nullptr;
Session             s_sess;
std::atomic<bool>   s_active{false};           // prefill or sending (read by the mesh task)
PartitionFlash      s_flash;
fanlogic::FrameRing s_ring(s_flash);
EspClock            s_clock;
fanlogic::ChunkFeed s_feed(s_ring, s_clock);
uint8_t*            s_work = nullptr;          // 2 frames, DMA: [0] the feed's assembly buffer, [1] uploadClip's frame
uint8_t             s_rx[SPI_FRAME];           // chunk receive buffer (httpd task only)
char                s_json[768];               // httpd task only
char                s_line[480];               // upload task only

struct Lock {
  Lock() { xSemaphoreTake(s_mu, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mu); }
};

void keepCtx(void*) {}
uintptr_t s_connSeq = 0;                       // httpd task only

bool activeLocked() { return s_sess.st == St::PREFILL || s_sess.st == St::SENDING; }

esp_err_t sendErr(httpd_req_t* r, const char* status, const char* err) {
  char body[64];
  snprintf(body, sizeof(body), "{\"ok\":false,\"err\":\"%s\"}", err);
  return sendJson(r, status, body);
}

// Caller holds no lock.
void sampleRssi() {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
  Lock l;
  if (!activeLocked()) return;
  if (!s_sess.haveRssi || ap.rssi < s_sess.rssiMin) s_sess.rssiMin = ap.rssi;
  if (!s_sess.haveRssi || ap.rssi > s_sess.rssiMax) s_sess.rssiMax = ap.rssi;
  s_sess.haveRssi = true;
}

// The upload task's view of the feed: every call under the mutex.
class LockedChunkSource : public fanlogic::FrameSource {
 public:
  size_t count() override { Lock l; return s_feed.count(); }
  bool full() override { Lock l; return s_feed.full(); }
  bool prefilled() override { Lock l; return s_feed.prefilled(); }
  BodyState state() override { Lock l; return s_feed.state(); }
  ClipErr failErr() override { Lock l; return s_feed.failErr(); }
  bool pop(uint8_t* frame) override {
    Lock l;
    if (!s_feed.pop(frame)) return false;
    s_sess.st = St::SENDING;
    s_sess.sent++;
    return true;
  }
  void wait(uint32_t ms) override {
    uint32_t now = s_clock.nowMs();
    if ((uint32_t)(now - lastRssi_) >= RSSI_EVERY_MS) {
      lastRssi_ = now;
      sampleRssi();
    }
    vTaskDelay(pdMS_TO_TICKS(ms < 10 ? ms : 10));   // 0 = just yield
  }
  size_t bodyBytes() override { Lock l; return s_feed.bodyBytes(); }
  char bodyEnd() override { Lock l; return s_feed.bodyEnd(); }
  void windowBegin() override { xSemaphoreTake(s_gate, portMAX_DELAY); }
  void windowEnd() override { xSemaphoreGive(s_gate); }

 private:
  uint32_t lastRssi_ = 0;
};

// runSession's MCU link: the ClipHooks main gave MaintIdf.
class HookLease : public fanlogic::McuLease {
 public:
  fanlogic::McuLink* lease(uint32_t ms) override {
    if (!clipHooks().ready()) {
      vTaskDelay(pdMS_TO_TICKS(ms));
      return nullptr;
    }
    return clipHooks().lease(ms);
  }
  fanlogic::ClipSpi* spi(const fanlogic::SpiConfig& cfg) override { return clipHooks().spi(cfg); }
  void diag(char* out, size_t size) override {
    if (clipHooks().diag) clipHooks().diag(out, size);
    else if (size) out[0] = '\0';
  }
  void release(bool refresh) override { clipHooks().release(refresh); }
};

const char* paceName(fanlogic::Pace p) {
  return p == fanlogic::Pace::RESERVE ? "reserve" : p == fanlogic::Pace::BURST ? "burst" : "trickle";
}

// "last" after a session. Caller holds the lock.
void formatLast(const fanlogic::UploadResult& r, const char* diag) {
  size_t rmin = s_feed.ringMin();
  snprintf(s_line, sizeof(s_line),
           "b=%u%c crc=%08lx wmax=%lu igmax=%lu stg=%lu fl=%lu/%lu/%lu %s req=%lu recon=%lu dup=%u gap=%lu stall=%lu quiet=%lu ringmin=%d "
           "rssi=%d/%d disc=%lu stack=%lu",
           (unsigned)r.bodyBytes, r.bodyEnd, (unsigned long)r.crc, (unsigned long)r.wmax, (unsigned long)r.igmax,
           (unsigned long)s_feed.stagingRate(), s_flash.eraseMs(), s_flash.writeMs(),
           s_flash.longestMs(), diag, (unsigned long)s_sess.req, (unsigned long)s_sess.recon,
           (unsigned)s_feed.dupBytes(), (unsigned long)s_sess.gap, (unsigned long)s_sess.stall,
           (unsigned long)s_feed.longestQuietMs(), rmin == SIZE_MAX ? -1 : (int)rmin, s_sess.rssiMin,
           s_sess.rssiMax, (unsigned long)(s_sess.discAtEnd - s_sess.discAtOpen), (unsigned long)s_sess.stackFree);
}

void uploadTask(void*) {
  // This session's own copies: once its state turns final below, a new
  // POST may reuse s_sess and s_work at once.
  char name[holofan::CLIP_NAME_MAX + 1];
  size_t size;
  fanlogic::SpiConfig spi;
  fanlogic::PaceConfig pace;
  uint8_t* work;
  {
    Lock l;
    strcpy(name, s_sess.name);
    size = s_sess.size;
    spi = s_sess.spi;
    pace = s_sess.pace;
    work = s_work;
  }
  LockedChunkSource src;
  HookLease lease;
  char diag[200];
  fanlogic::UploadResult r = fanlogic::runSession(lease, src, s_clock, name, size, spi, pace, work + SPI_FRAME,
                                                  scratch(), scratchSize(), diag, sizeof(diag));
  {
    Lock l;
    // Review focus 1: the client may still be sending (e.g. after a sync
    // error the body was still arriving). Fail the feed so no chunk ever
    // touches the assembly buffer freed next (a COMPLETE or FAILED feed
    // never writes into it).
    if (s_feed.state() == BodyState::ARRIVING) s_feed.abort();
  }
  heap_caps_free(work);
  {
    Lock l;
    s_work = nullptr;
    s_sess.st = r.err == ClipErr::OK ? St::OK : St::FAILED;
    s_sess.err = r.err;
    s_sess.partial = r.partial;
    s_sess.discAtEnd = wifiDisconnects();
    s_sess.stackFree = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    formatLast(r, diag);
    s_active = false;           // under the lock: a new POST sets it true only after this
  }
  noteLast("UPL", name, r.err, r.frames, s_line);
  ESP_LOGI(TAG, "%s: %s%s", name, fanlogic::clipErrName(r.err), r.partial ? " (partial file may remain)" : "");
  {
    // The deepest calls (snprintf, noteLast, the log) are done: sample again.
    uint32_t fin = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    Lock l;
    if (fin < s_sess.stackFree) s_sess.stackFree = fin;
  }
  vTaskDelete(nullptr);
}

esp_err_t sendOpened(httpd_req_t* r, uint32_t id, size_t staged, size_t size) {
  char body[96];
  snprintf(body, sizeof(body), "{\"ok\":true,\"id\":%lu,\"staged\":%u,\"size\":%u}", (unsigned long)id,
           (unsigned)staged, (unsigned)size);
  return sendJson(r, "200 OK", body);
}

// POST /api/upload?name=&size=[&mhz=&clk=]: open a session.
esp_err_t handleOpen(httpd_req_t* r) {
  char q[192] = "";
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_ERR_HTTPD_RESULT_TRUNC)
    return sendErr(r, "400 Bad Request", "query");
  fanlogic::UploadQuery uq;
  fanlogic::QueryErr qe = fanlogic::parseUploadQuery(fanlogic::UploadReq::OPEN, q, uq);
  if (qe != fanlogic::QueryErr::OK) return sendErr(r, "400 Bad Request", fanlogic::queryErrName(qe));

  fanlogic::OpenDecision d;
  uint32_t id;
  size_t staged;
  char aname[2 * holofan::CLIP_NAME_MAX + 1];
  St ast;
  {
    Lock l;
    d = fanlogic::openDecision(activeLocked(), s_sess.name, s_sess.size, uq.name, uq.size);
    id = s_sess.id;
    staged = s_feed.staged();
    fanlogic::jsonString(s_sess.name, aname, sizeof(aname));
    ast = s_sess.st;
  }
  if (d == fanlogic::OpenDecision::SAME) return sendOpened(r, id, staged, uq.size);
  if (d == fanlogic::OpenDecision::BUSY) {
    snprintf(s_json, sizeof(s_json), "{\"ok\":false,\"err\":\"busy\",\"name\":\"%s\",\"state\":\"%s\"}", aname,
             stName(ast));
    return sendJson(r, "409 Conflict", s_json);
  }

  if (!clipHooks().ready()) return sendErr(r, "503 Service Unavailable", "nomcu");
  const esp_partition_t* part = stagingPartition();
  if (!part) return sendErr(r, "503 Service Unavailable", "verify");
  uint8_t* work = (uint8_t*)heap_caps_malloc(2 * SPI_FRAME, MALLOC_CAP_DMA);
  if (!work) return sendErr(r, "500 Internal Server Error", "nomem");
  fanlogic::McuLink* link = clipHooks().lease(LEASE_WAIT_MS);
  if (!link) {
    heap_caps_free(work);
    return sendErr(r, "503 Service Unavailable", "busy");
  }
  scratch()[0] = '\0';                          // a failed list must not leave a stale one
  fanlogic::listClips(*link, scratch(), scratchSize());
  bool exists = fanlogic::listHas(scratch(), uq.name);
  clipHooks().release(false);
  if (exists) {
    heap_caps_free(work);
    return sendErr(r, "409 Conflict", "exists");
  }

  uint32_t newId = esp_random() & 0x7fffffff;
  if (newId == 0) newId = 1;
  bool ringOk;
  {
    Lock l;
    s_flash.use(part);
    ringOk = s_ring.begin();
    if (ringOk) {
      s_work = work;
      s_feed.begin(uq.size, s_work, uq.pre, uq.dryMs);
      s_sess = Session();
      s_sess.st = St::PREFILL;
      s_sess.id = newId;
      strcpy(s_sess.name, uq.name);
      s_sess.size = uq.size;
      s_sess.spi = uq.spi;
      s_sess.pace = uq.pace;
      s_sess.pre = uq.pre;
      s_sess.dryMs = uq.dryMs;
      s_sess.discAtOpen = wifiDisconnects();
      s_active = true;
    }
  }
  if (!ringOk) {
    heap_caps_free(work);
    return sendErr(r, "500 Internal Server Error", "flash");
  }
  if (xTaskCreate(uploadTask, "upload", UPLOAD_STACK, nullptr, UPLOAD_PRIO, nullptr) != pdPASS) {
    {
      Lock l;
      s_feed.abort();
      s_sess.st = St::NONE;
      s_work = nullptr;
    }
    s_active = false;
    heap_caps_free(work);
    return sendErr(r, "500 Internal Server Error", "nomem");
  }
  ESP_LOGI(TAG, "open %s: %u bytes, id %lu, staging in %s (%u frames)", uq.name, (unsigned)uq.size,
           (unsigned long)newId, part->label, (unsigned)s_ring.capacity());
  return sendOpened(r, newId, 0, uq.size);
}

// PUT /api/upload?id=&off=: one chunk. Every error reply
// closes the socket (ESP_FAIL): the body is unread or the link is bad.
esp_err_t handleChunk(httpd_req_t* r) {
  char q[96] = "";
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_ERR_HTTPD_RESULT_TRUNC) {
    sendErr(r, "400 Bad Request", "query");
    return ESP_FAIL;
  }
  fanlogic::UploadQuery uq;
  fanlogic::QueryErr qe = fanlogic::parseUploadQuery(fanlogic::UploadReq::CHUNK, q, uq);
  if (qe != fanlogic::QueryErr::OK) {
    sendErr(r, "400 Bad Request", fanlogic::queryErrName(qe));
    return ESP_FAIL;
  }
  const size_t len = r->content_len;
  enum class V { GO, NOID, ENDED, GAP, RANGE } v;
  size_t staged;
  St st;
  {
    Lock l;
    staged = s_feed.staged();
    st = s_sess.st;
    if (st == St::NONE || uq.id != s_sess.id) v = V::NOID;
    else if (!activeLocked()) v = V::ENDED;
    else if (uq.off > staged) { v = V::GAP; s_sess.gap++; }
    else if (len > s_sess.size || uq.off > s_sess.size - len) v = V::RANGE;
    else {
      v = V::GO;
      s_sess.req++;
      // lwIP reuses the lowest free fd, so tag the connection itself: httpd keeps
      // sess_ctx for the life of the socket (free_ctx is a no-op: it is not heap).
      if (!r->sess_ctx) { r->sess_ctx = (void*)++s_connSeq; r->free_ctx = keepCtx; }
      uintptr_t conn = (uintptr_t)r->sess_ctx;
      if (s_sess.lastConn && conn != s_sess.lastConn) s_sess.recon++;
      s_sess.lastConn = conn;
    }
  }
  if (v == V::NOID) { sendErr(r, "404 Not Found", "id"); return ESP_FAIL; }
  if (v == V::RANGE) { sendErr(r, "400 Bad Request", "length"); return ESP_FAIL; }
  if (v == V::GAP || v == V::ENDED) {
    if (v == V::GAP) snprintf(s_json, sizeof(s_json), "{\"ok\":false,\"err\":\"offset\",\"staged\":%u}", (unsigned)staged);
    else snprintf(s_json, sizeof(s_json), "{\"ok\":false,\"err\":\"state\",\"state\":\"%s\",\"staged\":%u}", stName(st),
                  (unsigned)staged);
    sendJson(r, "409 Conflict", s_json);
    return ESP_FAIL;
  }

  setRecvTimeout(r, BODY_RECV_TIMEOUT_S);
  size_t pos = uq.off, left = len;
  uint32_t quietFrom = s_clock.nowMs();
  bool ended = false, stalled = false, broken = false;
  while (left > 0 && !ended && !stalled) {
    int n = httpd_req_recv(r, (char*)s_rx, left < sizeof(s_rx) ? left : sizeof(s_rx));
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
      if ((uint32_t)(s_clock.nowMs() - quietFrom) >= fanlogic::CHUNK_STALL_MS) stalled = true;
      continue;
    }
    if (n <= 0) { broken = true; break; }   // 0 = closed by the client
    left -= (size_t)n;
    size_t done = 0;
    const uint32_t spaceFrom = s_clock.nowMs();
    while (done < (size_t)n) {
      size_t used;
      bool over;
      xSemaphoreTake(s_gate, portMAX_DELAY);                // not while a window is on the wire
      {
        Lock l;
        used = s_feed.write(pos, s_rx + done, (size_t)n - done);
        over = s_feed.state() == BodyState::FAILED || !activeLocked();
      }
      xSemaphoreGive(s_gate);
      pos += used;
      done += used;
      if (done == (size_t)n) break;
      if (over) { ended = true; break; }
      if ((uint32_t)(s_clock.nowMs() - spaceFrom) >= fanlogic::CHUNK_STALL_MS) { stalled = true; break; }
      vTaskDelay(pdMS_TO_TICKS(10));        // the ring is full: let the upload task send
    }
    quietFrom = s_clock.nowMs();
  }
  setRecvTimeout(r, RECV_TIMEOUT_S);
  {
    Lock l;
    staged = s_feed.staged();
    st = s_sess.st;
    if (stalled) s_sess.stall++;
  }
  if (!ended && !stalled && !broken) {
    snprintf(s_json, sizeof(s_json), "{\"ok\":true,\"staged\":%u}", (unsigned)staged);
    return sendJson(r, "200 OK", s_json);
  }
  if (ended) snprintf(s_json, sizeof(s_json), "{\"ok\":false,\"err\":\"state\",\"state\":\"%s\",\"staged\":%u}", stName(st),
                      (unsigned)staged);
  else snprintf(s_json, sizeof(s_json), "{\"ok\":false,\"err\":\"recv\",\"staged\":%u}", (unsigned)staged);
  sendJson(r, ended ? "409 Conflict" : "408 Request Timeout", s_json);
  return ESP_FAIL;
}

// GET /api/upload: session status.
esp_err_t handleStatus(httpd_req_t* r) {
  sampleRssi();
  Session s;
  size_t staged, ring, dup, rmin;
  uint32_t stg, quiet, discNow = wifiDisconnects();
  {
    Lock l;
    s = s_sess;
    staged = s_feed.staged();
    ring = s_feed.count();
    dup = s_feed.dupBytes();
    quiet = s_feed.longestQuietMs();
    rmin = s_feed.ringMin();
    stg = s_feed.stagingRate();
  }
  char name[2 * holofan::CLIP_NAME_MAX + 1];
  fanlogic::jsonString(s.name, name, sizeof(name));
  bool live = s.st == St::PREFILL || s.st == St::SENDING;
  snprintf(s_json, sizeof(s_json),
           "{\"state\":\"%s\",\"id\":%lu,\"name\":\"%s\",\"size\":%u,\"staged\":%u,\"ring\":%u,\"sent\":%lu,"
           "\"err\":\"%s\",\"partial\":%s,\"link\":{\"req\":%lu,\"recon\":%lu,\"dup\":%u,\"gap\":%lu,"
           "\"stall\":%lu,\"quiet\":%lu,\"ringmin\":%d,\"stg\":%lu,\"rssi\":[%d,%d],\"disc\":%lu,\"stack\":%lu,\"heap\":%u,\"hmin\":%u},"
           "\"pace\":{\"mode\":\"%s\",\"igap\":%lu,\"keep\":%lu,\"pre\":%u,\"dry\":%lu}}",
           stName(s.st), (unsigned long)s.id, name, (unsigned)s.size, (unsigned)staged, (unsigned)ring,
           (unsigned long)s.sent, s.st == St::FAILED ? fanlogic::clipErrName(s.err) : "",
           s.partial ? "true" : "false", (unsigned long)s.req, (unsigned long)s.recon, (unsigned)dup,
           (unsigned long)s.gap, (unsigned long)s.stall, (unsigned long)quiet, rmin == SIZE_MAX ? -1 : (int)rmin, (unsigned long)stg,
           s.rssiMin, s.rssiMax, (unsigned long)((live ? discNow : s.discAtEnd) - s.discAtOpen),
           (unsigned long)s.stackFree, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT), paceName(s.pace.pace),
           (unsigned long)s.pace.igapMs, (unsigned long)s.pace.keepMs, (unsigned)s.pre, (unsigned long)s.dryMs);
  return sendJson(r, "200 OK", s_json);
}

// DELETE /api/upload[?id=]: abort.
esp_err_t handleAbort(httpd_req_t* r) {
  char q[64] = "";
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_ERR_HTTPD_RESULT_TRUNC)
    return sendErr(r, "400 Bad Request", "query");
  fanlogic::UploadQuery uq;
  fanlogic::QueryErr qe = fanlogic::parseUploadQuery(fanlogic::UploadReq::ABORT, q, uq);
  if (qe != fanlogic::QueryErr::OK) return sendErr(r, "400 Bad Request", fanlogic::queryErrName(qe));
  bool wrongId;
  St st;
  {
    Lock l;
    wrongId = uq.hasId && uq.id != s_sess.id;
    if (!wrongId && activeLocked()) s_feed.abort();
    st = s_sess.st;
  }
  if (wrongId) return sendErr(r, "404 Not Found", "id");
  ESP_LOGW(TAG, "abort requested (%s)", stName(st));
  char body[48];
  snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"%s\"}", stName(st));
  return sendJson(r, "200 OK", body);
}

}  // namespace

void uploadRegister(httpd_handle_t http) {
  if (!s_mu) s_mu = xSemaphoreCreateMutex();
  if (!s_gate) s_gate = xSemaphoreCreateMutex();
  static const httpd_uri_t uris[] = {
    {"/api/upload", HTTP_POST, handleOpen, nullptr},
    {"/api/upload", HTTP_PUT, handleChunk, nullptr},
    {"/api/upload", HTTP_GET, handleStatus, nullptr},
    {"/api/upload", HTTP_DELETE, handleAbort, nullptr},
  };
  for (const httpd_uri_t& u : uris) httpd_register_uri_handler(http, &u);
}

bool uploadActive() { return s_active; }

}  // namespace maintidf
