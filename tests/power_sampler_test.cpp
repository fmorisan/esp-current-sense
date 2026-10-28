#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../firmware/power_sampler.h"

uint32_t testNowMs = 160;

static void finishWindow(float rawPolarity, float voltageBias = 1650.0f,
                         float currentBias = 1610.0f, float currentScale = 0.1f) {
  const int ac[] = {0, 100, 200, 100, 0, -100, -200, -100};
  power::resetAccum();
  for (int value : ac) {
    const double v = voltageBias + value;
    const double i = currentBias + rawPolarity * currentScale * value;
    power::vSum += v;
    power::vSumSq += v * v;
    ++power::vN;
    power::iSum += i;
    power::iSumSq += i * i;
    ++power::iN;
    power::pSumV += v;
    power::pSumI += i;
    power::pSumVI += v * i;
    ++power::pN;
  }
  power::haveFirstEdge = true;
  power::edges = power::kEdgesPerWindow;
  power::firstEdgeUs = 1000;
  power::lastEdgeUs = 161000;
  power::finalize();
}

static void testReversedSensorImport() {
  finishWindow(-1.0f);
  assert(power::realPowerW() > 0.0f);
  assert(fabs(power::powerFactor() - 1.0f) < 1e-5f);
  assert(fabs(power::realPowerW() - power::apparentVa()) < 1e-3f);
}

static void testOppositePowerFlowRemainsNegative() {
  finishWindow(1.0f);
  assert(power::realPowerW() < 0.0f);
  assert(fabs(power::powerFactor() + 1.0f) < 1e-5f);
  assert(fabs(power::realPowerW() + power::apparentVa()) < 1e-3f);
}

static void testRmsAndRawBiasAreNotInverted() {
  finishWindow(-1.0f);
  const float amps = power::currentA();
  const float volts = power::voltageV();
  const float apparent = power::apparentVa();
  const float watts = power::realPowerW();
  assert(power::ctBiasMillivolts() == 1610.0f);
  assert(power::biasMillivolts() == 1650.0f);
  finishWindow(1.0f);
  assert(power::currentA() == amps);
  assert(power::voltageV() == volts);
  assert(power::apparentVa() == apparent);
  assert(power::ctBiasMillivolts() == 1610.0f);
  finishWindow(-1.0f, 1500.0f, 1800.0f);
  assert(fabs(power::realPowerW() - watts) < 1e-3f);
  assert(power::ctBiasMillivolts() == 1800.0f);
}

static void testNoiseGateStillZerosBothDirections() {
  const float directions[] = {-1.0f, 1.0f};
  for (float direction : directions) {
    finishWindow(direction, 1650.0f, 1610.0f, 0.0001f);
    assert(power::currentA() == 0.0f);
    assert(power::realPowerW() == 0.0f);
    assert(power::apparentVa() == 0.0f);
    assert(power::powerFactor() == 0.0f);
  }
}

// Replay a captured raw /samples response through the production covariance
// finalizer and serializer, without requiring a flash or emulating ADC timing.
// Input: pair count followed by whitespace-separated voltage/current mV pairs.
static void replayRawCapture() {
  unsigned count = 0;
  assert(scanf("%u", &count) == 1);
  assert(count > 0 && count <= samples::Buffer::kCapacity);
  power::resetAccum();
  for (unsigned n = 0; n < count; ++n) {
    double v, i;
    assert(scanf("%lf %lf", &v, &i) == 2);
    power::vSum += v;
    power::vSumSq += v * v;
    ++power::vN;
    power::iSum += i;
    power::iSumSq += i * i;
    ++power::iN;
    power::pSumV += v;
    power::pSumI += i;
    power::pSumVI += v * i;
    ++power::pN;
    power::sampleBuffer.record((uint16_t)v, (float)i);
  }
  power::finalize();
  printf("{\"power_w\":%.3f,\"pf\":%.3f,\"raw_bias_mv\":%.3f,\"samples\":",
         (double)power::realPowerW(), (double)power::powerFactor(),
         (double)power::ctBiasMillivolts());
  power::recentSamples().writeJson([](const char* chunk) { fputs(chunk, stdout); });
  puts("}");
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--replay") == 0) {
    replayRawCapture();
    return 0;
  }
  testReversedSensorImport();
  testOppositePowerFlowRemainsNegative();
  testRmsAndRawBiasAreNotInverted();
  testNoiseGateStillZerosBothDirections();
  puts("Power sampler polarity/sign tests passed");
}
