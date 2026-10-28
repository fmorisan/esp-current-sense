import assert from 'node:assert/strict';
import test from 'node:test';
import { History, powerTriangle, segments, validateMetrics } from '../metrics/history.mjs';

const reading = { power_w: 910, power_va: 926.2, uptime_s: 100 };

test('power triangle normalizes the real and estimated reactive components to the unit circle', () => {
  const triangle = powerTriangle(3, 5);
  assert.equal(triangle.available, true);
  assert.equal(triangle.realFraction, 0.6);
  assert.equal(triangle.reactiveFraction, 0.8);
  assert.equal(triangle.reactiveMagnitude, 4);
  assert.ok(Math.abs(Math.hypot(triangle.realFraction, triangle.reactiveFraction) - 1) < 1e-12);
});

test('resistive and zero-real-power loads lie on the circle axes', () => {
  const resistive = powerTriangle(500, 500);
  assert.equal(resistive.realFraction, 1);
  assert.equal(resistive.reactiveFraction, 0);
  assert.equal(resistive.reactiveMagnitude, 0);
  const noRealPower = powerTriangle(0, 500);
  assert.equal(noRealPower.realFraction, 0);
  assert.equal(noRealPower.reactiveFraction, 1);
  assert.equal(noRealPower.reactiveMagnitude, 500);
});

test('the triangle preserves real-power direction without inventing reactive sign', () => {
  const triangle = powerTriangle(-3, 5);
  assert.equal(triangle.realFraction, -0.6);
  assert.equal(triangle.reactiveFraction, 0.8);
  assert.equal(triangle.reactiveMagnitude, 4);
});

test('zero load and inconsistent or missing readings do not produce a misleading triangle', () => {
  assert.match(powerTriangle(0, 0).reason, /zero-VA/);
  assert.match(powerTriangle(101, 100).reason, /exceeds VA/);
  for (const [watts, va] of [[0, 0], [101, 100], [-101, 100], [1, 0], [0, -1],
    [undefined, undefined], [NaN, 100], [10, Infinity]]) {
    const triangle = powerTriangle(watts, va);
    assert.equal(triangle.available, false);
    assert.equal(triangle.reactiveMagnitude, undefined);
  }
});

test('normalization remains finite for very small and very large finite readings', () => {
  for (const scale of [1e-200, 1e200]) {
    const triangle = powerTriangle(3 * scale, 5 * scale);
    assert.equal(triangle.available, true);
    assert.ok(Number.isFinite(triangle.reactiveMagnitude));
    assert.ok(Math.abs(triangle.reactiveMagnitude / (4 * scale) - 1) < 1e-12);
  }
});

test('accepts W and VA and rejects invalid readings without substituting VA for W', () => {
  assert.equal(validateMetrics(reading), reading);
  assert.equal(validateMetrics({ power_w: 0, power_va: 0 }).power_w, 0);
  for (const invalid of [null, {}, { power_va: 100 }, { power_w: '100', power_va: 100 },
    { power_w: NaN, power_va: 100 }, { power_w: 100, power_va: Infinity },
    { power_w: 100, power_va: -1 }]) {
    assert.throws(() => validateMetrics(invalid));
  }
});

test('preserves both power series and their read times', () => {
  const history = new History();
  history.add(reading, 1000);
  history.add({ ...reading, power_w: 200, power_va: 220 }, 3000);
  assert.deepEqual(history.points.map(point => [point.time, point.watts, point.va]),
    [[1000, 910, 926.2], [3000, 200, 220]]);
  assert.equal(segments(history.points).length, 1);
});

test('limits history by age and number of readings', () => {
  const history = new History({ maxAgeMs: 5000, maxPoints: 3 });
  for (const time of [1000, 2000, 3000, 4000]) history.add(reading, time);
  assert.deepEqual(history.points.map(point => point.time), [2000, 3000, 4000]);
  history.add(reading, 10000);
  assert.deepEqual(history.points.map(point => point.time), [10000]);
});

test('pauses, failed reads, long gaps and rebooted devices break the plotted lines', () => {
  const history = new History();
  history.add(reading, 1000);
  history.gap = true;
  history.add(reading, 3000);
  history.add(reading, 15000);
  history.add({ ...reading, uptime_s: 1 }, 17000);
  assert.equal(segments(history.points).length, 4);
});

test('rejects backward or non-finite timestamps and clears both series together', () => {
  const history = new History();
  history.add(reading, 2000);
  assert.equal(history.add(reading, 1000), false);
  assert.equal(history.add(reading, NaN), false);
  assert.equal(history.points.length, 1);
  history.gap = true;
  history.clear();
  assert.deepEqual(history.points, []);
  assert.equal(history.gap, false);
});

test('page starts live polling automatically, pauses cleanly, and resumes without overlapping', async t => {
  class Element {
    constructor() {
      this.value = '';
      this.textContent = '';
      this.children = [];
      this.handlers = {};
      this.disabled = false;
      this.attributes = {};
      this.classList = { toggle() {} };
    }
    setAttribute(key, value) { this.attributes[key] = value; }
    append(node) { this.children.push(node); }
    replaceChildren() { this.children = []; }
    addEventListener(event, handler) { this.handlers[event] = handler; }
    requestSubmit() { this.handlers.submit({ preventDefault() {} }); }
  }
  const elements = new Map();
  const get = id => {
    if (!elements.has(id)) elements.set(id, new Element());
    return elements.get(id);
  };
  get('board').value = '192.168.0.158';
  const globals = ['document', 'localStorage', 'setTimeout', 'clearTimeout', 'fetch'];
  const descriptors = new Map(globals.map(key => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  t.after(() => {
    for (const [key, descriptor] of descriptors) {
      if (descriptor) Object.defineProperty(globalThis, key, descriptor);
      else delete globalThis[key];
    }
  });
  globalThis.document = { getElementById: get, createElementNS: () => new Element() };
  globalThis.localStorage = { getItem: () => null, setItem() {} };
  const timers = new Map();
  let nextTimer = 0;
  globalThis.setTimeout = (callback, delay) => { timers.set(++nextTimer, { callback, delay }); return nextTimer; };
  globalThis.clearTimeout = id => timers.delete(id);
  const requests = [];
  globalThis.fetch = (url, options) => new Promise((resolve, reject) => {
    requests.push({ url, resolve, options });
    options.signal.addEventListener('abort', () => reject(new Error('aborted')));
  });
  const settle = async () => { for (let i = 0; i < 8; i++) await Promise.resolve(); };

  await import('../metrics/app.mjs');
  assert.equal(requests.length, 1, 'opening the page starts fetching');
  assert.equal(requests[0].url, '/metrics?board=192.168.0.158');
  assert.equal(get('start').disabled, true);
  assert.equal([...timers.values()].some(timer => timer.delay === 2000), false,
    'a second poll cannot start before the first finishes');

  requests[0].resolve({ ok: true, json: async () => reading });
  await settle();
  assert.equal(get('watts').textContent, '910.0');
  assert.equal(get('va').textContent, '926.2');
  assert.equal(get('power-real').textContent, '910.0 W');
  assert.equal(get('power-apparent').textContent, '926.2 VA');
  assert.equal(get('power-reactive').textContent,
    `${Math.sqrt(reading.power_va ** 2 - reading.power_w ** 2).toFixed(1)} var`);
  const vector = get('power-diagram').children.find(node => node.attributes.id === 'power-vector');
  assert.ok(vector, 'the live reading draws an apparent-power vector');
  const { x1, x2, y1, y2 } = vector.attributes;
  assert.ok(Math.abs(Math.hypot(x2 - x1, y2 - y1) - 170) < 1e-10,
    'the apparent-power vector has the unit-circle radius');
  assert.match(get('status').textContent, /Tracking/);
  const pollTimer = [...timers.entries()].find(([, timer]) => timer.delay === 2000);
  assert.ok(pollTimer);
  timers.delete(pollTimer[0]);
  pollTimer[1].callback();
  assert.equal(requests.length, 2);

  get('pause').handlers.click();
  await settle();
  assert.equal(requests[1].options.signal.aborted, true);
  assert.equal(timers.size, 0);
  assert.match(get('status').textContent, /Paused/);
  assert.equal(get('watts').textContent, '910.0', 'pausing retains the last values');
  assert.equal(get('power-real').textContent, '910.0 W', 'pausing retains the last diagram');

  get('board').value = '192.168.0.159';
  get('connect-form').requestSubmit();
  assert.equal(requests.length, 3);
  assert.match(requests[2].url, /192.168.0.159/);
  assert.equal(get('watts').textContent, '—', 'changing boards clears stale values');
  assert.equal(get('power-real').textContent, '— W', 'changing boards clears the previous triangle');
  assert.equal(get('power-diagram').children.some(node => node.attributes.id === 'power-vector'), false);
  requests[2].resolve({ ok: false, json: async () => ({ error: 'Board unreachable' }) });
  await settle();
  assert.match(get('status').textContent, /Board unreachable.*Retrying/);
  get('pause').handlers.click();
  await settle();
  assert.equal(timers.size, 0);
});
