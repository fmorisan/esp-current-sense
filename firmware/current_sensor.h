// CT clamp current sampling — SCT-013 100A/1V through the ADS1115 (A0).
//
// Frontend (see README.md): CT tip -> series 10 µF -> bias node (10k/10k
// divider 3V3->GND), CT sleeve -> GND, node -> ADS1115 A0. The divider biases
// the AC signal at ~1.65 V. A 100 nF node->GND is optional RF filtering
// (RC corner ~318 Hz with the 5 kΩ divider; ~1.2 % attenuation at 50 Hz).
//
// The ADS1115 needs a settling conversion after a mux/PGA change: the first
// conversion following a channel switch can return stale data. This module
// uses single-shot conversions (continuous mode on this clone returned a
// frozen register) and reads one channel only, so the settle discard happens
// once at setup.
//
// Method: burst-read for ~20 ms per loop iteration (keeps the HTTP server
// responsive), accumulating sum / sum-of-squares / min / max over a fixed
// ~1 s window. Bias is the window mean; RMS is measured around that bias.
//   I_rms = vrms_mV / 1000 * kAmpsPerVolt
#pragma once

#include <Arduino.h>
#include <Wire.h>

namespace current {

static const int kSdaPin = 21;
static const int kSclPin = 22;
static const uint8_t kAddr = 0x48;
// Single-shot, A0 vs GND, PGA ±4.096 V, 860 SPS, comparator disabled.
static const uint16_t kConfig = 0x8000 | 0x4000 | 0x0200 | 0x0100 | 0x00E0 | 0x0003;
static const float kLsbMv = 4096.0f / 32768.0f;  // 0.125 mV per count
static const uint32_t kSettleUs = 1800;

// Calibrated against a 2200 W kettle through the same frontend (10.0 A at
// ~79 mV RMS). The optional 100 nF node cap attenuates 50 Hz by ~1.2 %; trim
// this constant down to ~125.5 if you fit the cap and want the last percent.
static const float kAmpsPerVolt = 127.0f;
// ADS1115 noise floor measured ~0.25-0.31 mV RMS (~0.03-0.04 A at k=127).
static const float kNoiseFloorA = 0.05f;
static const uint32_t kWindowMs = 1000;  // fixed integration window

static bool found = false;
static uint32_t i2cErrors = 0;

static double sum = 0.0;
static double sumSq = 0.0;
static uint32_t n = 0;
static int16_t minRaw = 32767;
static int16_t maxRaw = -32768;
static uint32_t windowStartMs = 0;

static float biasMv = 0.0f;
static float vrmsMv = 0.0f;
static float rmsCurrentA = 0.0f;
static size_t windowsDone = 0;

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

static uint16_t oneConversion() {
  writeReg(0x01, kConfig);
  delayMicroseconds(kSettleUs);
  uint16_t raw = 0;
  readReg(0x00, raw);
  return raw;
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
    oneConversion();
    windowStartMs = millis();
  }
}

static void finalize() {
  if (n == 0) { windowStartMs = millis(); return; }
  biasMv = (float)(sum / (double)n);
  const double meanSq = sumSq / (double)n;
  const double var = meanSq - (double)biasMv * (double)biasMv;
  vrmsMv = var > 0 ? (float)sqrt(var) : 0.0f;
  rmsCurrentA = vrmsMv / 1000.0f * kAmpsPerVolt;
  if (rmsCurrentA < kNoiseFloorA) rmsCurrentA = 0.0f;  // gate the ADC noise floor
  ++windowsDone;
  sum = sumSq = 0.0;
  n = 0;
  minRaw = 32767;
  maxRaw = -32768;
  windowStartMs = millis();
}

// Read for ~burstMs then return (so loop() can keep serving HTTP).
inline void burst(uint32_t burstMs = 20) {
  if (!found) return;
  const uint32_t end = millis() + burstMs;
  while (millis() < end) {
    const uint16_t raw = oneConversion();
    const int16_t s = (int16_t)raw;
    const double mv = (double)s * kLsbMv;
    sum += mv;
    sumSq += mv * mv;
    ++n;
    if (s < minRaw) minRaw = s;
    if (s > maxRaw) maxRaw = s;
  }
  if (millis() - windowStartMs >= kWindowMs) finalize();
}

inline bool present() { return found; }
inline float currentA() { return rmsCurrentA; }
inline float vrmsMillivolts() { return vrmsMv; }
inline float biasMillivolts() { return biasMv; }
inline int16_t minCounts() { return minRaw; }
inline int16_t maxCounts() { return maxRaw; }
inline uint32_t samplesRead() { return n; }
inline size_t windowsDoneCount() { return windowsDone; }
inline uint32_t errors() { return i2cErrors; }

}  // namespace current
