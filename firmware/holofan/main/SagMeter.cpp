#include "SagMeter.h"
#include <atomic>
#include "Console.h"
#include "SagLog.h"
#include "esp_attr.h"
#include "esp_private/panic_internal.h"
#include "esp_rom_regi2c.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/brownout_ll.h"
#include "sdkconfig.h"
#include "soc/regi2c_brownout.h"
#include "soc/rtc_cntl_struct.h"

static fanlogic::SagLog   s_log;              // the meter task only, while s_running
static fanlogic::ProbeLog s_probe;            // the probe task only, while s_running
static bool (*s_send)(const uint8_t*, size_t) = nullptr;
static std::atomic<bool>  s_running{false};
static volatile bool      s_holding = false;  // the threshold may be above the reset level (read by the panic wrapper)
static volatile bool      s_stopping = false; // a restart is under way: never raise the threshold again
static volatile bool      s_probe_on = false;
static TaskHandle_t       s_task = nullptr;
static constexpr uint32_t SETTLE_US = 20;     // after a threshold change, before clearing the latch
static constexpr uint32_t PROBE_WINDOW_MS = 10;
static constexpr uint32_t PROBE_PERIOD_MS = 100;

// The threshold above 2.51 V must not reset the chip, close the flash or
// power the RF down at every small dip (IDF's esp_brownout_init enables all
// three in interrupt mode).
static void takeHardware() {
  s_holding = true;
  brownout_ll_intr_enable(false);
  brownout_ll_enable_flash_suspend(false);
  brownout_ll_enable_rf_power_down(false);
}

// Back to esp_brownout_init's configuration: CONFIG level, flash close, RF
// power-down, interrupt (whose handler resets the chip).
static void giveHardware() {
  brownout_ll_set_threshold(CONFIG_ESP_BROWNOUT_DET_LVL);
  esp_rom_delay_us(SETTLE_US);
  brownout_ll_intr_clear();
  brownout_ll_enable_flash_suspend(true);
  brownout_ll_enable_rf_power_down(true);
  brownout_ll_intr_enable(true);
  s_holding = false;
}

static void setLevel(int level) {
  if (s_stopping) return;
  brownout_ll_set_threshold((uint8_t)level);
  esp_rom_delay_us(SETTLE_US);
  brownout_ll_intr_clear();
}

// Read and clear the latched event: the supply went under the threshold since the last clear.
static bool takeLatch() {
  bool hit = RTCCNTL.int_raw.rtc_brown_out;
  if (hit) brownout_ll_intr_clear();
  return hit;
}

// A software restart is a CPU reset on the C2: the analog threshold stays,
// and the bootloader arms the direct brownout reset (mode1) at it before the
// app's esp_brownout_init rewrites it (review of 0.9.6). esp_restart() runs
// this first.
static void onShutdown() {
  s_stopping = true;                             // review of 0.9.8: the instrument (priority 7) must not raise it again
  if (s_task && s_task != xTaskGetCurrentTaskHandle()) vTaskSuspend(s_task);
  if (s_holding) brownout_ll_set_threshold(CONFIG_ESP_BROWNOUT_DET_LVL);   // the reset is imminent: threshold only
}

// Panics and the interrupt watchdog: IDF's documented hook (build with
// -Wl,--wrap=esp_panic_handler). The ROM write takes no lock (the panic may
// have interrupted the locked one); the rest is reset by the next boot.
extern "C" void __real_esp_panic_handler(panic_info_t* info);
extern "C" void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t* info) {
  if (s_holding)
    esp_rom_regi2c_write_mask(I2C_BOD, I2C_BOD_HOSTID, I2C_BOD_THRESHOLD, I2C_BOD_THRESHOLD_MSB, I2C_BOD_THRESHOLD_LSB,
                              CONFIG_ESP_BROWNOUT_DET_LVL);
  __real_esp_panic_handler(info);
}

static void finish() {
  giveHardware();
  con("[sag] done: brownout reset at level %d again", CONFIG_ESP_BROWNOUT_DET_LVL);
  s_task = nullptr;
  s_running = false;
  vTaskDelete(nullptr);
}

static void meterTask(void*) {
  takeHardware();
  int cur = -1;
  int64_t t0 = esp_timer_get_time();
  for (;;) {
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (s_log.done(ms)) break;
    int lvl = s_log.levelAt(ms);
    if (lvl != cur) {                           // new level: the latch starts clean, no sample this tick
      setLevel(lvl);
      cur = lvl;
    } else {
      bool det = RTCCNTL.brown_out.det;         // undocumented; counted apart
      s_log.sample(ms, lvl, takeLatch(), det);
    }
    vTaskDelay(1);                              // 1 ms (CONFIG_FREERTOS_HZ 1000)
  }
  char line[CON_LINE_MAX + 1];
  if (s_log.formatSummary(line, sizeof(line)) > 0) con("%s", line);
  for (int i = 0; i < s_log.timelineLines(); i++)
    if (s_log.formatTimeline(i, line, sizeof(line)) > 0) con("%s", line);
  finish();
}

// Per level: PROBE_FRAMES times an idle window (the background) then a
// window with one probe frame in it, PROBE_PERIOD_MS apart.
static void probeTask(void*) {
  int8_t oldPower = 0;
  bool havePower = esp_wifi_get_max_tx_power(&oldPower) == ESP_OK;
  esp_err_t err = esp_wifi_set_max_tx_power((int8_t)(s_probe.dBm() * 4));
  if (err != ESP_OK) con("[probe] set_max_tx_power(%d dBm): %s", s_probe.dBm(), esp_err_to_name(err));
  static uint8_t frame[250];                    // zeros: no WCB accepts it (size and magic)
  takeHardware();
  for (int i = 0; i < s_probe.levels(); i++) {
    int lvl = s_probe.levelOf(i);
    setLevel(lvl);
    for (int n = 0; n < fanlogic::PROBE_FRAMES; n++) {
      takeLatch();
      vTaskDelay(pdMS_TO_TICKS(PROBE_WINDOW_MS));
      bool idle = takeLatch();
      bool sent = s_send && s_send(frame, sizeof(frame));
      vTaskDelay(pdMS_TO_TICKS(PROBE_WINDOW_MS));
      s_probe.frame(lvl, sent && takeLatch(), idle);
      vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS - 2 * PROBE_WINDOW_MS));
    }
  }
  giveHardware();
  if (havePower) {
    esp_err_t back = esp_wifi_set_max_tx_power(oldPower);
    if (back != ESP_OK) con("[probe] TX power NOT restored (%s): restart the fan", esp_err_to_name(back));
  }
  char line[CON_LINE_MAX + 1];
  if (s_probe.format(line, sizeof(line)) > 0) con("%s", line);
  con("[probe] done: TX power %d (0.25 dBm), brownout reset at level %d again", havePower ? oldPower : -1,
      CONFIG_ESP_BROWNOUT_DET_LVL);
  s_probe_on = false;
  s_task = nullptr;
  s_running = false;
  vTaskDelete(nullptr);
}

static bool startTask(TaskFunction_t fn, const char* name) {
  static bool registered = false;
  if (!registered) registered = esp_register_shutdown_handler(onShutdown) == ESP_OK;
  if (!registered) return false;               // no restart safety: do not touch the detector
  s_running = true;
  // Above the mesh task (6) so the 1 ms polls keep their pace.
  if (xTaskCreate(fn, name, 3072, nullptr, 7, &s_task) != pdPASS) {
    s_task = nullptr;
    s_running = false;
    return false;
  }
  return true;
}

bool sagStart(int param) {
  if (s_running) return false;
  if (!s_log.begin(param / 10000, param / 1000 % 10, param % 1000)) return false;
  return startTask(meterTask, "sag");
}

bool sagStartProbe(int param, bool (*send)(const uint8_t* data, size_t len)) {
  if (s_running || !send) return false;
  if (!s_probe.begin(param / 10000, param / 1000 % 10, param % 1000)) return false;
  s_send = send;
  s_probe_on = true;
  if (startTask(probeTask, "probe")) return true;
  s_probe_on = false;
  return false;
}

bool sagRunning() { return s_running; }

bool sagProbeRunning() { return s_running && s_probe_on; }
