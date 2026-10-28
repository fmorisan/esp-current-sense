// Concurrent V+I sampler — voltage on GPIO34 (ZMPT101B), current on the
// ADS1115 (SCT-013 through the series-cap divider).
//
// The point of the split architecture: while the ADS1115 performs one
// conversion (~1.16 ms aperture at 860 SPS), the ESP32 samples the voltage
// channel continuously. That yields a time-aligned pair per ADS cycle and a
// continuous voltage stream for whole-cycle window closure — neither channel
// ever goes unsampled, and no zero-crossing is missed between bursts.
//
// Each ADS conversion is a delta-sigma average over the aperture, equivalent
// to the instantaneous value at the aperture centre scaled by sinc(πfT)
// (~0.95 at 50 Hz). The scaling is absorbed by kAmpsPerMv; the centre time
// keeps the phase relationship intact. The voltage channel's 300 Hz digital
// LPF and the CT node's 100 nF RC have nearly equal group delay (~0.53 ms),
// so the two channels' phase lags cancel and PF comes out right without
// extra compensation.
//
// Statistics per window (DC removed):
//   V_rms  = sqrt(E[v²] − E[v]²) × kVoltsPerMv
//   I_rms  = sqrt(E[i²] − E[i]²) × kAmpsPerMv
//   P_real = kCurrentPolarity × (E[v·i] − E[v]E[i]) × kVoltsPerMv × kAmpsPerMv
//   S      = V_rms × I_rms
//   PF     = P_real / S
#pragma once

#include <Arduino.h>

#include "adc_samples.h"
#include "ads1115.h"
#include "current_polarity.h"

namespace power {

// ---- Voltage channel (ZMPT101B on GPIO34) ----
static const uint8_t kVoltPin = 34;
// Calibrated against a multimeter: 220 V mains <-> ~281.6 mV RMS at the node.
static const float kVoltsPerMv = 0.688f;
// Digital single-pole low-pass: alpha ≈ 1 − e^(−2π·fc/fs), fc = 300 Hz,
// fs ≈ 20 kS/s. At 50 Hz it attenuates ~1.35 % (absorbed by the calibration)
// and lags ~9.5° — matched by the CT node's 100 nF RC for PF symmetry.
static const float kVoltLpfAlpha = 0.090f;
static const float kVoltNoiseFloorMv = 3.0f;

// ---- Current channel (SCT-013 through the ADS1115) ----
// 100 A / 1 V clamp through the series-cap divider. The ADS1115's 860 SPS
// sinc filter attenuates 50 Hz by ~5 %, so this constant is the SAR-derived
// 127 A/V divided by ~0.95. Verify against a kettle after wiring.
static const float kAmpsPerMv = 0.1316f;
static const float kCurrentNoiseFloorA = 0.05f;

// ---- Window ----
static const uint16_t kEdgesPerWindow = 9;      // 9 rising edges = 8 whole cycles
static const int16_t kCrossHystMv = 45;         // hysteresis band around bias
static const float kBiasValidMv = 500.0f;       // edges need a real bias
static const float kEdgeMinVrmsMv = 60.0f;      // edges only when signal is real
static const uint32_t kMaxWindowMs = 1000;      // fallback if edges never close
static const uint32_t kApertureSampleUs = 1200; // sample V until this point (conversion ≈1.16 ms)

// ---- Accumulators (voltage-only, current-only, paired) ----
static double vSum = 0, vSumSq = 0;
static uint32_t vN = 0;
static double iSum = 0, iSumSq = 0;
static uint32_t iN = 0;
static double pSumV = 0, pSumI = 0, pSumVI = 0;
static uint32_t pN = 0;
static int16_t vMinMv = 32767, vMaxMv = -32768;
static int16_t iMinRaw = 32767, iMaxRaw = -32768;

// ---- Window state ----
static uint32_t windowStartMs = 0;
static uint32_t firstEdgeUs = 0, lastEdgeUs = 0;
static uint16_t edges = 0;
static bool haveFirstEdge = false;
static bool crossAbove = false;
static bool needSeed = true;
static float lpfState = 1650.0f;

// ---- Results ----
static float vBiasMv = 0, vRmsMv = 0, vRms = 0;
static float iBiasMv = 0, iRmsMv = 0, iRmsA = 0;
static float pRealW = 0, pApparentVa = 0, pf = 0;
static float mainsHz = 0;
static uint32_t windowMs = 0;
static size_t windowsDone = 0;
static samples::Buffer sampleBuffer;  // recent ADC pairs, independent of RMS windows

inline void setup() {
  analogReadResolution(12);
  analogSetPinAttenuation(kVoltPin, ADC_11db);
  ads::setup();
  windowStartMs = millis();
}

static void resetAccum() {
  vSum = vSumSq = 0; vN = 0;
  iSum = iSumSq = 0; iN = 0;
  pSumV = pSumI = pSumVI = 0; pN = 0;
  vMinMv = 32767; vMaxMv = -32768;
  iMinRaw = 32767; iMaxRaw = -32768;
}

static void finalize() {
  const bool wholeCycles = haveFirstEdge && edges >= kEdgesPerWindow;
  if (vN == 0 || iN == 0) { windowStartMs = millis(); return; }

  const double vNd = (double)vN;
  const double iNd = (double)iN;
  vBiasMv = (float)(vSum / vNd);
  iBiasMv = (float)(iSum / iNd);

  const double varV = vSumSq / vNd - (double)vBiasMv * vBiasMv;
  const double varI = iSumSq / iNd - (double)iBiasMv * iBiasMv;
  vRmsMv = varV > 0 ? (float)sqrt(varV) : 0.0f;
  iRmsMv = varI > 0 ? (float)sqrt(varI) : 0.0f;

  vRms = vRmsMv < kVoltNoiseFloorMv ? 0.0f : vRmsMv * kVoltsPerMv;
  iRmsA = iRmsMv * kAmpsPerMv;
  if (iRmsA < kCurrentNoiseFloorA) iRmsA = 0.0f;

  // Keep raw accumulators/bias/RMS unchanged. Applying polarity after DC
  // removal is equivalent to inverting only the current's AC component.
  double cov = 0.0;
  if (pN > 0) {
    const double pNd = (double)pN;
    cov = pSumVI / pNd - (pSumV / pNd) * (pSumI / pNd);
  }
  pRealW = (float)(kCurrentPolarity * cov * (double)kVoltsPerMv * (double)kAmpsPerMv);
  pApparentVa = vRms * iRmsA;
  pf = pApparentVa > 0.5f ? pRealW / pApparentVa : 0.0f;
  if (pf > 1.0f) pf = 1.0f;
  if (pf < -1.0f) pf = -1.0f;

  // With no real clamp/voltage signal the covariance is pure noise; gate it so
  // a floating channel can't integrate phantom energy (either sign) into the
  // HA total_increasing meter.
  if (iRmsA == 0.0f || vRms == 0.0f) {
    pRealW = 0.0f;
    pApparentVa = 0.0f;
    pf = 0.0f;
  }

  mainsHz = (wholeCycles && lastEdgeUs > firstEdgeUs)
                ? (float)(edges - 1) * 1e6f / (float)(lastEdgeUs - firstEdgeUs)
                : 0.0f;
  windowMs = wholeCycles ? (lastEdgeUs - firstEdgeUs) / 1000
                         : (millis() - windowStartMs);

  // Bias for the next window's hysteresis detection: first window adopts the
  // measured bias outright, afterwards a 50/50 EMA tracks slow drift.
  vBiasMv = (windowsDone == 0) ? vBiasMv : vBiasMv * 0.5f + vBiasMv * 0.5f;

  ++windowsDone;
  resetAccum();
  haveFirstEdge = false;
  edges = 0;
  crossAbove = false;
  needSeed = true;
  windowStartMs = millis();
}

// Sample for ~burstMs then return (so loop() can keep serving HTTP).
inline void burst(uint32_t burstMs = 20) {
  if (!ads::present()) return;
  const uint32_t end = millis() + burstMs;
  while (millis() < end) {
    ads::start();
    const uint32_t t0 = micros();          // conversion starts at the I2C stop
    const uint32_t tCenter = t0 + ads::kCenterUs;

    bool haveBefore = false, haveAfter = false;
    int16_t vBefore = 0, vAfter = 0;
    uint32_t tBefore = 0, tAfter = 0;
    uint16_t sampleVoltageMv = 0;
    uint32_t closestDistanceUs = UINT32_MAX;

    while (micros() - t0 < kApertureSampleUs) {
      const float mv = (float)analogReadMilliVolts(kVoltPin);
      lpfState += kVoltLpfAlpha * (mv - lpfState);
      const int16_t v = (int16_t)lpfState;
      const uint32_t t = micros();

      // Keep the unfiltered ADC reading nearest the current aperture centre.
      // Relative times also work when micros() wraps; no extra ADC reads.
      const uint32_t elapsedUs = t - t0;
      const uint32_t distanceUs = elapsedUs > ads::kCenterUs
                                     ? elapsedUs - ads::kCenterUs
                                     : ads::kCenterUs - elapsedUs;
      if (distanceUs < closestDistanceUs) {
        closestDistanceUs = distanceUs;
        sampleVoltageMv = (uint16_t)mv;
      }

      if (v < vMinMv) vMinMv = v;
      if (v > vMaxMv) vMaxMv = v;
      vSum += v;
      vSumSq += (double)v * v;
      ++vN;

      if (needSeed) {
        // Seed hysteresis state so a mid-cycle start doesn't count a phantom edge.
        crossAbove = (float)v > vBiasMv + (float)kCrossHystMv;
        needSeed = false;
      } else if (vBiasMv > kBiasValidMv && vRmsMv > kEdgeMinVrmsMv) {
        if (!crossAbove && (float)v > vBiasMv + (float)kCrossHystMv) {
          crossAbove = true;
          if (!haveFirstEdge) {
            haveFirstEdge = true;
            firstEdgeUs = t;
            resetAccum();   // discard the partial preamble, keep whole cycles only
          }
          lastEdgeUs = t;
          ++edges;
        } else if (crossAbove && (float)v < vBiasMv - (float)kCrossHystMv) {
          crossAbove = false;
        }
      }

      if (t <= tCenter) { vBefore = v; tBefore = t; haveBefore = true; }
      else if (!haveAfter) { vAfter = v; tAfter = t; haveAfter = true; }
    }

    int16_t raw = 0;
    if (ads::readRaw(raw)) {
      const double iMv = (double)ads::rawToMv(raw);
      if (closestDistanceUs != UINT32_MAX) {
        sampleBuffer.record(sampleVoltageMv, (float)iMv);
      }
      iSum += iMv;
      iSumSq += iMv * iMv;
      ++iN;
      if (raw < iMinRaw) iMinRaw = raw;
      if (raw > iMaxRaw) iMaxRaw = raw;

      if (haveFirstEdge) {
        float v = (float)vBefore;
        if (haveBefore && haveAfter && tAfter > tBefore) {
          const float frac = (float)((int32_t)(tCenter - tBefore)) /
                             (float)((int32_t)(tAfter - tBefore));
          v = (float)vBefore + ((float)vAfter - (float)vBefore) * frac;
        }
        pSumV += v;
        pSumI += iMv;
        pSumVI += (double)v * iMv;
        ++pN;
      }
    }

    if (edges >= kEdgesPerWindow) { finalize(); return; }
  }
  if (millis() - windowStartMs >= kMaxWindowMs) finalize();
}

// Safe to serialize directly while HTTP is handled: acquisition is synchronous
// in loop(), so neither array can change partway through the response.
inline const samples::Buffer& recentSamples() { return sampleBuffer; }

inline float voltageV() { return vRms; }
inline float currentA() { return iRmsA; }
inline float realPowerW() { return pRealW; }
inline float apparentVa() { return pApparentVa; }
inline float powerFactor() { return pf; }
inline float vrmsMillivolts() { return vRmsMv; }
inline float biasMillivolts() { return vBiasMv; }
inline float ctVrmsMillivolts() { return iRmsMv; }
inline float ctBiasMillivolts() { return iBiasMv; }
inline float measuredMainsHz() { return mainsHz; }
inline int16_t vMinMillivolts() { return vMinMv; }
inline int16_t vMaxMillivolts() { return vMaxMv; }
inline int16_t ctMinCounts() { return iMinRaw; }
inline int16_t ctMaxCounts() { return iMaxRaw; }
inline uint32_t windowDurationMs() { return windowMs; }
inline size_t windowsDoneCount() { return windowsDone; }
inline bool adsPresent() { return ads::present(); }
inline uint32_t adsErrors() { return ads::errorCount(); }

}  // namespace power
