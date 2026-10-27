// ADS1115 dual-channel diagnostic — A0 = CT current, A1 = ZMPT101B voltage.
//
// The ADS1115 needs a settling conversion after a mux/PGA change: the first
// conversion following a channel switch can return the previous channel's
// data. This probe reads BOTH the first and the second conversion after each
// switch so we can see exactly what is happening, and accumulates statistics
// from the settled (second) conversion.
//
// Build/flash:
//   arduino-cli compile --fqbn esp32:esp32:nodemcu-32s --upload \
//       -p /dev/cu.usbserial-0001 tools/ads1115_probe

#include <Arduino.h>
#include <Wire.h>

static const int kSdaPin = 21;
static const int kSclPin = 22;
static const uint8_t kAddr = 0x48;

static const uint16_t kConfigA0 = 0x8000 | 0x4000 | 0x0200 | 0x0000 | 0x00E0 | 0x0003; // +/-4.096
static const uint16_t kConfigA1 = 0x8000 | 0x5000 | 0x0200 | 0x0000 | 0x00E0 | 0x0003; // +/-4.096 (A1 vs GND)

static const float kLsbA0Uv = 4096000.0f / 32768.0f;  // 125 uV
static const float kLsbA1Uv = 4096000.0f / 32768.0f;  // 125 uV
static const uint32_t kSettleUs = 1800;

static uint8_t activeAddr = 0;

struct Pair { int16_t first; int16_t second; };

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

static uint16_t oneConversion(uint16_t cfg) {
  writeReg(0x01, cfg);
  delayMicroseconds(kSettleUs);
  uint16_t raw = 0;
  readReg(0x00, raw);
  return raw;
}

// First conversion after switching config may be stale; second is settled.
static Pair readPair(uint16_t cfg) {
  Pair p;
  p.first  = (int16_t)oneConversion(cfg);
  p.second = (int16_t)oneConversion(cfg);
  return p;
}

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
      Serial.printf("[ads] FOUND at 0x%02X (acks %d/3)\r\n", a, acks);
      return a;
    }
  }
  return 0;
}

struct Chan {
  double sum = 0, sumSq = 0;
  int16_t mn = 32767, mx = -32768;
  uint32_t n = 0;
  void add(int16_t raw, double mv) {
    sum += mv; sumSq += mv * mv; ++n;
    if (raw < mn) mn = raw;
    if (raw > mx) mx = raw;
  }
  double mean() const { return n ? sum / n : 0; }
  double rms() const {
    if (!n) return 0;
    const double m = mean();
    const double v = sumSq / n - m * m;
    return v > 0 ? sqrt(v) : 0;
  }
  void reset() { sum = sumSq = 0; n = 0; mn = 32767; mx = -32768; }
};

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\r\n[ads1115-probe] dual-channel diagnostic");

  pinMode(kSdaPin, INPUT);
  pinMode(kSclPin, INPUT);
  delay(20);
  Serial.printf("[i2c] external pull-ups %s\r\n",
                (digitalRead(kSdaPin) && digitalRead(kSclPin)) ? "PRESENT" : "MISSING");

  pinMode(kSdaPin, INPUT_PULLUP);
  pinMode(kSclPin, INPUT_PULLUP);
  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(100000);
  delay(20);

  if (!findAds()) {
    Serial.println("[i2c] no ADS1115 found.");
  }
}

void loop() {
  if (activeAddr == 0) { delay(1000); return; }

  static Chan a0, a1;
  static uint32_t winStart = millis(), lastTrace = 0;
  static uint32_t bad = 0;
  static Pair last0, last1;

  last0 = readPair(kConfigA0);
  last1 = readPair(kConfigA1);

  a0.add(last0.second, (double)last0.second * kLsbA0Uv / 1000.0);
  a1.add(last1.second, (double)last1.second * kLsbA1Uv / 1000.0);

  if (millis() - lastTrace >= 500) {
    lastTrace = millis();
    Serial.printf("[trace] A0 first=%6d second=%6d | A1 first=%6d second=%6d\r\n",
                  last0.first, last0.second, last1.first, last1.second);
  }

  if (millis() - winStart >= 1000) {
    Serial.printf("[A0 CT] n=%lu bias=%.1f mV ac_rms=%.2f mV  raw %d..%d\r\n",
                  (unsigned long)a0.n, a0.mean(), a0.rms(), a0.mn, a0.mx);
    Serial.printf("[A1 AC] n=%lu bias=%.1f mV ac_rms=%.2f mV  raw %d..%d\r\n",
                  (unsigned long)a1.n, a1.mean(), a1.rms(), a1.mn, a1.mx);
    Serial.printf("[sum]  I=%.2f A @10mV/A  i2c_err=%lu\r\n\r\n",
                  a0.rms() / 10.0, (unsigned long)bad);
    a0.reset(); a1.reset();
    winStart = millis();
  }
}
