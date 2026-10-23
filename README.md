# esp-current-sense

Whole-home energy monitor: a NodeMCU-32S (ESP32) reading an SCT-013 CT clamp
on the mains, reporting to Home Assistant over the LAN.

- Board: NodeMCU-32S, ESP32-D0WD-V3 rev 3.1, CP2102 USB-UART
- Clamp: SCT-013 **100A/1V** (voltage-output type — burden resistor is built in)
- Firmware: Arduino framework via `arduino-cli`, esp32 core 3.3.12

## Toolchain

Already installed on this Mac:

- `arduino-cli` (brew) with `esp32:esp32` core 3.3.12
- `esptool` (brew) for low-level flash work

## Commands

```sh
make build     # compile
make upload    # compile + flash (PORT=/dev/cu... to override)
make monitor   # tail serial @115200
make dev       # upload + monitor
```

Default port: `/dev/cu.usbserial-0001`.

Non-interactive serial check (no TTY needed, prints 6 s of output then exits):

```sh
uv run --with pyserial python tools/serialpeek.py
```

## Current phase

Phase 1 — blink firmware (`firmware/firmware.ino`). Onboard LED is on **GPIO2**
(defined as `LED_BUILTIN` in the sketch). See the roadmap at the bottom for
what comes next.

---

## CT clamp wiring (SCT-013 100A/1V)

### What you have

The 100A/1V variant is a **voltage-output** CT: the burden resistor is inside
the clamp and it outputs **1 V RMS AC at 100 A** through the primary
(~500 mV RMS at 50 A, linear with current). So **no external burden resistor**
is needed. What *is* needed is a DC bias, because the ESP32 ADC reads 0–3.3 V
only and cannot see the negative half of the AC waveform. We bias the signal
at the ADC mid-rail (1.65 V) so the AC swings symmetrically around it.

### Circuit

```
 3V3 ──[ 10k ]──┬──[ 10k ]── GND
                │
                ├──══╪══ 10 µF electrolytic (– to GND)   (bias stabiliser)
                │
                ├── 100 nF ceramic to GND (optional, HF decoupling)
                │
                └──────────► GPIO34  (ADC1_CH6)

 CT tip   ──────┘ (signal into the bias node)
 CT sleeve ──── GND
```

Parts: 2 × 10 kΩ (matched pair, 1% if possible), 1 × 10 µF electrolytic,
optionally 1 × 100 nF ceramic.

At 100 A the waveform spans 1.65 V ± 1.414 V (≈ 0.24–3.07 V) — inside the
11 dB-attenuation ADC range. Typical household loads (5–40 A) sit right in
the comfortable middle of that range.

### Pin choice

**Use GPIO34.** On the ESP32, ADC2 (GPIO 0, 2, 4, 12–15, 25–27) is
unusable while WiFi is on — and WiFi is the whole point here. ADC1 pins
(GPIO 32–39) are the only safe ones. GPIO34 is the best of them:
input-only, so it can't interfere with boot strapping.

| Purpose            | Pin     | ADC channel | Notes                              |
|--------------------|---------|-------------|------------------------------------|
| **CT signal**      | GPIO34  | ADC1_CH6    | primary choice, input-only         |
| 2nd/3rd CT (later) | GPIO32/33 | ADC1_CH4/5 | both safe, have pull-up capability |
| avoid              | GPIO35/36/39 | ADC1_CH7/0/1 | work, but no pull-ups; 36/39 slightly noisier |
| never              | GPIO 0,2,4,12–15,25–27 | ADC2 | dead while WiFi is on  |

### Accuracy caveats (and upgrade paths)

- ESP32 ADC is noticeably non-linear near 0 V and near full scale. Firmware
  will use the factory eFuse calibration + 11 dB attenuation + heavy
  oversampling (sample ≥ 2 kHz, RMS over whole 50 Hz cycles) which is plenty
  for whole-home monitoring.
- Current-only measurement gives **apparent power** (VA) assuming nominal
  230 V and PF ≈ 1. Good enough for the HA energy dashboard; for true
  power/kWh later, add a ZMPT101B mains-voltage input on another ADC1 pin
  and compute real power.
- If precision matters later: an ADS1115 (I²C, 16-bit) replaces the noisy
  ESP32 SAR ADC for ~£5.

### Safety

The CT is **non-invasive**: it clamps around a single conductor (live *or*
neutral — never both; the fields cancel) inside the consumer unit. No mains
connection to the ESP32. Have an electrician fit the clamp if you're not
confident working near a Distribution Board / breaker panel. This
voltage-output variant is safe to disconnect at any time (a plain
current-output SCT-013-000 must never run open-circuit, but yours has the
burden built in).

### Calibration constant

Firmware `ICAL = 100.0` (amps per volt RMS at the ADC). Verify against a
known load: a 2 kW kettle ≈ 8.7 A @ 230 V. Trim the constant to match.

---

## Network plan (Home Assistant)

Two viable interfaces; the plan is **HTTP first for bring-up, MQTT as the
end state**.

### Phase A — HTTP GET (bring-up / debugging)

Firmware joins WiFi, serves JSON on `http://<ip>/metrics`:

```json
{"current_a": 4.21, "power_va": 968.3, "voltage": 230.0, "uptime_s": 1203}
```

HA can poll it with a `restful` sensor. Great for `curl` during development
and for a fail-safe fallback if MQTT drops.

### Phase B — MQTT with HA discovery (recommended end state)

- Broker: Mosquitto add-on on the HA instance.
- Firmware (PubSubClient) publishes state every ~5 s and pushes a one-shot
  **discovery config** so HA auto-creates the entity and links it to a
  device — no YAML needed:

```json
Topic:    homeassistant/sensor/esp-current-sense/current/config
Payload:  {"name":"Home current","stat_t":"esp-current-sense/state",
           "unit_of_measurement":"A","device_class":"current",
           "state_class":"measurement","uniq_id":"ecs-current",
           "val_tpl":"{{ value_json.current_a }}",
           "device":{"ids":"esp-current-sense","name":"Energy monitor",
                     "mf":"home-made","mdl":"ESP32 + SCT-013"}}
```

- State topic `esp-current-sense/state` carries the same JSON as the HTTP
  endpoint; availability topic for offline detection; QoS 1; non-retaining
  state (measurements), retaining config.

Why MQTT over pure HTTP polling: push instead of poll (no missed spikes),
multi-sensor friendly (add several CTs later without extra endpoints), and
native HA auto-discovery.

## Roadmap

1. **[done]** Blink + serial hello
2. WiFi connect + `/metrics` HTTP endpoint (verify end-to-end from a laptop)
3. CT frontend on GPIO34, ADC sampling + RMS current, kettle calibration
4. MQTT + HA discovery → energy dashboard (power VA → W at PF≈1)
5. OTA firmware updates, calibration constant in NVS
6. Optional: ZMPT101B voltage sensing (true power), multi-CT circuits
