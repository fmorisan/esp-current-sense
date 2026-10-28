// One polarity convention for signed power and the displayed current waveform.
#pragma once

namespace power {

// -1 corrects this installation's reversed current-sensor polarity. Use +1
// after physically reversing the CT/reference polarity; do not negate the gain.
static const int kCurrentPolarity = -1;
static_assert(kCurrentPolarity == -1 || kCurrentPolarity == 1,
              "Current polarity must be -1 or +1");

inline float correctCurrentMillivolts(float measuredMv, float biasMv) {
  return biasMv + kCurrentPolarity * (measuredMv - biasMv);
}

}  // namespace power
