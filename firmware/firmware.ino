// esp-current-sense — NodeMCU-32S (ESP32-D0WD-V3)
//
// Phase 1: hello-world. Blinks the onboard LED and prints to USB serial.
// On the NodeMCU-32S the blue onboard LED sits on GPIO2 (strapping pin —
// it flickers during boot/flashing, that's normal).
// Polarity varies between clones: if "HIGH" looks dim and "LOW" looks
// bright, your board is active-low — the blink period is unaffected.
//
// Later phases (see README.md):
//   2. WiFi + HTTP /metrics endpoint for bring-up
//   3. CT sampling on GPIO34 (ADC1_CH6) + RMS current
//   4. MQTT + Home Assistant discovery
//   5. OTA updates, calibration constants in NVS

#include <Arduino.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

static const uint32_t kSerialBaud = 115200;
static const uint32_t kBlinkIntervalMs = 500;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(kSerialBaud);
  delay(200);
  Serial.println();
  Serial.println("[esp-current-sense] boot ok — blinking GPIO2");
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.println("[esp-current-sense] LED on");
  delay(kBlinkIntervalMs);

  digitalWrite(LED_BUILTIN, LOW);
  Serial.println("[esp-current-sense] LED off");
  delay(kBlinkIntervalMs);
}
