// Minimal host declarations for exercising the actual sampler's window
// finalization. These tests do not emulate ADC timing, WiFi, or I2C hardware.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define abs(value) ((value) < 0 ? -(value) : (value))
static const int ADC_11db = 3;
static const int INPUT_PULLUP = 2;

extern uint32_t testNowMs;
inline uint32_t millis() { return testNowMs; }
inline uint32_t micros() { return testNowMs * 1000; }
inline void pinMode(int, int) {}
inline void delay(uint32_t) {}
inline void delayMicroseconds(uint32_t) {}
inline void analogReadResolution(int) {}
inline void analogSetPinAttenuation(int, int) {}
inline uint32_t analogReadMilliVolts(int) { return 0; }
