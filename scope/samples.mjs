export function validateSamples(data) {
  if (!data || !Array.isArray(data.voltage_mv) || !Array.isArray(data.current_mv)) {
    throw new Error('Expected voltage_mv and current_mv arrays from /samples.');
  }
  if (data.voltage_mv.length !== data.current_mv.length) {
    throw new Error('Voltage and current arrays must have the same length.');
  }
  if (data.voltage_mv.length > 256) {
    throw new Error('Expected at most 256 sample pairs.');
  }
  if (![...data.voltage_mv, ...data.current_mv].every(Number.isFinite)) {
    throw new Error('Samples must contain only finite numbers.');
  }
  return data;
}

export function prepareChannel(values, center) {
  if (!values.length) return { values: [], bias: 0, min: -1, max: 1 };
  const bias = values.reduce((sum, value) => sum + value, 0) / values.length;
  const plotted = values.map(value => value - (center ? bias : 0));
  const low = Math.min(...plotted);
  const high = Math.max(...plotted);
  if (center) {
    const extent = Math.max(1, Math.abs(low), Math.abs(high)) * 1.1;
    return { values: plotted, bias, min: -extent, max: extent };
  }
  const padding = Math.max(1, (high - low) * 0.1);
  return { values: plotted, bias, min: low - padding, max: high + padding };
}
