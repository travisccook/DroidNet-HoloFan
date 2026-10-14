#include "McuSpi.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "spi";

static constexpr int PIN_MOSI = 7;
static constexpr int PIN_MISO = 2;
static constexpr int PIN_SCLK = 6;
static constexpr int PIN_CS_HW = 0;
static constexpr int PIN_CS_MANUAL = 10;
static constexpr int PIN_SYNC = 18;

// "pll" is what every upload before 0.7.2 used: SPI_CLK_SRC_DEFAULT, which
// is PLL_F40M on the ESP32-C2 (soc/esp32c2/include/soc/clk_tree_defs.h).
static_assert(SPI_CLK_SRC_DEFAULT == SPI_CLK_SRC_PLL_F40M, "pll must stay the IDF default source");

static void IRAM_ATTR csLow(spi_transaction_t*) { gpio_set_level((gpio_num_t)PIN_CS_MANUAL, 0); }
static void IRAM_ATTR csHigh(spi_transaction_t*) { gpio_set_level((gpio_num_t)PIN_CS_MANUAL, 1); }

bool McuSpi::begin(const fanlogic::SpiConfig& cfg) {
  if (dev_ && cfg == cfg_) return true;
  if (dev_) {
    // Another clock for this upload. Polling transactions only, all
    // finished, so the device can go (gpio_reset_pin leaves its CS pin
    // GPIO0 disabled with the pull-up on until it is added again; the
    // manual CS GPIO10 is not touched).
    esp_err_t err = spi_bus_remove_device(dev_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "remove device: %s", esp_err_to_name(err));
      return false;
    }
    dev_ = nullptr;
    actualKhz_ = 0;
  }
  if (!busUp_) {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = PIN_MOSI;
    bus.miso_io_num = PIN_MISO;
    bus.sclk_io_num = PIN_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.data4_io_num = -1;
    bus.data5_io_num = -1;
    bus.data6_io_num = -1;
    bus.data7_io_num = -1;
    bus.max_transfer_sz = (int)fanlogic::SPI_WINDOW;
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "bus init: %s", esp_err_to_name(err));
      return false;
    }
    busUp_ = true;
  }
  spi_device_interface_config_t dev = {};
  dev.mode = 0;
  // Both sources are 40 MHz here (PLL_F40M; XTAL per CONFIG_XTAL_FREQ=40),
  // and 10/20/40 MHz divide it exactly (spi_ll_master_cal_clock).
  dev.clock_source = cfg.clk == fanlogic::SpiClk::XTAL ? SPI_CLK_SRC_XTAL : SPI_CLK_SRC_DEFAULT;
  dev.clock_speed_hz = (int)cfg.hz;
  dev.spics_io_num = PIN_CS_HW;
  dev.queue_size = 1;
  dev.pre_cb = csLow;
  dev.post_cb = csHigh;
  esp_err_t err = spi_bus_add_device(SPI2_HOST, &dev, &dev_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "add device: %s", esp_err_to_name(err));
    dev_ = nullptr;
    return false;
  }
  cfg_ = cfg;
  if (spi_device_get_actual_freq(dev_, &actualKhz_) != ESP_OK) actualKhz_ = 0;
  ESP_LOGI(TAG, "device: %d kHz from %s", actualKhz_, fanlogic::spiClkName(cfg.clk));
  return true;
}

bool McuSpi::SyncPin::high() { return gpio_get_level((gpio_num_t)PIN_SYNC) != 0; }
void McuSpi::SyncPin::yield() { taskYIELD(); }
void McuSpi::SyncPin::delayMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

bool McuSpi::beforeFrame() {
  bool ok = sync_.beforeFrame();
  if (!ok) ESP_LOGW(TAG, "GPIO18 credit: never HIGH, pending=%d", sync_.pending());
  return ok;
}

bool McuSpi::sendFrame(const uint8_t* buf, size_t len) {
  int64_t now = esp_timer_get_time();
  if (lastFrameUs_ == 0) {
    firstMs_ = (uint32_t)((now - resetUs_) / 1000);
  } else {
    uint32_t gap = (uint32_t)((now - lastFrameUs_) / 1000);
    if (inWindow_ > 0 && gap > gapMs_) gapMs_ = gap;
    if (gap > worstGapMs_) worstGapMs_ = gap;
  }
  lastFrameUs_ = now;
  inWindow_++;
  spi_transaction_t t = {};
  t.length = len * 8;                        // bits
  t.tx_buffer = buf;
  return spi_device_polling_transmit(dev_, &t) == ESP_OK;
}

bool McuSpi::waitWindow() {
  int64_t t0 = esp_timer_get_time();
  bool ok = sync_.afterWindow();
  lastUs_ = (uint32_t)(esp_timer_get_time() - t0);
  if (ok) { windows_++; inWindow_ = 0; gapMs_ = 0; }
  else ESP_LOGW(TAG, "GPIO18 wait: spin cap, low=%d", sync_.lastLow());
  return ok;
}

void McuSpi::resetStats() {
  windows_ = 0;
  sync_.reset();
  lastUs_ = 0;
  gapMs_ = worstGapMs_ = firstMs_ = 0;
  lastFrameUs_ = 0;
  inWindow_ = 0;
  resetUs_ = esp_timer_get_time();
}

void McuSpi::stats(char* out, size_t size) const {
  snprintf(out, size, "win=%d low=%d it=%d us=%lu first=%lu gap=%lu worst=%lu hiw=%d spi=%dk/%s", windows_,
           sync_.lastLow(), sync_.lastSpins(), (unsigned long)lastUs_, (unsigned long)firstMs_, (unsigned long)gapMs_,
           (unsigned long)worstGapMs_, sync_.highWaits(), actualKhz_, fanlogic::spiClkName(cfg_.clk));
}
