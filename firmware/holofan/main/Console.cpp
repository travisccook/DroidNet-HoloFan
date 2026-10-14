#include "Console.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "FanMesh.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static QueueHandle_t s_mirror = nullptr;
static constexpr int MIRROR_DEPTH = 16;

void con(const char* fmt, ...) {
  char line[CON_LINE_MAX + 1];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  printf("%s\n", line);
  if (s_mirror) xQueueSend(s_mirror, line, 0);    // full: the line is only lost on the mesh
}

bool conTakeMirror(char* out, size_t size) {
  char line[CON_LINE_MAX + 1];
  if (!s_mirror || xQueueReceive(s_mirror, line, 0) != pdTRUE) return false;
  strncpy(out, line, size - 1);
  out[size - 1] = '\0';
  return true;
}

// UART0 reader: collects a line (CR or LF ends it) and hands it over.
static void readerTask(void*) {
  char line[CON_LINE_MAX + 1];
  size_t len = 0;
  for (;;) {
    uint8_t c;
    int r = uart_read_bytes((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, portMAX_DELAY);
    if (r < 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }    // driver error: never spin
    if (r != 1) continue;
    if (c == '\r' || c == '\n') {
      if (len == 0) continue;
      line[len] = '\0';
      fanMeshConsoleLine(line);
      len = 0;
    } else if (len < CON_LINE_MAX) {
      line[len++] = (char)c;
    }
  }
}

void conStart() {
  s_mirror = xQueueCreate(MIRROR_DEPTH, CON_LINE_MAX + 1);
  // Route stdin/stdout through the UART driver so reads and printf share it.
  esp_err_t err = uart_driver_install((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, nullptr, 0);
  if (err != ESP_OK) {
    printf("console: uart_driver_install failed (%s): no console input\n", esp_err_to_name(err));
    return;
  }
  uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
  xTaskCreate(readerTask, "console", 3072, nullptr, 3, nullptr);
}
