# esp-current-sense

Whole-home energy monitor: a NodeMCU-32S (ESP32) reading an SCT-013 CT clamp
on the mains, reporting to Home Assistant over the LAN.

- Board: NodeMCU-32S, ESP32-D0WD-V3 rev 3.1, CP2102 USB-UART
- Clamp: SCT-013 **100A/1V** (voltage-output type — burden resistor is built in)
- Firmware: Arduino framework via `arduino-cli`, esp32 core 3.3.12

## Toolchain

Already installed on this Mac:

- `arduino-cli` (brew) with `esp32:esp32` core 3.3.12
- `PubSubClient` 2.8 Arduino library (MQTT)
- `mosquitto` (brew) — test broker + `mosquitto_sub`/`mosquitto_pub` clients
- `esptool` (brew) for low-level flash work
- `uv` — runs the python helpers in `tools/` with zero-setup venvs

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

Phase 4 — CT sampling + HTTP + MQTT with HA discovery.

- Joins WiFi from `firmware/secrets.h` (copy from `secrets.h.example`, gitignored).
- LED: fast blink = joining, **steady on = ready**, off = lost connection (retries every 10 s).
- `GET /metrics` — JSON:

```sh
curl http://192.168.0.158/metrics
# {"current_a":0.000,"power_va":0.0,"voltage":220.0,"energy_kwh":0.0000,
#  "vrms_mv":0.0,"win_ms":216,"windows":40,"wifi_rssi_dbm":-65,
#  "heap_free":211464,"uptime_s":30,"source":"adc"}
```

`current_a`/`power_va`/`energy_kwh` read ~0 until the CT bias divider is
wired to GPIO34 (phase 3 code is live; values go real the moment it is).

The board also answers on `http://esp-current-sense.local/` **if** your
router doesn't block multicast — this LAN does (mDNS verified dead), so give
the board a **DHCP reservation** on the router and use the IP.

### MQTT

Publishes to the **homelab broker at `192.168.0.18:1883`** (anonymous allowed;
add credentials in `secrets.h` via `MQTT_USER`/`MQTT_PASS` when you tighten it).
Home Assistant runs on `192.168.0.22` (port **80**, HA Container install — no
Supervisor/add-ons); its MQTT integration points at the homelab broker and
the three entities auto-appeared under the "Home energy monitor" device.

Watch the live feed from anywhere:

```sh
/opt/homebrew/bin/mosquitto_sub -h 192.168.0.18 -t 'esp-current-sense/#' -v
```

Topics (all under the hostname):

| Topic | Payload |
|---|---|
| `esp-current-sense/state` | JSON, every 5 s (same as /metrics) |
| `esp-current-sense/availability` | `online`/`offline`, retained + LWT |
| `homeassistant/sensor/esp-current-sense/{current,power,energy}/config` | HA discovery, retained |

**Going production** (when ready): install the Mosquitto broker add-on on the
HA box, create an MQTT user, then in `firmware/secrets.h` set `MQTT_HOST` to
the HA box IP and fill `MQTT_USER`/`MQTT_PASS` — `make dev`. Entities appear
in HA automatically (Settings → Devices & services → MQTT). Give the board a
DHCP reservation. Until then, the HA entities would track the test broker.

---

## CT clamp wiring (SCT-013 100A/1V)

### What you have

The 100A/1V variant is a **voltage-output** CT: the burden resistor is inside
the clamp and it outputs **1 V RMS AC at 100 A** through the primary
(~500 mV RMS at 50 A, linear with current). So **no external burden resistor**
is needed.

> ⚠️ **"Built-in sampling resistor" ≠ no extra parts.** The burden converts
> current to voltage, but the output is still a 0-centred AC waveform. The
> ESP32 ADC only reads 0–3.1 V and its protection diodes clamp negative
> swings — you'd lose the whole negative half-cycle (wrong readings) and
> stress the pin at higher loads. The **bias divider below is mandatory**:
> it lifts the waveform to mid-rail so it fits the ADC window.
>
> Multimeter check before wiring: DC V range tip↔sleeve should read ~0.00 V
> (proves AC-centred output); AC V range should read load-proportional
> millivolts (~100 mV RMS per 10 A of house load).

### Circuit

**Interactive schematic** — open this link in a browser to see and simulate
the frontend (live scope on the GPIO34 node showing the 60 Hz waveform riding
on the 1.65 V bias, modelled at the full 100 A swing):

<[https://pfalstad.github.io/circuitjs1/circuitjs.html?ctz=DwYwlgTgBAZgvAIgMwEYFQC4GdEFYCmAtAGzpgiIAMAdLuiAEZ6XoBuEe6ADkwri1AC22RCiIBOBAD5grKAA9ESAExQVa1eNJQA7vAQDBAQ3msl1JOgD0MnQqWr1AFnUrrMgOb3km4hqgoygAc7sAANgB23sqUQf4xcSGY%2BEoAapZQNsDQigjKTpTxBQGB6BwIKJRVAlk5iPmFKIFQDQHiymWi1TWe0cUo7S3FyvmhdrmtTapOTnFToXK5WiW%2BLbHoeqLoMOX86MamotROKE6hdQjLU0ONpVDlMaGR3lfNxE4r6BgpCADCACqYMBcUJeJZ%2BBJQZYNUIgbxIJB%2Ba4IpHteiiIh0TK9XIotqOREtUbY0DeAq3aZVfHoipEADsoLJVIGlMKIzOJOeuRmc2auGa82SiAA4gAFACSAHkkB8ABQAQQAIr8UAB9X4ACWIAEpQpKybMVlAeQEZtstlAsBQDKEAPZQfARUTaLBcBzmvLcKjoXJiqUy6TALhQMw2y3Wjokqy2mRWcAQKRAA>](https://pfalstad.github.io/circuitjs1/circuitjs.html?ctz=DwYwlgTgBAZgvAIgMwEYFQC4GdEFYCmAtAGzpgiIAMAdLuiAEZ6XoBuEe6ADkwri1AC22RCiIBOBAD5grKAA9ESAExQVa1eNJQA7vAQDBAQ3msl1JOgD0MnQqWr1AFnUrrMgOb3km4hqgoygAc7sAANgB23sqUQf4xcSGY%2BEoAapZQNsDQigjKTpTxBQGB6BwIKJRVAlk5iPmFKIFQDQHiymWi1TWe0cUo7S3FyvmhdrmtTapOTnFToXK5WiW%2BLbHoeqLoMOX86MamotROKE6hdQjLU0ONpVDlMaGR3lfNxE4r6BgpCADCACqYMBcUJeJZ%2BBJQZYNUIgbxIJB%2Ba4IpHteiiIh0TK9XIotqOREtUbY0DeAq3aZVfHoipEADsoLJVIGlMKIzOJOeuRmc2auGa82SiAA4gAFACSAHkkB8ABQAQQAIr8UAB9X4ACWIAEpQpKybMVlAeQEZtstlAsBQDKEAPZQfARUTaLBcBzmvLcKjoXJiqUy6TALhQMw2y3Wjokqy2mRWcAQKRAA)

Also saved as `docs/ct-frontend.circuitjs` (CircuitJS1 XML — File → Import in
the simulator) and `docs/ct-frontend.url`. Regenerate both with
`uv run --with lzstring python tools/gen_ct_schematic.py`.

```
 3V3 ──[ 10k ]──┬──[ 10k ]── GND
                │
                ├──► GPIO34  (ADC1_CH6)
                │
                └──[ 10 µF, + toward node ]──●── CT tip
CT sleeve ──────────────────────────── GND   (● = node)
```

> ⚠️ **The series coupling cap in the tip line is REQUIRED for this clamp —
> verified empirically.** This SCT-013's internal burden is DC-conductive
> tip→sleeve: connected straight to the divider node it drags the bias from
> 1.65 V down to ~0.14 V (measured). The series cap blocks that DC path
> while passing 60 Hz untouched (corner = 1/(2π·5kΩ·C) ≈ 3 Hz @ 10 µF).
> Go/no-go check on serial: `bias≈1658mV` **with the clamp attached**.

**Bias sanity check:** firmware prints `[ct] bias=…mV` — it must sit at
≈ 1600–1700 mV **with the clamp connected**. Below ~1.5 V means something
DC-conductive is loading the node; a perfectly flat line (min == max) means
the ADC pin isn't connected to anything at all.

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
  oversampling (sample ≥ 2 kHz, RMS over whole mains cycles — the window is
  closed on hysteresis-counted rising edges, so it always spans complete
  half-cycles and self-adapts to 50 or 60 Hz) which is plenty for whole-home
  monitoring.
- Current-only measurement gives **apparent power** (VA) assuming nominal
  220 V and PF ≈ 1. Good enough for the HA energy dashboard; for true
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

Firmware `kAmpsPerVolt = 127.0` — calibrated against a 2200 W kettle on 220 V
mains through the final frontend (series-cap tip line + 300 Hz digital
low-pass): kettle displays 10.0–10.5 A against a nameplate 10.0 A. Residual
accuracy ±3 %, dominated by element/mains tolerance (kettle power scales with
V², and mains voltage drifts a few percent) — refine against a utility meter
over a day or a plug-in power meter for better.

Noise gate: `kNoiseFloorA = 2.0` — the node's idle floor measures ~14–16 mV RMS
(~1.8–2 A equivalent at k=127). That floor is the ESP32 SAR ADC's own conversion
noise, not coupled RF: the 100 nF ceramic at the pin did **not** lower it, so
the gate is what manages it. This clamp+ADC pair cannot resolve much below ~2 A;
readings under 2 A report as 0. A 50 A/1 V clamp or an ADS1115 16-bit ADC would
lower the floor and allow a tighter gate.

### ZMPT101B voltage frontend (true power, optional)

The ZMPT101B module is a voltage transformer with a built-in sampling
resistor: its output is a 0-centred AC waveform **DC-biased at VCC/2**
(~2.5 V when powered from 5 V). Power it from **VIN (5 V)** — its VCC pin
is 5 V only. Its primary terminals go across mains L/N (safety rules as in
the CT section apply).

Feed the ADS1115 `A1` like this — the divider provides the DC bias from the
3V3 rail, and the coupling cap blocks the ZMPT's own 2.5 V DC so only the AC
rides on the divider bias:

```
ZMPT101B VCC  ── VIN (5 V)
ZMPT101B GND  ── GND
ZMPT101B OUT ──[10 µF, + toward OUT]──┐
                                       ●── A1 (ADS1115)
3V3 ──[10k]──┬──[10k]── GND           │
             └───────────┘  (node = 1.65 V)
```

- **Divider:** 10k top to 3V3, 10k bottom to GND → node sits at **1.65 V**,
  symmetric headroom to the 3.3 V rail in both directions.
- **10 µF polarity:** ZMPT OUT is at ~2.5 V DC, the node at 1.65 V, so the
  **+ lead faces the ZMPT OUT**. Backwards it degrades/leaks.
- **100 nF ceramic** node→GND optional, for RF junk (matters more with the
  ADS1115 than the SAR ADC).
- Sanity check with no mains on the ZMPT primary: `A1 bias ≈ 1650 mV`, flat.
  With mains connected, the waveform should swing symmetrically about it; if
  the positive peak approaches 3.3 V, trim the module's onboard pot down.

---

## Network plan (Home Assistant)

Two viable interfaces; the plan is **HTTP first for bring-up, MQTT as the
end state**.

### Phase A — HTTP GET (bring-up / debugging)

Firmware joins WiFi, serves JSON on `http://<ip>/metrics`:

```json
{"current_a": 4.21, "power_va": 926.2, "voltage": 220.0, "uptime_s": 1203}
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
2. **[done]** WiFi connect + `/metrics` HTTP endpoint
3. **[done]** CT frontend on GPIO34 — series-coupled divider verified,
   RMS sampling, kettle-calibrated to ±15 %, noise gate at 2 A
4. **[done on test broker]** MQTT + HA discovery — repoint secrets.h at the
   HA broker when Mosquitto is installed there
5. OTA firmware updates, calibration constant in NVS
6. Optional: ZMPT101B voltage sensing (true power), multi-CT circuits
