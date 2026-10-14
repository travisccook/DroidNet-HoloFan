// Holofan UART bridge — SparkFun RedBoard Artemis (Apollo3 core 1.2.1).
//
// Turns the RedBoard Artemis into a 3.3 V USB-serial adapter for reading the
// holofan's ESP8684 (ESP32-C2) over its 5-hole header. Any 3.3 V USB-serial
// adapter does the same job; see README.md, "Connecting the fan".
//
//   Mac USB  <->  Serial  (CH340C, 115200)
//   Fan      <->  Serial1 (D0 = RX1 <- fan TX, D1 = TX1 -> fan RX, 115200)
//
// Pure byte passthrough, no commands, no boot-pin control: put the fan in
// download mode by holding its GPIO9 to GND by hand while it powers up.
// For the listen-only step leave the D1 -> fan RX wire disconnected.
//
// Core 1.2.1 on purpose: 2.1.x hangs if Serial1 RX is pulled low
// (sparkfun/Arduino_Apollo3 issue #423).

// FAN_BAUD can be overridden per build (the fan side only). Changing
// Serial1's baud at runtime with end()/begin() does not work on core 1.2.1
// (end() disables the wrong pads), so rebuild instead.
#ifndef FAN_BAUD
#define FAN_BAUD 115200
#endif

const unsigned long BAUD = 115200;

void setup() {
  Serial.begin(BAUD);
  Serial1.begin(FAN_BAUD);
  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  uint8_t buf[256];
  size_t n = 0;

  while (Serial.available() && n < sizeof(buf)) buf[n++] = Serial.read();
  if (n) Serial1.write(buf, n);

  n = 0;
  while (Serial1.available() && n < sizeof(buf)) buf[n++] = Serial1.read();
  if (n) {
    Serial.write(buf, n);
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));  // flickers on fan traffic
  }
}
