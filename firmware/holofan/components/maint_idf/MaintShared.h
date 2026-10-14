// Shared by MaintIdf.cpp and UploadIdf.cpp; not part of the component's API.
#ifndef MAINT_SHARED_H
#define MAINT_SHARED_H
#include <stddef.h>
#include <stdint.h>
#include "ClipOps.h"
#include "MaintIdf.h"
#include "esp_http_server.h"
#include "esp_partition.h"
#include "esp_timer.h"

namespace maintidf {

constexpr uint32_t LEASE_WAIT_MS = 5000;
constexpr int      RECV_TIMEOUT_S = 10;       // httpd's per-socket receive timeout
// Clip bodies: the socket's receive timeout is cut to this, so a Wi-Fi stall
// returns "nothing yet" about once a second and the caller decides.
constexpr int      BODY_RECV_TIMEOUT_S = 1;

esp_err_t sendJson(httpd_req_t* r, const char* status, const char* body);
void setRecvTimeout(httpd_req_t* r, int seconds);
// "<op> <name> <err> f=<frames> <diag>" into /api/info "last" (JSON-safe).
void noteLast(const char* op, const char* name, fanlogic::ClipErr e, size_t frames, const char* diag);
const ClipHooks& clipHooks();
// The 8 KB "s" reply buffer: the HTTP task, or the upload task while a
// session runs (every other user of it is refused then).
char* scratch();
size_t scratchSize();
// The spare OTA slot, if staging may use it (null while a fresh OTA image
// still proves itself).
const esp_partition_t* stagingPartition();
// Wi-Fi disconnects since maintenance started (WIFI_EVENT_STA_DISCONNECTED).
uint32_t wifiDisconnects();
// UploadIdf.cpp: register POST/PUT/GET/DELETE /api/upload.
void uploadRegister(httpd_handle_t http);

class EspClock : public fanlogic::Clock {
 public:
  uint32_t nowMs() override { return (uint32_t)(esp_timer_get_time() / 1000); }
};

// The spare OTA slot as a frame ring's flash. Every call is timed (bench:
// fl= in "last"): total erase and write time, longest single call.
class PartitionFlash : public fanlogic::FlashIo {
 public:
  PartitionFlash() = default;
  explicit PartitionFlash(const esp_partition_t* p) : p_(p) {}
  void use(const esp_partition_t* p) { p_ = p; eraseUs_ = writeUs_ = longestUs_ = 0; }
  size_t size() const override { return p_ ? p_->size : 0; }
  bool erase(size_t off, size_t len) override {
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = esp_partition_erase_range(p_, off, len);
    eraseUs_ += took(t0);
    return e == ESP_OK;
  }
  bool write(size_t off, const uint8_t* buf, size_t len) override {
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = esp_partition_write(p_, off, buf, len);
    writeUs_ += took(t0);
    return e == ESP_OK;
  }
  bool read(size_t off, uint8_t* buf, size_t len) override {
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = esp_partition_read(p_, off, buf, len);
    took(t0);
    return e == ESP_OK;
  }
  unsigned long eraseMs() const { return (unsigned long)(eraseUs_ / 1000); }
  unsigned long writeMs() const { return (unsigned long)(writeUs_ / 1000); }
  unsigned long longestMs() const { return (unsigned long)(longestUs_ / 1000); }

 private:
  int64_t took(int64_t t0) {
    int64_t us = esp_timer_get_time() - t0;
    if (us > longestUs_) longestUs_ = us;
    return us;
  }
  const esp_partition_t* p_ = nullptr;
  int64_t eraseUs_ = 0, writeUs_ = 0, longestUs_ = 0;
};

}  // namespace maintidf
#endif
