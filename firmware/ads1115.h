// Minimal ADS1115 driver — single-shot conversions on A0.
//
// Continuous mode on this clone returned a frozen conversion register, so the
// driver always uses single-shot: write the config (which starts a conversion
// on the I2C stop), wait out the aperture, then read the result. The first
// conversion after a mux/PGA change can be stale and is discarded at setup.
#pragma once

#include <Arduino.h>
#include <Wire.h>

namespace ads {

static const int kSdaPin = 21;
static const int kSclPin = 22;
static const uint8_t kAddr = 0x48;
// OS=1 | MUX=100 (A0-GND) | PGA=001 (±4.096 V) | MODE=1 (single-shot)
// | DR=111 (860 SPS) | comparator disabled.
static const uint16_t kConfig = 0x8000 | 0x4000 | 0x0200 | 0x0100 | 0x00E0 | 0x0003;
static const float kLsbMv = 4096.0f / 32768.0f;   // 0.125 mV per count
static const uint32_t kApertureUs = 1163;         // 1/860 SPS conversion time
static const uint32_t kCenterUs = 581;            // aperture centre, for time-alignment

static bool found = false;
static uint32_t errors = 0;

static bool writeReg(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}

static bool readReg(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)kAddr, (uint8_t)2) != 2) return false;
  value = ((uint16_t)Wire.read() << 8) | (uint16_t)Wire.read();
  return true;
}

inline void setup() {
  pinMode(kSdaPin, INPUT_PULLUP);
  pinMode(kSclPin, INPUT_PULLUP);
  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(100000);
  delay(20);

  for (uint8_t a = 0x48; a <= 0x4B; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() != 0) continue;
    uint16_t cfg = 0;
    if (readReg(0x01, cfg) && cfg != 0x0000 && cfg != 0xFFFF) {
      found = true;
      break;
    }
  }

  if (found) {
    // Discard the first conversion after the mux/PGA switch — it can be stale.
    writeReg(0x01, kConfig);
    delayMicroseconds(kApertureUs + 300);
    uint16_t dummy = 0;
    readReg(0x00, dummy);
  }
}

// Starts a conversion (completes when the I2C stop condition is sent).
inline void start() { writeReg(0x01, kConfig); }

inline bool readRaw(int16_t& raw) {
  uint16_t v = 0;
  if (!readReg(0x00, v)) { ++errors; return false; }
  raw = (int16_t)v;
  return true;
}

inline float rawToMv(int16_t raw) { return (float)raw * kLsbMv; }
inline bool present() { return found; }
inline uint32_t errorCount() { return errors; }

}  // namespace ads
