// Bounded, paired ADC history. Independent of Arduino so buffer ordering and
// JSON output can be tested on the host. Capture and HTTP run in the same loop.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "current_polarity.h"

namespace samples {

class Buffer {
 public:
  static const size_t kCapacity = 256;

  void record(uint16_t voltageMv, float currentMv) {
    data_[next_] = {voltageMv, currentMv};
    next_ = (next_ + 1) % kCapacity;
    if (count_ < kCapacity) ++count_;
  }

  // Emit small, null-terminated chunks rather than allocating a whole response.
  // The caller must not record new samples until this synchronous call returns.
  template <typename Send>
  void writeJson(Send send, bool rawCurrent = false) const {
    // Reflect the waveform about this snapshot's own mean, preserving its DC
    // bias even before the first complete power window. Never alter raw history.
    double currentSum = 0;
    for (size_t i = 0; i < count_; ++i) currentSum += data_[i].currentMv;
    const float currentBias = count_ ? (float)(currentSum / count_) : 0.0f;
    char chunk[512];
    for (unsigned channel = 0; channel < 2; ++channel) {
      send(channel == 0 ? "{\"voltage_mv\":[" : "],\"current_mv\":[");
      size_t used = 0;
      for (size_t i = 0; i < count_; ++i) {
        // Enough room for a separator and either ADC's millivolt value.
        if (sizeof(chunk) - used < 32) {
          send(chunk);
          used = 0;
        }
        const Sample& sample = data_[(next_ + kCapacity - count_ + i) % kCapacity];
        if (channel == 0) {
          used += snprintf(chunk + used, sizeof(chunk) - used, "%s%u",
                           i ? "," : "", (unsigned)sample.voltageMv);
        } else {
          const float currentMv = rawCurrent ? sample.currentMv
              : power::correctCurrentMillivolts(sample.currentMv, currentBias);
          used += snprintf(chunk + used, sizeof(chunk) - used, "%s%.3f",
                           i ? "," : "", (double)currentMv);
        }
      }
      if (used) send(chunk);
    }
    send("]}");
  }

 private:
  struct Sample {
    uint16_t voltageMv;
    float currentMv;  // ADS1115 ADC-input mV, not calibrated mains current
  };

  Sample data_[kCapacity] = {};
  size_t next_ = 0;
  size_t count_ = 0;
};

}  // namespace samples
