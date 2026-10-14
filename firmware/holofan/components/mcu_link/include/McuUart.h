// The UART1 command link to the fan's LED/SD MCU and its GPIO lines, exactly
// as the stock firmware sets them up (docs/HOW_IT_WORKS.md; confirmed on
// hardware):
//   UART1 230400 8N1, TX GPIO4, RX GPIO5 (pull-up), rx buffer 2048
//   GPIO3  out, idle 1, ~20 ms low pulse every 2.5 s (heartbeat)
//   GPIO10 out, idle 1 (SPI manual CS for the clip upload, McuSpi)
//   GPIO18 in, pull-up (SPI handshake from the MCU)
// GPIO8 and GPIO9 (boot straps; GPIO9 is on the programming header) are
// never configured. Use from one task only (the MCU task).
#ifndef MCU_UART_H
#define MCU_UART_H
#include <atomic>
#include "FanController.h"

class McuUart : public fanlogic::McuLink {
 public:
  static constexpr uint32_t HEARTBEAT_PERIOD_MS = 2500;
  static constexpr uint32_t HEARTBEAT_LOW_MS = 20;

  // GPIO initial states first (as stock), then the UART driver. False if the
  // UART driver could not be installed.
  bool begin();
  // Holds TX (GPIO4), GPIO3 and GPIO10 at their idle-high level (gpio_hold_en,
  // which survives a chip or system reset). In the droid the MCU's UART
  // stopped receiving for good after the C2 reset under it with the lines
  // free (2026-10-06 software resets, 2026-10-07 brownouts); a cold power-up
  // was fine. Since 0.8.9 the lines stay parked between uses, so an
  // unplanned reset (brownout, panic) also finds them held: begin() parks
  // them, each send releases TX only until its bytes are out, heartbeat()
  // releases GPIO3 for its pulse, and a clip lease releases GPIO10
  // (releaseCs/parkCs). begin() takes the holds over after a reset.
  void park();
  // park() for a planned reset (OTA, restart, rollback), and from then on
  // nothing releases the lines (write() fails, heartbeat() and releaseCs()
  // do nothing): esp_restart() lets other tasks run during its shutdown
  // handlers.
  void holdForRestart();
  void releaseCs();
  void parkCs();
  // One GPIO3 heartbeat pulse: low for HEARTBEAT_LOW_MS, then high (blocks).
  void heartbeat();

  // Diagnostics: bytes received and lines sent since boot, and the last
  // received bytes made printable (no spaces, no '^') for a mesh reply.
  uint32_t rxBytes() const { return rxBytes_; }
  uint32_t txLines() const { return txLines_; }
  void lastRx(char* out, size_t size) const;
  // Line levels (0/1): RX (GPIO5, MCU TX; a live UART idles high) and the
  // GPIO18 sync input. Samples `n` times over ~n ms and returns how many
  // samples were high, so a toggling line shows up as 0 < result < n.
  int  highSamples(int pin, int n) const;
  // Same, with the pin's pull-up swapped for a pull-down while sampling (then
  // restored): a line actually driven high stays high, a floating one drops.
  int  highSamplesPulledDown(int pin, int n) const;
  // Bench trace: while on, every line sent ("\n[<ms>]> line") and every byte
  // received is appended to `buf` (NUL-terminated; when full the oldest
  // bytes go, fanlogic::traceAppend).
  void traceStart(char* buf, size_t size);
  void traceStop() { trace_ = nullptr; }
  // Unsolicited "mculog:" lines the MCU printed outside clip leases
  // (fanlogic::McuLogLines), oldest first; false when none wait.
  bool takeLog(char* out, size_t size) { return log_.take(out, size); }
  static constexpr int PIN_RX_LINE = 5;
  static constexpr int PIN_SYNC_LINE = 18;

  // fanlogic::McuLink
  void wake() override;
  bool send(const char* line) override;
  int transact(const char* line, const char* needle, uint32_t timeoutMs, char* reply, size_t size) override;
  int expect(const char* line, const char* const* tokens, int n, uint32_t timeoutMs) override;
  int collect(char* buf, size_t size, uint32_t quietMs, uint32_t maxMs) override;
  int transactChunks(const char* line, uint32_t timeoutMs, void (*feed)(void* ctx, const char* b, size_t n),
                     void* ctx) override;

 private:
  void drain();
  bool write(const void* data, size_t n);
  std::atomic<bool> restarting_{false};
  fanlogic::McuLogLines log_;
  void note(const uint8_t* buf, int n);
  uint32_t rxBytes_ = 0, txLines_ = 0;
  void traceAdd(const char* s, size_t n);
  char*    trace_ = nullptr;
  size_t   traceSize_ = 0, traceLen_ = 0;
  int64_t  traceT0_ = 0;
  bool     traceRx_ = false;   // the trace is inside received bytes (no '< ' header needed)
  static constexpr size_t LAST_N = 40;
  uint8_t  last_[LAST_N] = {0};
  size_t   lastLen_ = 0;
};

#endif
