// CT clamp sampling — SCT-013 100A/1V on GPIO34 (ADC1_CH6).
//
// Frontend (see README.md): tip -> bias node (10k/10k divider 3V3->GND),
// sleeve -> GND. The divider biases the AC signal at ~1.65 V so the ADC can
// read the full waveform. We measure that bias ourselves each window instead
// of assuming 1650 mV, so divider tolerance and ADC offset cancel out.
//
// Method: burst-sample calibrated millivolts for ~20 ms per loop iteration
// (keeps the HTTP server responsive); when ~0.5 s of samples (~10 mains
// cycles @ 50 Hz) are buffered, compute bias + RMS, publish, restart.
//
//   I_primary_rms = V_secondary_rms * kAmpsPerVolt   (100 A / 1 V)
#pragma once

#include <Arduino.h>

namespace ct {

static const uint8_t kPin = 34;            // ADC1_CH6 — input-only, safe with WiFi
// Calibrated against a 2200 W kettle on 220 V mains through the final
// frontend (series 10 µF in the tip line): kettle displays 10.00 A with
// k = 127 (raw vrms ~79 mV through the 300 Hz digital low-pass).
// Residual accuracy ±15 % — refine against a utility meter or plug-in
// power meter.
static const float kAmpsPerVolt = 127.0f;
// Idle junk floor measures ~12.5 mV RMS → ~1.6 A at k=127; gate above it.
// The 100 nF ceramic at the pin will shrink junk and this gate together.
static const float kNoiseFloorA = 2.0f;
// Digital single-pole low-pass applied to samples before RMS: kills the
// aliased RF/SMPS junk the SAR ADC demodulates into our passband.
// alpha ≈ 1 - e^(-2π·fc/fs), fc = 300 Hz, fs ≈ 20 kS/s.
static const float kLpfAlpha = 0.090f;
static const size_t kMaxSamples = 4096;    // ~0.5 s window at ESP32 ADC rates

static int16_t samples[kMaxSamples];  // millivolts, calibrated
static size_t count = 0;
static uint32_t windowStartUs = 0;
static uint32_t windowMs = 0;
static size_t windowsDone = 0;

static float vrmsMv = 0.0f;
static float rmsCurrentA = 0.0f;
static float biasMv = 0.0f;
static float lpfState = 1650.0f;
static int16_t minMv = 0;
static int16_t maxMv = 0;
static uint16_t crossings = 0;

inline void setup() {
  analogReadResolution(12);
  analogSetPinAttenuation(kPin, ADC_11db);  // ~3.1 V usable range, factory-calibrated
  lpfState = 1650.0f;                       // seed near the expected bias midpoint
}

static void finalize() {
  const uint32_t elapsedUs = micros() - windowStartUs;

  // Pass 1: DC bias = mean of the window (self-calibrating midpoint).
  int64_t sum = 0;
  for (size_t i = 0; i < count; ++i) sum += samples[i];
  const float biasMvInstant = (float)((double)sum / (double)count);

  // Pass 2: RMS around THIS window's measured bias (not the smoothed one —
  // using the lagging EMA here inflates RMS whenever the signal centre moves,
  // e.g. during clamp moves or big load steps).
  double sq = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double d = (double)samples[i] - (double)biasMvInstant;
    sq += d * d;
  }
  vrmsMv = (float)sqrt(sq / (double)count);
  rmsCurrentA = vrmsMv / 1000.0f * kAmpsPerVolt;
  if (rmsCurrentA < kNoiseFloorA) rmsCurrentA = 0.0f;  // gate the ADC noise floor

  // Waveform shape diagnostics: min/max and zero crossings around the bias.
  // A clean 60 Hz sine in a ~240 ms window crosses ~29 times; aliased or
  // broadband noise crosses erratically. Peaks show if we're hitting the
  // ADC rails (clipping).
  minMv = 32767;
  maxMv = -32768;
  crossings = 0;
  bool lastAbove = samples[0] > (int16_t)biasMvInstant;
  for (size_t i = 0; i < count; ++i) {
    if (samples[i] < minMv) minMv = samples[i];
    if (samples[i] > maxMv) maxMv = samples[i];
    const bool above = samples[i] > (int16_t)biasMvInstant;
    if (above != lastAbove) {
      ++crossings;
      lastAbove = above;
    }
  }

  biasMv = biasMv * 0.5f + (float)biasMvInstant * 0.5f;  // smoothed for telemetry
  windowMs = elapsedUs / 1000;
  ++windowsDone;
  count = 0;  // restart window
}

// Sample for ~burstMs then return (so loop() can keep serving HTTP).
// Each sample passes through the digital low-pass (kLpfAlpha) before being
// buffered — junk above ~300 Hz never reaches the RMS math.
// Finalizes a window once the sample buffer fills.
inline void burst(uint32_t burstMs = 20) {
  const uint32_t end = millis() + burstMs;
  while (millis() < end && count < kMaxSamples) {
    if (count == 0) windowStartUs = micros();
    const float mv = (float)analogReadMilliVolts(kPin);
    lpfState += kLpfAlpha * (mv - lpfState);
    samples[count++] = (int16_t)lpfState;
  }
  if (count >= kMaxSamples) finalize();
}

inline float currentA() { return rmsCurrentA; }
inline float vrmsMillivolts() { return vrmsMv; }
inline float biasMillivolts() { return biasMv; }
inline int16_t minMillivolts() { return minMv; }
inline int16_t maxMillivolts() { return maxMv; }
inline uint16_t zeroCrossings() { return crossings; }
inline float powerVa(float assumedMainsVoltage) { return rmsCurrentA * assumedMainsVoltage; }

}  // namespace ct
