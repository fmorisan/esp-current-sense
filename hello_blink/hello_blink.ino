// hello_blink — minimal toolchain smoke test for the NodeMCU-32S.
//
// No WiFi, no sensors, no libraries: just prove that arduino-cli can build,
// flash and run code on this board, and that the onboard LED is where we think
// it is. Flash this first if you ever suspect the toolchain or the board.
//
//   make -C hello_blink build    # or: arduino-cli compile --fqbn esp32:esp32:nodemcu-32s .
//   make -C hello_blink upload   # or: arduino-cli upload --fqbn esp32:esp32:nodemcu-32s --port /dev/cu.usbserial-0001 .
//
// NodeMCU-32S onboard blue LED is on GPIO2 on the common clones. If nothing
// blinks, try 22 or 23 — some boards differ. GPIO2 is also a boot strapping
// pin (must be high at reset), which is why the LED is wired to it on many
// ESP32 dev boards: an LED + resistor to 3V3 satisfies the pull-up.

#include <Arduino.h>

#ifndef LED_PIN
#define LED_PIN 2
#endif

static const uint32_t kBlinkMs = 500;

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[hello_blink] esp-current-sense toolchain smoke test");
  Serial.printf("[hello_blink] chip=%s  cores=%d  freq=%d MHz  flash=%d KB\r\n",
                ESP.getChipModel(), ESP.getChipCores(),
                (int)(ESP.getCpuFreqMHz()), (int)(ESP.getFlashChipSize() / 1024));
  Serial.printf("[hello_blink] blinking GPIO%d every %lums\r\n",
                LED_PIN, (unsigned long)kBlinkMs);
}

void loop() {
  static bool on = false;
  on = !on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  Serial.printf("[hello_blink] LED %s  uptime=%lus\r\n",
                on ? "ON " : "OFF", (unsigned long)(millis() / 1000));
  delay(kBlinkMs);
}
