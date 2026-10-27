// ADS1115 bring-up probe — standalone, no WiFi, no project deps.
//
// Diagnostics performed at boot:
//   1. Are there EXTERNAL pull-ups? (module powered/connected)
//   2. Probe 0x48..0x4B with SDA=21, SCL=22
//   3. If nothing valid, retry with the pins SWAPPED in software — proves
//      whether the SDA/SCL wiring is reversed without touching hardware.
//   4. Once found: A0 single-ended, PGA +/-4.096 V, 860 SPS, continuous,
//      stream raw counts and a 1 s RMS summary.
//
// Build/flash:
//   arduino-cli compile --fqbn esp32:esp32:nodemcu-32s --upload \
//       -p /dev/cu.usbserial-0001 tools/ads1115_probe

#include <Arduino.h>
#include <Wire.h>

static const int kSdaPin = 21;
static const int kSclPin = 22;
static const uint8_t kExpectedAddr = 0x48;   // ADDR -> GND

static const uint16_t kConfig =
    0x8000 |  // OS: start conversions
    0x4000 |  // MUX: 100 = AIN0 single-ended vs GND
    0x0200 |  // PGA: 001 = +/-4.096 V
    0x0100 |  // MODE: continuous
    0x00E0 |  // DR: 111 = 860 SPS
    0x0003;   // comparator disabled

static const float kLsbUv = 4096000.0f / 32768.0f;  // ~125 uV per count
static uint8_t activeAddr = 0;

static bool writeReg(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(activeAddr);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}

static bool readReg(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(activeAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)activeAddr, (uint8_t)2) != 2) return false;
  value = ((uint16_t)Wire.read() << 8) | (uint16_t)Wire.read();
  return true;
}

static bool readConversion(int16_t& out) {
  uint16_t raw = 0;
  if (!readReg(0x00, raw)) return false;
  out = (int16_t)raw;
  return true;
}

// Probe 0x48..0x4B: require a stable ACK *and* a readable config register.
static uint8_t findAds() {
  for (uint8_t a = 0x48; a <= 0x4B; ++a) {
    int acks = 0;
    for (int t = 0; t < 3; ++t) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) ++acks;
      delayMicroseconds(300);
    }
    if (acks < 2) continue;
    activeAddr = a;
    uint16_t cfg = 0;
    if (readReg(0x01, cfg) && cfg != 0x0000 && cfg != 0xFFFF) {
      Serial.printf("[ads] FOUND at 0x%02X (acks %d/3, config reads 0x%04X)\r\n", a, acks, cfg);
      return a;
    }
    Serial.printf("[ads] 0x%02X acks %d/3 but config unreadable\r\n", a, acks);
  }
  return 0;
}

static bool tryBus(int sda, int scl) {
  Wire.end();
  Wire.begin(sda, scl);
  Wire.setClock(50000);
  delay(20);
  Serial.printf("[i2c] probing with SDA=GPIO%d SCL=GPIO%d\r\n", sda, scl);
  activeAddr = 0;
  return findAds() != 0;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\r\n[ads1115-probe] boot");

  // External pull-up test: with the ESP32's internal pull-ups off, a powered
  // module holds both lines high. A floating bus reads 0 / wanders.
  pinMode(kSdaPin, INPUT);
  pinMode(kSclPin, INPUT);
  delay(20);
  const int bareSda = digitalRead(kSdaPin);
  const int bareScl = digitalRead(kSclPin);
  Serial.printf("[i2c] internal pull-ups off: SDA=%d SCL=%d -> external pull-ups %s\r\n",
                bareSda, bareScl, (bareSda && bareScl) ? "PRESENT" : "MISSING");

  pinMode(kSdaPin, INPUT_PULLUP);
  pinMode(kSclPin, INPUT_PULLUP);

  bool ok = tryBus(kSdaPin, kSclPin);
  if (!ok) {
    Serial.println("[i2c] nothing valid — retrying with SDA/SCL SWAPPED in software");
    ok = tryBus(kSclPin, kSdaPin);
    if (ok) {
      Serial.println("[i2c] *** SUCCESS WITH SWAPPED PINS -> your SDA/SCL wires are reversed ***");
    }
  }

  if (!ok) {
    Serial.println("[i2c] no ADS1115 on either pin order. Check, in this order:");
    Serial.println("       1. VDD on the module reads ~3.3 V (multimeter)");
    Serial.println("       2. GND continuity module <-> ESP32");
    Serial.println("       3. SDA/SCL continuity to GPIO21/GPIO22");
    Serial.println("       4. ADDR pin tied to GND (~0 V)");
    Serial.println("       5. reseat the module in the breadboard (loose row)");
    return;
  }

  writeReg(0x01, kConfig);
  delay(10);
  uint16_t verify = 0;
  if (readReg(0x01, verify)) {
    Serial.printf("[ads] config 0x%04X written, read back 0x%04X\r\n", kConfig, verify);
  }
}

void loop() {
  if (activeAddr == 0) { delay(1000); return; }

  static uint32_t winStart = millis();
  static double sum = 0.0, sumSq = 0.0;
  static uint32_t n = 0, bad = 0, lastPrint = 0;

  int16_t raw = 0;
  if (readConversion(raw)) {
    const double mv = (double)raw * kLsbUv / 1000.0;
    sum += mv; sumSq += mv * mv; ++n;
  } else {
    ++bad;
  }

  if (millis() - lastPrint >= 250) {
    lastPrint = millis();
    Serial.printf("[raw] %6d  %8.2f mV\r\n", raw, (double)raw * kLsbUv / 1000.0);
  }

  if (millis() - winStart >= 1000) {
    if (n > 0) {
      const double mean = sum / n;
      const double variance = sumSq / n - mean * mean;
      const double rms = variance > 0 ? sqrt(variance) : 0.0;
      Serial.printf("[stats] n=%lu  bias=%.1f mV  ac_rms=%.2f mV  (%.2f A @10mV/A)  i2c_err=%lu\r\n",
                    (unsigned long)n, mean, rms, rms / 10.0, (unsigned long)bad);
    } else {
      Serial.printf("[stats] no samples, i2c_err=%lu\r\n", (unsigned long)bad);
    }
    winStart = millis(); sum = sumSq = 0.0; n = 0; bad = 0;
  }

  delay(1);
}
