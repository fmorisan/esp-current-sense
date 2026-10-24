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
static const float kAmpsPerVolt = 100.0f;  // calibration: trim after kettle test
static const size_t kMaxSamples = 4096;    // ~0.5 s window at ESP32 ADC rates

static int16_t samples[kMaxSamples];  // millivolts, calibrated
static size_t count = 0;
static uint32_t windowStartUs = 0;
static uint32_t windowMs = 0;
static size_t windowsDone = 0;

static float vrmsMv = 0.0f;
static float rmsCurrentA = 0.0f;

inline void setup() {
  analogReadResolution(12);
  analogSetPinAttenuation(kPin, ADC_11db);  // ~3.1 V usable range, factory-calibrated
}

static void finalize() {
  const uint32_t elapsedUs = micros() - windowStartUs;

  // Pass 1: DC bias = mean of the window (self-calibrating midpoint).
  int64_t sum = 0;
  for (size_t i = 0; i < count; ++i) sum += samples[i];
  const float biasMv = (float)((double)sum / (double)count);

  // Pass 2: RMS around the measured bias.
  double sq = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double d = (double)samples[i] - (double)biasMv;
    sq += d * d;
  }
  vrmsMv = (float)sqrt(sq / (double)count);
  rmsCurrentA = vrmsMv / 1000.0f * kAmpsPerVolt;
  windowMs = elapsedUs / 1000;
  ++windowsDone;
  count = 0;  // restart window
}

// Sample for ~burstMs then return (so loop() can keep serving HTTP).
// Finalizes a window once the sample buffer fills.
inline void burst(uint32_t burstMs = 20) {
  const uint32_t end = millis() + burstMs;
  while (millis() < end && count < kMaxSamples) {
    if (count == 0) windowStartUs = micros();
    samples[count++] = (int16_t)analogReadMilliVolts(kPin);
  }
  if (count >= kMaxSamples) finalize();
}

inline float currentA() { return rmsCurrentA; }
inline float vrmsMillivolts() { return vrmsMv; }
inline float powerVa(float assumedMainsVoltage) { return rmsCurrentA * assumedMainsVoltage; }

}  // namespace ct
