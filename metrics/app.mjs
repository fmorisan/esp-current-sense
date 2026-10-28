import { History, powerTriangle, segments, validateMetrics } from './history.mjs';

const $ = id => document.getElementById(id);
const history = new History();
const chart = $('chart');
const box = { left: 76, right: 884, top: 30, bottom: 320 };
const colors = { watts: '#4ade80', va: '#7dd3fc', reactive: '#fbbf24', grid: '#293142', text: '#9aa4b2' };
let running = false;
let generation = 0;
let timer;
let request;
let activeBoard = '';
let lastRead = '';
let domain = { start: 0, end: 1 };

try { $('board').value = localStorage.getItem('metrics-board') || localStorage.getItem('scope-board') || $('board').value; } catch { /* Optional storage. */ }

function element(tag, attributes = {}, text, parent = chart) {
  const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, value);
  if (text !== undefined) node.textContent = text;
  parent.append(node);
  return node;
}

function label(x, y, text, color = colors.text, anchor = 'middle') {
  element('text', { x, y, fill: color, 'font-size': 13, 'text-anchor': anchor }, text);
}

function timeLabel(time) {
  return new Date(time).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

function render() {
  const points = history.points;
  const end = points.at(-1)?.time ?? Date.now();
  const start = Math.min(points[0]?.time ?? end - 20000, end - 20000);
  domain = { start, end };
  const readings = points.flatMap(point => [point.watts, point.va]);
  const min = Math.min(0, ...readings) * 1.1;
  const max = Math.max(10, ...readings) * 1.1;
  const x = time => box.left + (time - start) / (end - start) * (box.right - box.left);
  const y = value => box.bottom - (value - min) / (max - min) * (box.bottom - box.top);
  chart.replaceChildren();
  element('title', { id: 'chart-title' }, `Real power in W and apparent power in VA, ${points.length} readings`);
  label(box.left, 17, 'W', colors.watts);
  label(box.right, 17, 'VA', colors.va);
  for (let tick = 0; tick <= 4; tick++) {
    const fraction = tick / 4;
    const value = min + fraction * (max - min);
    const top = y(value);
    element('line', { x1: box.left, x2: box.right, y1: top, y2: top, stroke: colors.grid });
    label(box.left - 12, top + 4, value.toFixed(0), colors.watts, 'end');
    label(box.right + 12, top + 4, value.toFixed(0), colors.va, 'start');
    if (points.length) label(x(start + fraction * (end - start)), box.bottom + 24, timeLabel(start + fraction * (end - start)));
  }
  for (const key of ['va', 'watts']) {
    for (const part of segments(points)) {
      element('polyline', {
        points: part.map(point => `${x(point.time).toFixed(2)},${y(point[key]).toFixed(2)}`).join(' '),
        fill: 'none', stroke: colors[key], 'stroke-width': 2,
        'stroke-dasharray': key === 'va' ? '7 4' : 'none', 'vector-effect': 'non-scaling-stroke',
      });
      if (part.length === 1) element('circle', { cx: x(part[0].time), cy: y(part[0][key]), r: 3, fill: colors[key] });
    }
  }
  if (!points.length) label(480, 175, 'Start tracking to plot W and VA over time');
  label(480, 381, 'Local time · latest 30 minutes · matching numeric scales');
  const watts = points.map(point => point.watts);
  $('summary').textContent = points.length
    ? `${points.length} readings · Mean W: ${(watts.reduce((sum, value) => sum + value, 0) / watts.length).toFixed(1)} · Min: ${Math.min(...watts).toFixed(1)} W · Max: ${Math.max(...watts).toFixed(1)} W`
    : 'No readings yet.';
  $('inspect').textContent = 'Move over the chart to inspect a reading.';
}

chart.addEventListener('pointermove', event => {
  if (!history.points.length) return;
  const bounds = chart.getBoundingClientRect();
  const position = (event.clientX - bounds.left) / bounds.width * 960;
  const time = domain.start + (position - box.left) / (box.right - box.left) * (domain.end - domain.start);
  const point = history.points.reduce((nearest, candidate) =>
    Math.abs(candidate.time - time) < Math.abs(nearest.time - time) ? candidate : nearest);
  $('inspect').textContent = `${timeLabel(point.time)} · ${point.watts.toFixed(1)} W · ${point.va.toFixed(1)} VA`;
});

function status(message, error = false) {
  $('status').textContent = message;
  $('status').classList.toggle('error', error);
}

function renderPowerDiagram(data) {
  const diagram = $('power-diagram');
  const triangle = powerTriangle(data.power_w, data.power_va);
  const center = 240;
  const radius = 170;
  diagram.replaceChildren();
  const add = (tag, attributes, text) => element(tag, attributes, text, diagram);
  const text = (x, y, value, color = colors.text, anchor = 'middle') =>
    add('text', { x, y, fill: color, 'font-size': 13, 'text-anchor': anchor }, value);
  add('title', { id: 'power-title' }, 'Apparent power normalized to a unit circle');
  add('desc', { id: 'power-description' }, triangle.available
    ? `Real power ${data.power_w.toFixed(1)} W, apparent power ${data.power_va.toFixed(1)} VA, estimated non-active magnitude ${triangle.reactiveMagnitude.toFixed(1)} var. Reactive sign is unknown.`
    : triangle.reason);
  add('circle', { cx: center, cy: center, r: radius, fill: 'none', stroke: '#465269', 'stroke-width': 1.5 });
  add('circle', { cx: center, cy: center, r: radius / 2, fill: 'none', stroke: colors.grid, 'stroke-dasharray': '3 5' });
  add('line', { x1: 40, x2: 440, y1: center, y2: center, stroke: '#465269' });
  add('line', { x1: center, x2: center, y1: 40, y2: 440, stroke: '#465269' });
  text(438, center - 12, 'P / S', colors.watts);
  text(center, 25, '|Q| / S (estimate)', colors.reactive);
  text(center - radius, center + 22, '−1');
  text(center + radius, center + 22, '1');
  text(center - 12, center + 20, '0');
  text(center - 16, center - radius + 4, '1', colors.text, 'end');
  text(center - 16, center - radius / 2 + 4, '0.5', colors.text, 'end');
  text(center, 466, 'Circle radius = S / S = 1 · reactive sign unknown');

  $('power-real').textContent = Number.isFinite(data.power_w) ? `${data.power_w.toFixed(1)} W` : '— W';
  $('power-apparent').textContent = Number.isFinite(data.power_va) ? `${data.power_va.toFixed(1)} VA` : '— VA';
  $('power-reactive').textContent = triangle.available ? `${triangle.reactiveMagnitude.toFixed(1)} var` : '— var';
  $('power-fraction').textContent = triangle.available ? triangle.realFraction.toFixed(3) : '—';
  $('power-notice').textContent = triangle.available
    ? 'Latest successful reading. Both components are divided by S; the radius shows proportions, not load size.'
    : triangle.reason;
  if (!triangle.available) {
    add('circle', { cx: center, cy: center, r: 4, fill: colors.text });
    return;
  }
  const x = center + radius * triangle.realFraction;
  const y = center - radius * triangle.reactiveFraction;
  add('polygon', { points: `${center},${center} ${x},${center} ${x},${y}`, fill: colors.va, 'fill-opacity': 0.08 });
  add('line', { id: 'power-real-component', x1: center, x2: x, y1: center, y2: center,
    stroke: colors.watts, 'stroke-width': 4 });
  add('line', { id: 'power-reactive-component', x1: x, x2: x, y1: center, y2: y,
    stroke: colors.reactive, 'stroke-width': 4 });
  add('line', { x1: center, x2: x, y1: y, y2: y, stroke: colors.reactive,
    'stroke-opacity': 0.5, 'stroke-dasharray': '4 4' });
  add('line', { id: 'power-vector', x1: center, x2: x, y1: center, y2: y,
    stroke: colors.va, 'stroke-width': 2.5 });
  add('circle', { cx: x, cy: y, r: 5, fill: colors.va });
}

function updateTiles(data) {
  for (const [id, key, digits] of [
    ['watts', 'power_w', 1], ['va', 'power_va', 1], ['pf', 'pf', 3],
    ['amps', 'current_a', 2], ['volts', 'voltage_v', 1], ['energy', 'energy_kwh', 4],
  ]) $(id).textContent = Number.isFinite(data[key]) ? data[key].toFixed(digits) : '—';
  renderPowerDiagram(data);
}

async function poll(version) {
  if (!running || version !== generation) return;
  const controller = new AbortController();
  request = controller;
  const timeout = setTimeout(() => controller.abort(), 8000);
  try {
    const response = await fetch(`/metrics?${new URLSearchParams({ board: activeBoard })}`, {
      signal: controller.signal, cache: 'no-store',
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
    validateMetrics(data);
    if (!running || version !== generation) return;
    if (data.ads_ok === false) throw new Error('ADS1115 is missing; power readings may be invalid.');
    history.add(data, Date.now());
    updateTiles(data);
    render();
    lastRead = timeLabel(Date.now());
    status(`Tracking ${activeBoard} · Last reading ${lastRead} · Refreshing every 2 seconds.`);
  } catch (error) {
    if (!running || version !== generation) return;
    history.gap = true;
    const reason = controller.signal.aborted ? 'Request timed out.' : error.message;
    status(`${reason} Retrying in 2 seconds.${lastRead ? ` Displayed values are from ${lastRead}.` : ''}`, true);
  } finally {
    clearTimeout(timeout);
    if (request === controller) request = null;
    if (running && version === generation) timer = setTimeout(() => poll(version), 2000);
  }
}

$('connect-form').addEventListener('submit', event => {
  event.preventDefault();
  if (running) return;
  const board = $('board').value.trim();
  if (!board) return;
  if (activeBoard && board !== activeBoard) {
    history.clear();
    lastRead = '';
    updateTiles({});
    render();
  }
  activeBoard = board;
  running = true;
  generation++;
  $('board').disabled = true;
  $('start').disabled = true;
  $('pause').disabled = false;
  try { localStorage.setItem('metrics-board', board); } catch { /* Optional storage. */ }
  status(`Connecting to ${activeBoard}…`);
  poll(generation);
});

$('pause').addEventListener('click', () => {
  running = false;
  generation++;
  clearTimeout(timer);
  request?.abort();
  history.gap = true;
  $('board').disabled = false;
  $('start').disabled = false;
  $('start').textContent = 'Start / resume';
  $('pause').disabled = true;
  status(`Paused. Chart retained; no requests are being made.${lastRead ? ` Last reading ${lastRead}.` : ''}`);
});

$('clear').addEventListener('click', () => { history.clear(); render(); });
renderPowerDiagram({});
render();
$('connect-form').requestSubmit();
