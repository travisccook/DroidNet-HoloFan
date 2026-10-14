// SPI2 clip-upload path to the fan's LED/SD MCU, set up exactly as the stock
// firmware does it (docs/HOW_IT_WORKS.md; confirmed on hardware):
//   SPI2_HOST, MOSI 7 / MISO 2 / SCLK 6, max transfer 28800, DMA auto,
//   mode 0, 40 MHz, hardware CS GPIO0, manual CS GPIO10 low/high in the
//   pre/post callbacks (GPIO10 is set up by McuUart::begin()).
//   0.7.2: the clock (10/20/40 MHz) and its source (PLL_F40M, the IDF
//   default, or XTAL as stock) can be chosen per upload
//   (fanlogic::SpiConfig, PUT ?mhz=&clk=); the default is the above.
//   After each 20-frame window: count GPIO18 LOW samples, 100 -> ok, spin
//   cap 3,000,000 with a yield every 1024, at the cap ok iff >= 10 LOW.
//   Before every frame (0.7.2): the stock GPIO18 credit check, so at most
//   one window is outstanding while the MCU holds GPIO18 LOW
//   (fanlogic::SyncCredit; the wait for HIGH is capped, unlike stock).
//   Both GPIO18 steps also run for the zero tail, which stock sends blind,
//   20 ms apart (see fanlogic::SyncCredit).
// Initialised on first use, so boot and playback are unchanged. One task at
// a time (the holder of the FanMcu link lease).
#ifndef MCU_SPI_H
#define MCU_SPI_H
#include "ClipOps.h"
#include "driver/spi_master.h"

class McuSpi : public fanlogic::ClipSpi {
 public:
  // Idempotent for the same `cfg`. A device set up with a different one is
  // removed and added again (spi_bus_remove_device, spi_bus_add_device)
  // with only clock_speed_hz and clock_source changed. False if the SPI
  // driver could not be set up (retried next call).
  bool begin(const fanlogic::SpiConfig& cfg);
  bool beforeFrame() override;
  bool sendFrame(const uint8_t* buf, size_t len) override;
  bool waitWindow() override;
  // The last GPIO18 wait: LOW samples, spin iterations, microseconds, and
  // windows completed since begin of the current upload (resetStats());
  // hiw = frames that had to wait for GPIO18 HIGH (credit check); spi = the
  // device's clock as the driver set it (spi_device_get_actual_freq, kHz)
  // and its source.
  void stats(char* out, size_t size) const;
  // A new upload: statistics and the GPIO18 credit cleared.
  void resetStats();

 private:
  // GPIO18 for the credit check: gpio_get_level, taskYIELD, vTaskDelay.
  class SyncPin : public fanlogic::SyncLine {
   public:
    bool high() override;
    void yield() override;
    void delayMs(uint32_t ms) override;
  };
  bool busUp_ = false;
  spi_device_handle_t dev_ = nullptr;
  fanlogic::SpiConfig cfg_;      // dev_'s configuration
  int      actualKhz_ = 0;       // dev_'s real clock (0: no device yet)
  SyncPin  pin_;
  fanlogic::SyncCredit sync_{pin_};
  int      windows_ = 0;
  uint32_t lastUs_ = 0;
  // Bench: the longest pause between two frames inside the current window
  // (gapMs_, reset per window) and over the whole upload (worstGapMs_).
  // firstMs_: from resetStats() (just before the upload) to the first frame;
  // worstGapMs_ also counts the gaps across window boundaries.
  uint32_t gapMs_ = 0, worstGapMs_ = 0, firstMs_ = 0;
  int64_t  lastFrameUs_ = 0, resetUs_ = 0;
  int      inWindow_ = 0;
};

#endif
