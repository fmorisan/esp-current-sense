#pragma once

#include <stdint.h>

class HostWire {
 public:
  void begin(int, int) {}
  void setClock(uint32_t) {}
  void beginTransmission(uint8_t) {}
  void write(uint8_t) {}
  uint8_t endTransmission(bool = true) { return 0; }
  uint8_t requestFrom(uint8_t, uint8_t count) { return count; }
  int read() { return 0; }
};

static HostWire Wire;
