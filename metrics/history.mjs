export function validateMetrics(data) {
  if (!data || !Number.isFinite(data.power_w) || !Number.isFinite(data.power_va) || data.power_va < 0) {
    throw new Error('Expected numeric power_w and power_va readings from /metrics.');
  }
  return data;
}

// P and S alone give a non-active magnitude, not signed reactive power. For
// sinusoidal waveforms this is |Q|; with harmonics it includes distortion.
export function powerTriangle(watts, va) {
  if (!Number.isFinite(watts) || !Number.isFinite(va) || va < 0) {
    return { available: false, reason: 'Waiting for valid W and VA readings.' };
  }
  if (Math.abs(watts) > va) {
    return { available: false, reason: 'Measured |W| exceeds VA. Check sensor calibration; no valid power triangle can be drawn.' };
  }
  if (va === 0) {
    return { available: false, reason: 'No apparent power: a zero-VA reading cannot be normalized to a unit circle.' };
  }
  const realFraction = watts / va;
  // Normalize before squaring to avoid overflow for large, finite readings.
  const reactiveFraction = Math.sqrt(Math.max(0, 1 - realFraction * realFraction));
  return { available: true, realFraction, reactiveFraction, reactiveMagnitude: va * reactiveFraction };
}

export class History {
  constructor({ maxAgeMs = 30 * 60 * 1000, maxPoints = 1000 } = {}) {
    this.maxAgeMs = maxAgeMs;
    this.maxPoints = maxPoints;
    this.points = [];
    this.gap = false;
  }

  add(data, time) {
    validateMetrics(data);
    const previous = this.points.at(-1);
    if (!Number.isFinite(time) || (previous && time <= previous.time)) return false;
    const reboot = Number.isFinite(previous?.uptime) && Number.isFinite(data.uptime_s)
      && data.uptime_s < previous.uptime;
    this.points.push({
      time, watts: data.power_w, va: data.power_va, uptime: data.uptime_s,
      breakBefore: this.gap || reboot || Boolean(previous && time - previous.time > 10000),
    });
    this.gap = false;
    while (this.points.length > this.maxPoints || this.points[0].time < time - this.maxAgeMs) {
      this.points.shift();
    }
    return true;
  }

  clear() {
    this.points = [];
    this.gap = false;
  }
}

export function segments(points) {
  const result = [];
  for (const point of points) {
    if (!result.length || point.breakBefore) result.push([]);
    result.at(-1).push(point);
  }
  return result;
}
