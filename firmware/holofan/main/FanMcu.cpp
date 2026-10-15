#include "FanMcu.h"
#include <atomic>
#include <stdio.h>
#include <string.h>
#include "Console.h"
#include "FanMesh.h"
#include "FanController.h"
#include "McuSpi.h"
#include "McuUart.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static constexpr int QUEUE_DEPTH = 8;

static QueueHandle_t     s_queue = nullptr;
static QueueHandle_t     s_raw = nullptr;       // char[fanlogic::MCU_LINE_MAX] console lines
struct VolReq { uint32_t id; int v; };
static QueueHandle_t     s_vol = nullptr;       // one VolReq (#HLFY), overwritten by a newer one
static uint32_t          s_volNextId = 0;       // mesh task only
static uint32_t          s_volDoneId = 0;       // guarded by s_lock
static int               s_volDone = 0;         // guarded by s_lock
static char              s_bootLine[CON_LINE_MAX + 1];   // guarded by s_lock
static bool              s_bootReady = false;   // guarded by s_lock: s_bootLine waits for the mesh
static char              s_latched[CON_LINE_MAX + 1] = "";   // guarded by s_lock: UNPROMPTED while off the mesh
static std::atomic<bool> s_answered{false};
static SemaphoreHandle_t s_lock = nullptr;
static holofan::Status   s_status;      // guarded by s_lock
static holofan::Catalog  s_catalog;     // guarded by s_lock
static char              s_diag[holofan::REPLY_MAX + 1] = "#HL:FANDIAG st=BOOT";   // guarded by s_lock
static McuUart           s_link;
static fanlogic::FanController s_fan(s_link);
static McuSpi            s_spi;
static char              s_trace[1536] = "";   // the last clip lease's MCU conversation (0.8.6: was 3 KB; heap is tight)
static SemaphoreHandle_t s_linkLock = nullptr;   // the MCU task per iteration, or a clip lease
static std::atomic<bool> s_linkUp{false};

static uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

static int s_rxHigh = -1, s_syncHigh = -1;   // of 20 samples, refreshed every few seconds
static int s_rxDriven = -1, s_syncDriven = -1;  // same, with a pull-down instead of the pull-up

static void publish() {
  char last[48], diag[sizeof(s_diag)];
  s_link.lastRx(last, sizeof(last));
  snprintf(diag, sizeof(diag),
           "#HL:FANDIAG st=%s rxb=%lu tx=%lu tries=%lu ans=%d rx5=%d/%d sync18=%d/%d rst=%d chg=%lu v0=%d txp=%d last=%s",
           s_fan.status().state == holofan::FanState::READY ? "READY" : "NOMCU", (unsigned long)s_link.rxBytes(),
           (unsigned long)s_link.txLines(), (unsigned long)s_fan.queryTries(), s_fan.mcuAnswered() ? 1 : 0, s_rxHigh,
           s_rxDriven, s_syncHigh, s_syncDriven, (int)esp_reset_reason(), (unsigned long)s_fan.unprompted(),
           s_fan.bootVolume(), fanMeshTxDbm(), last);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status = s_fan.status();
  s_catalog = s_fan.catalog();
  strcpy(s_diag, diag);
  xSemaphoreGive(s_lock);
}

static void report(holofan::FanState& last) {
  const holofan::Status& s = s_fan.status();
  if (s.state == last) return;
  last = s.state;
  if (s.state == holofan::FanState::READY)
    con("[fan] MCU READY: %d clips, br=%d vol=%d", s_fan.catalog().count, s.brightness, s.volume);
  else
    con("[fan] MCU not answering (NOMCU): playback commands are dropped");
}

// Split "<cmd> [a0 [a1 [a2]]]" on spaces, frame it with buildLine (which
// refuses `v` and unsafe bytes) and print the MCU's raw reply, escaped.
static void runRaw(char* typed) {
  const char* args[3] = {nullptr, nullptr, nullptr};
  int argc = 0;
  char* save = nullptr;
  char* cmd = strtok_r(typed, " ", &save);
  for (char* t = strtok_r(nullptr, " ", &save); t && argc < 3; t = strtok_r(nullptr, " ", &save)) args[argc++] = t;
  char line[fanlogic::MCU_LINE_MAX];
  if (!cmd || fanlogic::buildLine(cmd, args, argc, line, sizeof(line)) < 0) {
    con("[mcu] refused (empty, unsafe, or v)");
    return;
  }
  static char reply[1024];
  int n = s_link.transact(line, nullptr, 2000, reply, sizeof(reply));
  char out[CON_LINE_MAX + 1];
  size_t o = 0;
  for (int i = 0; i < n && o + 4 < sizeof(out); i++) {
    unsigned char c = (unsigned char)reply[i];
    if (c == '\n') { out[o++] = '|'; }
    else if (c >= 0x20 && c < 0x7f) out[o++] = (char)c;
    else o += (size_t)snprintf(out + o, sizeof(out) - o, "\\x%02x", c);
  }
  out[o] = '\0';
  con("[mcu] %d bytes: %s", n, n > 0 ? out : "<no reply>");
}

// #HLFY: the timed MCU conversation of one checked volume change, as
// "[mcu] vt: ..." lines (newlines shown as |), so a busy or silent MCU
// shows on RTERM.
static void printVolumeTrace(const char* t) {
  constexpr size_t CHUNK = 140;
  char out[CHUNK + 1];
  size_t o = 0;
  for (const char* p = t; *p; p++) {
    unsigned char c = (unsigned char)*p;
    out[o++] = c == '\n' ? '|' : (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    if (o == CHUNK) { out[o] = '\0'; con("[mcu] vt: %s", out); o = 0; }
  }
  if (o) { out[o] = '\0'; con("[mcu] vt: %s", out); }
}

// An "[mcu] <label><text>" line now, or kept for the first command back on
// the mesh (e.g. after an upload in maintenance).
static void emitOrLatch(const char* label, const char* text) {
  if (fanMeshCanSend()) { con("[mcu] %s%s", label, text); return; }
  printf("[mcu] %s%s (latched)\n", label, text);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  size_t n = strlen(s_latched);
  snprintf(s_latched + n, sizeof(s_latched) - n, "%s%s%s", n ? "; " : "", label, text);
  xSemaphoreGive(s_lock);
}

// 0.9.8: what nobody here commanded, and what the MCU said at connect.
static void reportMcuState() {
  static bool bootFormatted = false;
  char text[fanlogic::UNPROMPTED_MAX];
  if (s_fan.takeUnprompted(text, sizeof(text))) emitOrLatch("UNPROMPTED: ", text);
  if (s_fan.takeLeaseReport(text, sizeof(text))) emitOrLatch("after a clip lease: ", text);
  if ((!bootFormatted && s_fan.bootDone()) || (bootFormatted && s_fan.takeBootLate())) {
    char line[CON_LINE_MAX + 1];
    if (s_fan.formatBoot(line, sizeof(line)) > 0) {
      printf("%s\n", line);                       // UART0 now; RTERM at the first mesh command
      xSemaphoreTake(s_lock, portMAX_DELAY);
      strcpy(s_bootLine, line);
      s_bootReady = true;
      xSemaphoreGive(s_lock);
    }
    bootFormatted = true;
  }
}

static void mcuTask(void*) {
  holofan::FanState last = holofan::FanState::NOMCU;
  if (!s_link.begin()) {
    con("[fan] UART1 init failed: MCU unreachable");
    s_fan.markNoLink();             // so #HLFS says err=nouart instead of a bare NOMCU
    publish();
    vTaskDelete(nullptr);
    return;
  }
  s_linkUp = true;
  con("[fan] UART1 230400 up; waking the MCU");
  xSemaphoreTake(s_linkLock, portMAX_DELAY);
  s_fan.begin(nowMs());
  xSemaphoreGive(s_linkLock);
  if (s_fan.status().state == holofan::FanState::NOMCU) con("[fan] MCU never answered get_setting (NOMCU)");
  report(last);
  publish();
  uint32_t nextBeat = nowMs() + McuUart::HEARTBEAT_PERIOD_MS;
  for (;;) {
    holofan::Cmd c;
    bool got = xQueueReceive(s_queue, &c, pdMS_TO_TICKS(50)) == pdTRUE;
    xSemaphoreTake(s_linkLock, portMAX_DELAY);   // blocks while a clip transfer holds the link
    if (got) s_fan.execute(c, nowMs());
    char typed[fanlogic::MCU_LINE_MAX];
    if (xQueueReceive(s_raw, typed, 0) == pdTRUE) {
      bool changes = fanlogic::rawLineMayChange(typed);   // before runRaw: it splits the line in place
      runRaw(typed);
      if (changes) s_fan.forgetBaseline();       // an action may have changed anything; queries keep the baseline
    }
    VolReq vr;
    if (xQueueReceive(s_vol, &vr, 0) == pdTRUE) {
      static char vtrace[640];
      s_link.traceStart(vtrace, sizeof(vtrace));
      int got = s_fan.setVolumeChecked(vr.v);
      s_link.traceStop();
      con("[mcu] volume %d requested, MCU reports %d", vr.v, got);
      printVolumeTrace(vtrace);
      xSemaphoreTake(s_lock, portMAX_DELAY);
      s_volDoneId = vr.id;
      s_volDone = got;
      xSemaphoreGive(s_lock);
    }
    uint32_t now = nowMs();
    if ((int32_t)(now - nextBeat) >= 0) {
      s_rxHigh = s_link.highSamples(McuUart::PIN_RX_LINE, 20);
      s_syncHigh = s_link.highSamples(McuUart::PIN_SYNC_LINE, 20);
      s_rxDriven = s_link.highSamplesPulledDown(McuUart::PIN_RX_LINE, 20);
      s_syncDriven = s_link.highSamplesPulledDown(McuUart::PIN_SYNC_LINE, 20);
      s_link.heartbeat();
      nextBeat = now + McuUart::HEARTBEAT_PERIOD_MS;
    }
    s_fan.poll(nowMs());
    char ml[fanlogic::McuLogLines::MAX_CHARS + 1];
    while (s_link.takeLog(ml, sizeof(ml))) con("[mcu] %s", ml);   // what the MCU says by itself (0.9.2)
    xSemaphoreGive(s_linkLock);
    reportMcuState();
    report(last);
    if (s_fan.mcuAnswered()) s_answered = true;
    publish();
  }
}

void fanMcuStart() {
  s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(holofan::Cmd));
  s_raw = xQueueCreate(2, fanlogic::MCU_LINE_MAX);
  s_vol = xQueueCreate(1, sizeof(VolReq));
  s_lock = xSemaphoreCreateMutex();
  s_linkLock = xSemaphoreCreateMutex();
  xTaskCreate(mcuTask, "mcu", 6144, nullptr, 4, nullptr);
}

bool fanMcuPost(const holofan::Cmd& c) {
  return s_queue && xQueueSend(s_queue, &c, 0) == pdTRUE;
}

void fanMcuSnapshot(holofan::Status* status, holofan::Catalog* catalog) {
  if (!s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (status) *status = s_status;
  if (catalog) *catalog = s_catalog;
  xSemaphoreGive(s_lock);
}

bool fanMcuAnswered() { return s_answered; }

void fanMcuHoldForRestart() { s_link.holdForRestart(); }

void fanMcuDiag(char* out, size_t size) {
  if (!s_lock || size == 0) { if (size) out[0] = '\0'; return; }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  strncpy(out, s_diag, size - 1);
  out[size - 1] = '\0';
  xSemaphoreGive(s_lock);
}

bool fanMcuRaw(const char* line) {
  char buf[fanlogic::MCU_LINE_MAX];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  return s_raw && xQueueSend(s_raw, buf, 0) == pdTRUE;
}

uint32_t fanMcuVolumeChecked(int v) {
  if (!s_vol) return 0;
  if (++s_volNextId == 0) s_volNextId = 1;
  VolReq r = {s_volNextId, v};
  xQueueOverwrite(s_vol, &r);
  return r.id;
}

int fanMcuVolumeResult(uint32_t id) {
  if (!s_lock) return -1;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  int v = s_volDoneId == id ? s_volDone : -1;
  xSemaphoreGive(s_lock);
  return v;
}

bool fanMcuReady() {
  holofan::Status s;
  fanMcuSnapshot(&s, nullptr);
  return s_linkUp && s.state == holofan::FanState::READY;
}

fanlogic::McuLink* fanMcuLease(uint32_t timeoutMs) {
  if (!s_linkUp || !s_linkLock) return nullptr;
  if (xSemaphoreTake(s_linkLock, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return nullptr;
  s_fan.beginLease();                              // what to put back afterwards (0.9.8; the MCU task waits on the lock)
  s_link.releaseCs();                              // the SPI callbacks drive GPIO10 during the lease
  s_link.traceStart(s_trace, sizeof(s_trace));
  return &s_link;
}

const char* fanMcuTrace() { return s_trace; }

bool fanMcuTakeBootLine(char* out, size_t size) {
  if (!s_lock || size == 0) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool ready = s_bootReady;
  if (ready) {
    strncpy(out, s_bootLine, size - 1);
    out[size - 1] = '\0';
    s_bootReady = false;
  }
  xSemaphoreGive(s_lock);
  return ready;
}

bool fanMcuTakeLatchedUnprompted(char* out, size_t size) {
  if (!s_lock || size == 0) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool any = s_latched[0] != '\0';
  if (any) {
    strncpy(out, s_latched, size - 1);
    out[size - 1] = '\0';
    s_latched[0] = '\0';
  }
  xSemaphoreGive(s_lock);
  return any;
}

void fanMcuRelease(bool refresh) {
  s_fan.endLease(refresh);                         // refresh = the lease touched the MCU: put back what changed (0.9.8)
  s_link.traceStop();
  s_link.parkCs();
  xSemaphoreGive(s_linkLock);
  if (refresh) fanMcuPost(holofan::Cmd(holofan::CmdType::CATALOG_REFRESH, 0));
}

fanlogic::ClipSpi* fanMcuSpi(const fanlogic::SpiConfig& cfg) {
  if (!s_spi.begin(cfg)) return nullptr;
  s_spi.resetStats();
  return &s_spi;
}

void fanMcuClipDiag(char* out, size_t size) {
  char spi[160], rx[48];
  s_spi.stats(spi, sizeof(spi));
  s_link.lastRx(rx, sizeof(rx));
  snprintf(out, size, "%s rx=%s", spi, rx);
}
