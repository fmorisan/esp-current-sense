import assert from 'node:assert/strict';
import test from 'node:test';
import { prepareChannel, validateSamples } from '../scope/samples.mjs';

test('accepts paired samples, including an empty snapshot', () => {
  for (const data of [
    { voltage_mv: [], current_mv: [] },
    { voltage_mv: [1650, 1702], current_mv: [1651.25, 1658.5] },
    { voltage_mv: Array(256).fill(1650), current_mv: Array(256).fill(1651.25) },
  ]) {
    assert.equal(validateSamples(data), data);
  }
});

test('rejects missing, mismatched, oversized, or nonnumeric samples', () => {
  for (const data of [
    null, {}, { voltage_mv: [] },
    { voltage_mv: [1], current_mv: [] },
    { voltage_mv: ['1650'], current_mv: [1650] },
    { voltage_mv: [null], current_mv: [1650] },
    { voltage_mv: [NaN], current_mv: [1650] },
    { voltage_mv: [1650], current_mv: [Infinity] },
    { voltage_mv: Array(257).fill(1), current_mv: Array(257).fill(2) },
  ]) {
    assert.throws(() => validateSamples(data));
  }
});

test('removes bias without changing the original values or sample ordering', () => {
  const values = [1600, 1650, 1700];
  const channel = prepareChannel(values, true);
  assert.equal(channel.bias, 1650);
  assert.deepEqual(channel.values, [-50, 0, 50]);
  assert.deepEqual(values, [1600, 1650, 1700]);
  assert.ok(channel.min <= -50 && channel.max >= 50);
  assert.equal(channel.min, -channel.max);
});

test('raw mode preserves ADC levels and both axes scale independently', () => {
  const voltage = prepareChannel([1000, 1650, 2300], false);
  const current = prepareChannel([1649, 1650, 1651], false);
  assert.deepEqual(voltage.values, [1000, 1650, 2300]);
  assert.ok(voltage.min < 1000 && voltage.max > 2300);
  assert.ok(current.min < 1649 && current.max > 1651);
  assert.ok(current.max - current.min < voltage.max - voltage.min);
});

test('empty, single-sample and constant channels always have finite, nonzero scales', () => {
  for (const values of [[], [1650], [1650, 1650, 1650]]) {
    for (const center of [true, false]) {
      const channel = prepareChannel(values, center);
      assert.ok(Number.isFinite(channel.min) && Number.isFinite(channel.max));
      assert.ok(channel.max > channel.min);
    }
  }
});
