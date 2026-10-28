import { prepareChannel, validateSamples } from './samples.mjs';

const $ = id => document.getElementById(id);
const chart = $('chart');
const colors = { voltage: '#7dd3fc', current: '#fbbf24', grid: '#293142', text: '#9aa4b2' };
const plot = { left: 88, right: 872, top: 28, bottom: 354 };
let snapshot = { voltage_mv: [], current_mv: [] };

try {
  $('board').value = localStorage.getItem('scope-board') || $('board').value;
} catch { /* Storage is optional, for example in private browsing. */ }

function element(tag, attributes = {}, text) {
  const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, value);
  if (text !== undefined) node.textContent = text;
  chart.append(node);
  return node;
}

function label(x, y, text, color = colors.text, anchor = 'middle') {
  element('text', { x, y, fill: color, 'font-size': 13, 'text-anchor': anchor }, text);
}

function drawSeries(channel, color) {
  const x = i => plot.left + i / Math.max(1, channel.values.length - 1) * (plot.right - plot.left);
  const y = value => plot.bottom - (value - channel.min) / (channel.max - channel.min) * (plot.bottom - plot.top);
  const points = channel.values.map((value, i) => `${x(i).toFixed(2)},${y(value).toFixed(2)}`).join(' ');
  element('polyline', { points, fill: 'none', stroke: color, 'stroke-width': 2,
    'vector-effect': 'non-scaling-stroke', 'stroke-linejoin': 'round' });
  if (channel.values.length === 1) {
    element('circle', { cx: x(0), cy: y(channel.values[0]), r: 3, fill: color });
  }
}

function render() {
  const voltage = prepareChannel(snapshot.voltage_mv, $('center').checked);
  const current = prepareChannel(snapshot.current_mv, $('center').checked);
  const count = snapshot.voltage_mv.length;
  chart.replaceChildren();
  element('title', { id: 'chart-title' }, `Voltage and current ADC waveforms, ${count} sample pairs`);
  label(plot.left, 16, 'V (mV)', colors.voltage);
  label(plot.right, 16, 'I (mV)', colors.current);

  for (let tick = 0; tick <= 4; tick++) {
    const fraction = tick / 4;
    const y = plot.bottom - fraction * (plot.bottom - plot.top);
    element('line', { x1: plot.left, x2: plot.right, y1: y, y2: y, stroke: colors.grid });
    label(plot.left - 12, y + 4, (voltage.min + fraction * (voltage.max - voltage.min)).toFixed(1), colors.voltage, 'end');
    label(plot.right + 12, y + 4, (current.min + fraction * (current.max - current.min)).toFixed(1), colors.current, 'start');
  }
  if (count) {
    const ticks = Math.min(4, count - 1);
    for (let tick = 0; tick <= ticks; tick++) {
      const index = Math.round(tick / Math.max(1, ticks) * (count - 1));
      const x = plot.left + index / Math.max(1, count - 1) * (plot.right - plot.left);
      label(x, plot.bottom + 24, index);
    }
    drawSeries(voltage, colors.voltage);
    drawSeries(current, colors.current);
  } else {
    label(480, 185, 'No samples to display — fetch a snapshot');
  }
  label(480, 408, 'Sample index (oldest → newest)');
  $('count').textContent = `${count} sample pairs`;
  $('voltage-bias').textContent = `V bias: ${count ? voltage.bias.toFixed(2) + ' mV' : '—'}`;
  $('current-bias').textContent = `I bias: ${count ? current.bias.toFixed(2) + ' mV' : '—'}`;
}

function setStatus(message, error = false) {
  $('status').textContent = message;
  $('status').classList.toggle('error', error);
}

$('center').addEventListener('change', render);
$('fetch-form').addEventListener('submit', async event => {
  event.preventDefault();
  const board = $('board').value.trim();
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 8000);
  $('fetch-button').disabled = true;
  setStatus(`Fetching /samples from ${board}…`);
  try {
    const query = new URLSearchParams({ board });
    const response = await fetch(`/samples?${query}`, { signal: controller.signal, cache: 'no-store' });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
    snapshot = validateSamples(data);
    data.current_mv = data.current_mv.slice(256-32)
    data.voltage_mv = data.voltage_mv.slice(256-32)
    render();
    try { localStorage.setItem('scope-board', board); } catch { /* Optional. */ }
    setStatus(snapshot.voltage_mv.length
      ? `Fetched ${snapshot.voltage_mv.length} pairs from ${board} at ${new Date().toLocaleTimeString()}.`
      : 'The board returned no samples. Check that the ADS1115 is connected and sampling.');
  } catch (error) {
    const message = controller.signal.aborted ? 'Request timed out. Check the board address and WiFi connection.' : error.message;
    setStatus(`${message}${snapshot.voltage_mv.length ? ' Previous snapshot retained.' : ''}`, true);
  } finally {
    clearTimeout(timer);
    $('fetch-button').disabled = false;
  }
});

render();
