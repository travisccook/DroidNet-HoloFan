#include "McuUart.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "mcu";

static constexpr uart_port_t MCU_UART = UART_NUM_1;
static constexpr int PIN_TX = 4;
static constexpr int PIN_RX = 5;
static constexpr int PIN_TRIG = 3;       // heartbeat out
static constexpr int PIN_CS_MANUAL = 10; // SPI manual CS, idle high
static constexpr int PIN_SYNC = 18;      // SPI handshake in
static constexpr int BAUD = 230400;
static constexpr int RX_BUF = 2048;      // UART1 rx buffer (bytes)
static constexpr uint32_t QUIET_MS = 350; // "end of reply" for replies without a token

static int64_t nowUs() { return esp_timer_get_time(); }

bool McuUart::begin() {
  gpio_config_t out = {};
  out.pin_bit_mask = (1ULL << PIN_TRIG) | (1ULL << PIN_CS_MANUAL);
  out.mode = GPIO_MODE_OUTPUT;
  out.pull_up_en = GPIO_PULLUP_DISABLE;
  out.pull_down_en = GPIO_PULLDOWN_DISABLE;
  out.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&out);
  gpio_set_level((gpio_num_t)PIN_TRIG, 1);
  gpio_set_level((gpio_num_t)PIN_CS_MANUAL, 1);
  gpio_hold_dis((gpio_num_t)PIN_TRIG);              // after a holdForRestart(): levels are already set
  gpio_hold_dis((gpio_num_t)PIN_CS_MANUAL);

  gpio_config_t in = {};
  in.pin_bit_mask = (1ULL << PIN_SYNC) | (1ULL << PIN_RX);
  in.mode = GPIO_MODE_INPUT;
  in.pull_up_en = GPIO_PULLUP_ENABLE;
  in.pull_down_en = GPIO_PULLDOWN_DISABLE;
  in.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&in);

  uart_config_t cfg = {};
  cfg.baud_rate = BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.rx_flow_ctrl_thresh = 0;
  cfg.source_clk = UART_SCLK_DEFAULT;     // must be explicit: 0 is not a valid clock
  esp_err_t err = uart_param_config(MCU_UART, &cfg);
  if (err == ESP_OK) err = uart_set_pin(MCU_UART, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (err == ESP_OK) err = uart_driver_install(MCU_UART, RX_BUF, 0, 0, nullptr, 0);
  gpio_hold_dis((gpio_num_t)PIN_TX);                // UART TX now drives the idle-high level
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "UART1 init failed: %s", esp_err_to_name(err));
    return false;
  }
  park();
  return true;
}

void McuUart::park() {
  uart_wait_tx_done(MCU_UART, pdMS_TO_TICKS(50));   // TX back at idle (high)
  gpio_set_level((gpio_num_t)PIN_TRIG, 1);
  gpio_set_level((gpio_num_t)PIN_CS_MANUAL, 1);     // not latched low if a park lands mid-frame
  gpio_hold_en((gpio_num_t)PIN_TX);
  gpio_hold_en((gpio_num_t)PIN_TRIG);
  gpio_hold_en((gpio_num_t)PIN_CS_MANUAL);
}

// TX is released only while bytes go out. If the FIFO does not drain in time
// TX stays released: holding it mid-byte would latch a low (a break).
bool McuUart::write(const void* data, size_t n) {
  if (restarting_) return false;                    // the lines stay parked until the reset
  gpio_hold_dis((gpio_num_t)PIN_TX);
  bool ok = uart_write_bytes(MCU_UART, data, n) == (int)n;   // returns int, not esp_err_t
  if (uart_wait_tx_done(MCU_UART, pdMS_TO_TICKS(50)) == ESP_OK) gpio_hold_en((gpio_num_t)PIN_TX);
  return ok;
}

void McuUart::releaseCs() {
  if (!restarting_) gpio_hold_dis((gpio_num_t)PIN_CS_MANUAL);
}

void McuUart::parkCs() {
  gpio_set_level((gpio_num_t)PIN_CS_MANUAL, 1);
  gpio_hold_en((gpio_num_t)PIN_CS_MANUAL);
}

// esp_restart() runs its shutdown handlers (esp_wifi_stop) before it stops
// the scheduler, so the MCU task can still run after this: the flag keeps
// write(), heartbeat() and releaseCs() from releasing the held lines.
void McuUart::holdForRestart() {
  restarting_ = true;
  park();
}

void McuUart::heartbeat() {
  if (restarting_) return;
  gpio_hold_dis((gpio_num_t)PIN_TRIG);
  gpio_set_level((gpio_num_t)PIN_TRIG, 0);
  vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_LOW_MS));
  gpio_set_level((gpio_num_t)PIN_TRIG, 1);
  gpio_hold_en((gpio_num_t)PIN_TRIG);
}

void McuUart::wake() {
  const char nn[2] = {'\n', '\n'};
  write(nn, sizeof(nn));
}

// Keep a count and the last LAST_N received bytes.
void McuUart::traceStart(char* buf, size_t size) {
  traceT0_ = nowUs();
  traceLen_ = 0;
  traceSize_ = size;
  if (size) buf[0] = '\0';
  trace_ = size ? buf : nullptr;
}

// Full: drop the oldest bytes, keep the latest (fanlogic::traceAppend; 0.7.2
// fixes a NUL written one byte past the buffer).
void McuUart::traceAdd(const char* s, size_t n) {
  if (trace_) traceLen_ = fanlogic::traceAppend(trace_, traceSize_, traceLen_, s, n);
}

void McuUart::note(const uint8_t* buf, int n) {
  if (n <= 0) return;
  if (trace_ && !traceRx_) {
    char head[24];
    int h = snprintf(head, sizeof(head), "\n[%lu]< ", (unsigned long)((nowUs() - traceT0_) / 1000));
    traceAdd(head, (size_t)h);
    traceRx_ = true;
  }
  traceAdd((const char*)buf, (size_t)n);
  if (!trace_) log_.feed(buf, (size_t)n);           // a clip lease has its own trace
  rxBytes_ += (uint32_t)n;
  for (int i = 0; i < n; i++) {
    if (lastLen_ == LAST_N) { memmove(last_, last_ + 1, LAST_N - 1); lastLen_--; }
    last_[lastLen_++] = buf[i];
  }
}

int McuUart::highSamples(int pin, int n) const {
  int high = 0;
  for (int i = 0; i < n; i++) {
    high += gpio_get_level((gpio_num_t)pin) ? 1 : 0;
    esp_rom_delay_us(1000);
  }
  return high;
}

int McuUart::highSamplesPulledDown(int pin, int n) const {
  gpio_pullup_dis((gpio_num_t)pin);
  gpio_pulldown_en((gpio_num_t)pin);
  esp_rom_delay_us(200);
  int high = highSamples(pin, n);
  gpio_pulldown_dis((gpio_num_t)pin);
  gpio_pullup_en((gpio_num_t)pin);
  return high;
}

void McuUart::lastRx(char* out, size_t size) const {
  if (size == 0) return;
  size_t o = 0;
  for (size_t i = 0; i < lastLen_ && o + 1 < size; i++) {
    uint8_t c = last_[i];
    out[o++] = (c == '\n') ? '|' : (c > 0x20 && c < 0x7F && c != '^' && c != '{') ? (char)c : '.';
  }
  if (o == 0 && size > 1) out[o++] = '-';
  out[o] = '\0';
}

bool McuUart::send(const char* line) {
  size_t n = strlen(line);
  txLines_++;
  if (trace_) {
    char head[24];
    int h = snprintf(head, sizeof(head), "\n[%lu]> ", (unsigned long)((nowUs() - traceT0_) / 1000));
    traceAdd(head, (size_t)h);
    traceAdd(line, n);
    traceRx_ = false;
  }
  return write(line, n);
}

// Drop whatever is waiting (stray "mculog:" lines, late replies), bounded so
// a chatty MCU cannot hold us here.
void McuUart::drain() {
  uint8_t b[256];
  for (int i = 0; i < 16; i++) {
    int r = uart_read_bytes(MCU_UART, b, sizeof(b), pdMS_TO_TICKS(5));
    if (r <= 0) break;
    note(b, r);
  }
}

int McuUart::transact(const char* line, const char* needle, uint32_t timeoutMs, char* reply, size_t size) {
  if (size == 0) return 0;
  reply[0] = '\0';
  drain();
  if (!send(line)) return 0;
  int len = 0;
  int64_t deadline = nowUs() + (int64_t)timeoutMs * 1000;
  int64_t lastByte = 0;
  while (nowUs() < deadline && len < (int)size - 1) {
    int r = uart_read_bytes(MCU_UART, (uint8_t*)reply + len, size - 1 - len, pdMS_TO_TICKS(10));
    if (r > 0) {
      note((const uint8_t*)reply + len, r);
      len += r;
      reply[len] = '\0';
      lastByte = nowUs();
      if (needle && strstr(reply, needle)) {
        // The token is in; let the rest of its line arrive.
        if (strchr(strstr(reply, needle), '\n')) break;
      }
    } else if (!needle && len > 0 && nowUs() - lastByte >= (int64_t)QUIET_MS * 1000) {
      break;
    }
  }
  return len;
}

// As transact(line, nullptr, ...), but each piece goes to `feed` as it
// arrives: the full "s" listing (#HLFT) can be longer than any buffer here.
int McuUart::transactChunks(const char* line, uint32_t timeoutMs, void (*feed)(void* ctx, const char* b, size_t n),
                            void* ctx) {
  drain();
  if (!send(line)) return 0;
  int total = 0;
  int64_t deadline = nowUs() + (int64_t)timeoutMs * 1000;
  int64_t lastByte = 0;
  uint8_t tmp[128];
  while (nowUs() < deadline) {
    int r = uart_read_bytes(MCU_UART, tmp, sizeof(tmp), pdMS_TO_TICKS(10));
    if (r > 0) {
      note(tmp, r);
      feed(ctx, (const char*)tmp, (size_t)r);
      total += r;
      lastByte = nowUs();
    } else if (total > 0 && nowUs() - lastByte >= (int64_t)QUIET_MS * 1000) {
      break;
    }
  }
  return total;
}

int McuUart::expect(const char* line, const char* const* tokens, int n, uint32_t timeoutMs) {
  if (line) {
    drain();
    if (!send(line)) return -1;
  }
  char buf[256];
  size_t len = 0;
  int64_t deadline = nowUs() + (int64_t)timeoutMs * 1000;
  while (nowUs() < deadline) {
    int r = uart_read_bytes(MCU_UART, (uint8_t*)buf + len, sizeof(buf) - 1 - len, pdMS_TO_TICKS(10));
    if (r <= 0) continue;
    note((const uint8_t*)buf + len, r);
    len += (size_t)r;
    buf[len] = '\0';
    for (int i = 0; i < n; i++)
      if (strstr(buf, tokens[i])) return i;
    if (len > 192) {                       // keep a tail so a token split across reads still matches
      memmove(buf, buf + len - 16, 16);
      len = 16;
      buf[len] = '\0';
    }
  }
  return -1;
}

int McuUart::collect(char* buf, size_t size, uint32_t quietMs, uint32_t maxMs) {
  size_t len = buf ? strlen(buf) : 0;
  uint8_t tmp[128];
  int total = 0;
  int64_t start = nowUs(), last = start;
  while (nowUs() - start < (int64_t)maxMs * 1000 && nowUs() - last < (int64_t)quietMs * 1000) {
    // Never wait past maxMs: with maxMs < 10 the read is non-blocking (0 ticks), so a
    // 1 ms poll between SPI frames does not add a 10 ms stall.
    const TickType_t wait = maxMs < 10 ? 0 : pdMS_TO_TICKS(10);
    int r = uart_read_bytes(MCU_UART, tmp, sizeof(tmp), wait);
    if (r <= 0) continue;
    if (trace_) traceRx_ = false;     // a gap long enough to be worth a new timestamp
    note(tmp, r);
    total += r;
    last = nowUs();
    if (buf && size > 0) {
      size_t k = (size_t)r;
      if (k > size - 1 - len) k = size - 1 - len;
      memcpy(buf + len, tmp, k);
      len += k;
      buf[len] = '\0';
    }
  }
  return total;
}
