// Mains voltage sampling — ZMPT101B on GPIO34 (ADC1_CH6).
//
// Frontend (see README.md): ZMPT101B OUT -> series 10 µF -> bias node
// (10k/10k divider 3V3->GND, ~1.65 V), ZMPT101B VCC -> VIN (5 V).
// The divider biases the AC signal at mid-rail so the ADC reads the full
// waveform. The bias is measured each window instead of assumed, so divider
// tolerance and ADC offset cancel out.
//
// Method: burst-sample calibrated millivolts for ~20 ms per loop iteration
// (keeps the HTTP server responsive). The RMS window closes after a whole
// number of mains cycles — rising edges counted with hysteresis around the
// previous window's bias — so the integral always covers complete half-cycles
// (sin² has period π, so any whole number of them averages exactly). That
// removes partial-cycle error and self-adapts to 50 or 60 Hz mains.
//
//   V_rms = vrms_mV / 1000 * kVoltsPerMv   (kVoltsPerMv ≈ 0.691 V/mV,
//   calibrated against a multimeter: 219 V ↔ ~317 mV at the node)
#pragma once

#include <Arduino.h>

namespace voltage {

static const uint8_t kPin = 34;            // ADC1_CH6 — input-only, safe with WiFi
// Calibrated against a multimeter: 220 V mains <-> ~281.6 mV RMS at the node.
// Re-derive whenever the divider, coupling cap or ZMPT pot changes.
static const float kVoltsPerMv = 0.781f;
// Below this the input is considered "no mains" (idle noise ~0.3-0.8 mV RMS).
static const float kNoiseFloorMv = 3.0f;
// Digital single-pole low-pass applied to samples before RMS: kills the
// aliased RF/SMPS junk the SAR ADC demodulates into our passband.
// alpha ≈ 1 - e^(-2π·fc/fs), fc = 300 Hz, fs ≈ 20 kS/s.
static const float kLpfAlpha = 0.090f;
static const size_t kMaxSamples = 4096;      // ~180 ms at ~22 kS/s — safety cap
static const uint16_t kEdgesPerWindow = 9;   // 9 rising edges = 8 whole cycles
static const int16_t kCrossHystMv = 45;      // well above the ~15 mV ADC noise floor
static const float kBiasValidMv = 500.0f;    // crossing detection needs a real bias
// Edge counting only runs when the previous window's RMS was clearly above the
// hysteresis band; below that the buffer-full fallback takes over.
static const float kEdgeMinVrmsMv = 60.0f;

static int16_t samples[kMaxSamples];  // millivolts, calibrated
static size_t count = 0;
static size_t firstEdgeIdx = 0;       // sample index of the window's first rising edge
static bool haveFirstEdge = false;
static uint32_t windowStartUs = 0;
static uint32_t windowMs = 0;
static size_t windowsDone = 0;

static float vrmsMv = 0.0f;
static float rmsVolts = 0.0f;
static float biasMv = 0.0f;
static float lpfState = 1650.0f;
static int16_t minMv = 0;
static int16_t maxMv = 0;
static uint16_t crossings = 0;
static uint16_t cycles = 0;
static uint32_t firstCycleUs = 0;
static uint32_t lastCycleUs = 0;
static bool crossAbove = false;
static float mainsHz = 0.0f;

inline void setup() {
  analogReadResolution(12);
  analogSetPinAttenuation(kPin, ADC_11db);  // ~3.1 V usable range, factory-calibrated
  lpfState = 1650.0f;                       // seed near the expected bias midpoint
}

static void finalize() {
  // Whole-cycle window: skip the partial preamble before the first rising edge.
  const bool wholeCycles = haveFirstEdge && cycles >= kEdgesPerWindow;
  const size_t start = wholeCycles ? firstEdgeIdx : 0;
  const size_t n = count - start;
  if (n == 0) { count = 0; haveFirstEdge = false; cycles = 0; return; }

  const uint32_t t0 = wholeCycles ? firstCycleUs : windowStartUs;
  const uint32_t elapsedUs = micros() - t0;

  // Pass 1: DC bias = mean of the window (self-calibrating midpoint).
  int64_t sum = 0;
  for (size_t i = start; i < count; ++i) sum += samples[i];
  const float biasMvInstant = (float)((double)sum / (double)n);

  // Pass 2: RMS around THIS window's measured bias.
  double sq = 0.0;
  for (size_t i = start; i < count; ++i) {
    const double d = (double)samples[i] - (double)biasMvInstant;
    sq += d * d;
  }
  vrmsMv = (float)sqrt(sq / (double)n);
  rmsVolts = (vrmsMv < kNoiseFloorMv) ? 0.0f : vrmsMv * kVoltsPerMv;

  // Waveform diagnostics: min/max and hysteresis crossings around this
  // window's bias. Two crossings per mains cycle; hysteresis keeps ADC noise
  // from triggering false counts.
  minMv = 32767;
  maxMv = -32768;
  crossings = 0;
  bool above = false;
  for (size_t i = start; i < count; ++i) {
    if (samples[i] < minMv) minMv = samples[i];
    if (samples[i] > maxMv) maxMv = samples[i];
    if (!above && (float)samples[i] > biasMvInstant + (float)kCrossHystMv) {
      above = true;
      ++crossings;
    } else if (above && (float)samples[i] < biasMvInstant - (float)kCrossHystMv) {
      above = false;
      ++crossings;
    }
  }

  // Mains frequency from the edge timestamps — measured between real waveform
  // events, so the sampling gaps between bursts don't bias it.
  if (cycles >= 2 && lastCycleUs > firstCycleUs) {
    mainsHz = (float)(cycles - 1) * 1e6f / (float)(lastCycleUs - firstCycleUs);
  } else {
    mainsHz = 0.0f;
  }

  // First window: adopt the measured bias outright so hysteresis crossing
  // detection has a valid reference from the next window on.
  biasMv = (windowsDone == 0) ? biasMvInstant : biasMv * 0.5f + biasMvInstant * 0.5f;
  windowMs = wholeCycles ? (lastCycleUs - firstCycleUs) / 1000 : elapsedUs / 1000;
  ++windowsDone;
  count = 0;  // restart window
  firstEdgeIdx = 0;
  haveFirstEdge = false;
  cycles = 0;
  crossAbove = false;
}

// Sample for ~burstMs then return (so loop() can keep serving HTTP).
inline void burst(uint32_t burstMs = 20) {
  const uint32_t end = millis() + burstMs;
  while (millis() < end && count < kMaxSamples) {
    const bool firstSample = (count == 0);
    if (firstSample) windowStartUs = micros();

    const float mv = (float)analogReadMilliVolts(kPin);
    lpfState += kLpfAlpha * (mv - lpfState);
    samples[count++] = (int16_t)lpfState;

    if (firstSample) {
      // Seed the hysteresis state so a mid-cycle start doesn't count a phantom edge.
      crossAbove = lpfState > biasMv + (float)kCrossHystMv;
    } else if (biasMv > kBiasValidMv && vrmsMv > kEdgeMinVrmsMv) {
      if (!crossAbove && lpfState > biasMv + (float)kCrossHystMv) {
        crossAbove = true;
        if (!haveFirstEdge) {
          haveFirstEdge = true;
          firstEdgeIdx = count - 1;
          firstCycleUs = micros();
        }
        lastCycleUs = micros();
        ++cycles;
        if (cycles >= kEdgesPerWindow) break;   // window complete — stop sampling
      } else if (crossAbove && lpfState < biasMv - (float)kCrossHystMv) {
        crossAbove = false;
      }
    }
  }
  if (cycles >= kEdgesPerWindow || count >= kMaxSamples) finalize();
}

inline float voltageV() { return rmsVolts; }
inline float vrmsMillivolts() { return vrmsMv; }
inline float biasMillivolts() { return biasMv; }
inline int16_t minMillivolts() { return minMv; }
inline int16_t maxMillivolts() { return maxMv; }
inline uint16_t zeroCrossings() { return crossings; }
inline float measuredMainsHz() { return mainsHz; }

}  // namespace voltage
