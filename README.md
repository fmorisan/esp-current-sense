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
make test      # host-side sample buffer/JSON + signed-power tests (C++ compiler required)
make scope     # local I/V waveform viewer at http://127.0.0.1:8000
make metrics   # live W/VA dashboard at http://127.0.0.1:8002
make test-scope # local dashboard tests (Python 3 + Node.js)
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

CT sampling + HTTP + MQTT with HA discovery + HTTP OTA updates.

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

### ADC sample snapshots

`GET /samples` returns the latest **256 pairs** (or fewer while the buffer fills),
oldest first. The homepage links to it:

```sh
curl http://192.168.0.158/samples
# {"voltage_mv":[1650,1702,1751],"current_mv":[1651.250,1658.500,1665.750]}
```

Both arrays have the same length; matching indexes are approximately aligned
in time. One pair is recorded per successful ADS1115 current read, using the
existing GPIO34 voltage reading nearest that conversion's midpoint. There are
no extra ADC reads and no timestamps.

- `voltage_mv`: GPIO34 ADC-input millivolts **before** the firmware's digital
  low-pass filter.
- `current_mv`: ADS1115 A0 millivolts with the configured current polarity
  applied around the snapshot's mean. This reflects the AC waveform without
  negating its DC bias. Use `GET /samples?raw=1` on the board for uncorrected
  ADC-input readings (raw counts × 0.125 mV).
- Both retain their DC bias; these are **not** mains volts or amps. Subtract
  each array's mean if you want centred waveforms for plotting.
- If no pairs have been acquired (for example, the ADS1115 is missing), both
  arrays are empty. Read errors skip the whole pair; old samples remain until
  replaced, so a snapshot is not a guarantee of fresh data.

This is a small diagnostic snapshot, not a continuous stream or precise
timebase. Sampling pauses while HTTP sends the response, and HTTP/MQTT work can
leave timing gaps between pairs. Prefer occasional fetches rather than rapid
polling. The history uses about 2 KB of RAM; JSON is sent in small chunks.

### Current-sensor polarity

`kCurrentPolarity` in `firmware/current_polarity.h` is **−1** for this
installation's reversed current-sensor polarity. Change it to **+1** if the
physical sensor/reference polarity changes. Keep `kAmpsPerMv` positive.

The same setting corrects signed real power / power factor and the default
`/samples` current waveform. Raw ADC history, bias diagnostics, RMS current,
and apparent power remain unchanged. `/metrics` reports `current_polarity`;
the scope requires no separate inversion and must not invert the data again.
Its raw-ADC view is available directly on the board at `/samples?raw=1`.

The old `abs()` masking is removed: consumption should give positive W, while
opposite power flow gives negative W and PF. **That sign is not leading/lagging**;
reactive direction still requires phase measurement. `energy_kwh` accumulates
only positive real power (import), so export cannot decrease Home Assistant's
`total_increasing` consumption counter.

After flashing, check a mostly resistive load such as the kettle: corrected
voltage/current waveforms should roughly align and W/PF should be positive.
Polarity inversion is not a time-delay or phase-offset calibration, and does
not require rewiring mains conductors.

### Local waveform viewer

Run `make scope`, then open **http://127.0.0.1:8000**. Enter the board's IP
address (or hostname) and click **Fetch samples**. Each click requests one
snapshot; there is no automatic polling. The address is remembered locally.

The viewer overlays V and I against sample index, with separate, color-coded
vertical scales so a small current signal is still visible. **Remove DC bias**
is enabled by default; uncheck it to inspect the DC-biased, polarity-corrected
waveforms. Both axes use ADC-scale **mV**, not calibrated mains volts or amps. The original
MQTT dashboard in `index.html` is unchanged.

The viewer needs only Python 3 and a browser, with no packages or CDN scripts.
A loopback-only server serves `scope/` and proxies `GET /samples`, avoiding
browser CORS restrictions without changing the firmware. It does not expose
the repository, secrets, or OTA endpoints. Stop it with Ctrl-C. To change the
local port, run `python3 tools/serve_scope.py --port 8001`.

### Live power dashboard (W and VA)

Run `make metrics`, then open **http://127.0.0.1:8002**. It automatically polls
the board's `/metrics` endpoint about every **2 seconds** while the page is open,
plotting **real power (W)** and **apparent power (VA)** together against local
time. Both axes share the same numeric scale. Pause tracking to change the board
address, then resume; the address is remembered locally. No Home Assistant token,
additional firmware changes, external packages, or Internet access are needed.

- The chart retains up to **30 minutes** of readings in this browser tab.
  Reloading clears it; it does not retrieve historical readings from the board.
- Pauses, network failures, and detected board reboots leave gaps instead of
  connecting unrelated readings. Retries never overlap. A failed read is clearly
  marked, and the last displayed values remain visible with their read time.
- Hover over the chart to inspect W and VA. The cards also show power factor,
  current, voltage, and the board's since-boot imported-energy counter.
- A live **unit-circle power triangle** represents apparent power S as a radius
  of 1, with real power P/S horizontally and estimated reactive magnitude
  |Q|/S vertically. Actual W, VA, and estimated var are listed alongside it.
  The estimate is `sqrt(S² - P²)`: for sinusoidal loads it is |Q|, but harmonics
  add distortion to this non-active magnitude. The firmware cannot determine
  reactive sign (leading/lagging), so the estimate is drawn upward without
  assigning a sign. Zero VA cannot be normalized; readings with |W| > VA are
  flagged instead of drawing a misleading triangle. This is power, not reactance
  in ohms.
- Compare **W** before and after switching a load off to see changes in real
  consumption. VA includes the reactive component and is not energy in kWh.
  The energy counter resets at board reboot; sensor calibration still determines
  accuracy. This is not a billing meter.

The dashboard uses the same loopback-only proxy as the ADC viewer, but exposes
only `/metrics` and its own static assets. The ADC viewer and original MQTT
dashboard remain unchanged. Browser sleep/background throttling may cause gaps;
this page is for live observation, not unattended long-term logging.

To use another local port:

```sh
python3 tools/serve_scope.py --metrics --port 8003
```

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

### OTA firmware updates (local network)

The existing HTTP server serves a browser updater at `http://<board-ip>/update`,
also linked from `/`. It uses `HTTPUpdateServer` bundled with the ESP32 core;
no extra library is needed.

1. **First time only:** run `make upload` over USB to install this OTA-capable
   firmware. The default NodeMCU-32S partition layout already has two OTA app
   slots; keep using that layout for future builds.
2. For subsequent updates, run `make build`.
3. Open `http://<board-ip>/update` (for example,
   `http://192.168.0.158/update`). In the **Firmware** form, select
   `.build/firmware.ino.bin` and click **Update Firmware**. Do not use the
   FileSystem form or upload the merged, bootloader, or partitions binaries.
4. Wait for the success message and automatic reboot, then check `/metrics`.
   Sampling/MQTT pause during the upload; uptime and the since-boot energy
   counter reset after reboot. Keep the board powered throughout the update.

Alternatively, upload from the command line (the matching `Origin` header is
required by the core updater's browser-origin check):

```sh
BOARD_URL=http://192.168.0.158
curl --fail --show-error -H "Origin: $BOARD_URL" \
  -F 'firmware=@.build/firmware.ino.bin' "$BOARD_URL/update"
```

Check the response body for **Update Success**; the bundled updater can return
HTTP 200 even when it reports an update error.

**No authentication or TLS for now:** anyone who can reach the board can replace
its firmware. Use only on a trusted LAN; do not port-forward or expose this
server to the Internet. Authentication can be added later through the updater's
credential options. Keep OTA support in future firmware if you want to continue
updating wirelessly.

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
                ├──► ADS1115 A0   (I2C 0x48 on GPIO21/22)
                │
                └──[ 10 µF, + toward node ]──●── CT tip
CT sleeve ──────────────────────────── GND   (● = node)

ADS1115 VDD ── 3V3   ADS1115 GND ── GND   SDA ── GPIO21   SCL ── GPIO22
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

**Current (CT) — ADS1115 A0.** The biased CT signal goes to the ADS1115's A0
input; the ADS1115 talks to the ESP32 over I2C on **GPIO21 (SDA)** and
**GPIO22 (SCL)**. That keeps the 16-bit converter next to the signal and
leaves the ESP32's own ADC free for the voltage channel.

**Voltage (ZMPT101B) — GPIO34.** Use an ADC1 pin; ADC2 (GPIO 0, 2, 4, 12–15,
25–27) is unusable while WiFi is on. GPIO34 is input-only and safe.

If you'd rather skip the ADS1115 and read the CT on the ESP32's SAR ADC
directly, use **GPIO35** (ADC1_CH7) or **GPIO39** (ADC1_CH3) — both are
input-only and free. Expect a much coarser result (~1 A noise floor).

| Purpose            | Pin                     | Notes                                    |
|--------------------|-------------------------|------------------------------------------|
| **CT signal**      | ADS1115 A0              | via series-cap divider; I2C GPIO21/22    |
| **Voltage signal** | GPIO34                  | ADC1_CH6, ZMPT101B via divider            |
| Direct-ADC CT alt. | GPIO35 / GPIO39         | ADC1, input-only, ~1 A noise floor        |
| avoid              | GPIO 0,2,4,12–15,25–27  | ADC2 — dead while WiFi is on             |

### Accuracy caveats (and upgrade paths)

- **Voltage** uses the ESP32 SAR ADC on GPIO34 (ZMPT101B through a 10 µF
  coupling into a 10k/10k bias divider), with factory eFuse calibration,
  11 dB attenuation and whole-cycle RMS windows — plenty for mains voltage.
- **Current** uses a 16-bit ADS1115 on A0 (single-shot, 860 SPS, ±4.096 V PGA)
  behind the CT's series-cap divider. Its ~0.25 mV RMS noise floor corresponds
  to ~0.03 A at k=127, so the noise gate sits at 0.05 A — real sub-amp
  resolution, unlike the SAR ADC's ~1.3 A floor.
- Real power and PF are computed by phase-aligning the current and voltage
  channels. A ~5 % phase error remains because the ADS1115 reads A0/A1
  sequentially (~1.25 ms skew ≈ 22° at 50 Hz): a resistive kettle reads
  PF ≈ 0.95 instead of ≈1.0. Add a phase-offset trim for true kWh.

### Safety

The CT is **non-invasive**: it clamps around a single conductor (live *or*
neutral — never both; the fields cancel) inside the consumer unit. No mains
connection to the ESP32. Have an electrician fit the clamp if you're not
confident working near a Distribution Board / breaker panel. This
voltage-output variant is safe to disconnect at any time (a plain
current-output SCT-013-000 must never run open-circuit, but yours has the
burden built in).

### Calibration constants

- **Current:** `kAmpsPerMv = 0.1316` in `power_sampler.h` — calibrated against
  a 2200 W kettle on 220 V mains through the final frontend (series-cap tip
  line + 300 Hz digital low-pass): kettle displays ≈9.9 A against a 10 A
  nameplate. Residual accuracy ±3 %, dominated by element/mains tolerance
  (kettle power scales with V², and mains voltage drifts a few percent) —
  refine against a utility meter over a day or a plug-in power meter for
  better. If you fit the optional 100 nF at the CT node, trim the constant
  down by the resulting attenuation ratio.
- **Voltage:** `kVoltsPerMv = 0.688` in `power_sampler.h` — calibrated against
  a multimeter: 220 V ↔ ~320 mV RMS at the node. Re-derive whenever the
  divider, coupling cap or ZMPT pot changes.

Noise gates: `kNoiseFloorA = 0.05` on the ADS1115 current channel (its
~0.25 mV RMS floor ≈ 0.03 A); `kNoiseFloorMv = 3.0` on the voltage channel.

### ZMPT101B voltage frontend (true power, optional)

The ZMPT101B module is a voltage transformer with a built-in sampling
resistor: its output is a 0-centred AC waveform **DC-biased at VCC/2**
(~2.5 V when powered from 5 V). Power it from **VIN (5 V)** — its VCC pin
is 5 V only. Its primary terminals go across mains L/N (safety rules as in
the CT section apply).

Feed GPIO34 like this — the divider provides the DC bias from the
3V3 rail, and the coupling cap blocks the ZMPT's own 2.5 V DC so only the AC
rides on the divider bias:

```
ZMPT101B VCC  ── VIN (5 V)
ZMPT101B GND  ── GND
ZMPT101B OUT ──[10 µF, + toward OUT]──┐
                                       ●── GPIO34 (ADC1_CH6)
3V3 ──[10k]──┬──[10k]── GND           │
             └───────────┘  (node = 1.65 V)
```

- **Divider:** 10k top to 3V3, 10k bottom to GND → node sits at **1.65 V**,
  symmetric headroom to the 3.3 V rail in both directions.
- **10 µF polarity:** ZMPT OUT is at ~2.5 V DC, the node at 1.65 V, so the
  **+ lead faces the ZMPT OUT**. Backwards it degrades/leaks.
- **100 nF ceramic** node→GND optional, for RF junk (matters more with the
  ADS1115 than the SAR ADC).
- Sanity check with no mains on the ZMPT primary: `GPIO34 bias ≈ 1650 mV`, flat.
  With mains connected, the waveform should swing symmetrically about it; if
  the positive peak approaches 3.3 V, trim the module's onboard pot down.

---

## Network plan (Home Assistant)

Two viable interfaces; the plan is **HTTP first for bring-up, MQTT as the
end state**.

### Phase A — HTTP GET (bring-up / debugging)

Firmware joins WiFi, serves JSON on `http://<ip>/metrics`:

```json
{"current_a": 4.21, "voltage_v": 220.0, "power_va": 926.2, "power_w": 910.0,
 "pf": 0.982, "energy_kwh": 1.2345, "mains_hz": 50.0, "uptime_s": 1203}
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

1. **[done]** Blink + serial hello (`hello_blink/`)
2. **[done]** WiFi connect + `/metrics` HTTP endpoint
3. **[done]** CT frontend on ADS1115 A0 — series-coupled divider, RMS sampling,
   noise gate at 0.05 A (calibrate against a kettle once the clamp is fitted)
4. **[done]** ZMPT101B voltage on GPIO34 — whole-cycle RMS, mains frequency,
   concurrent V+I sampling for true power and PF
5. **[done on test broker]** MQTT + HA discovery — repoint secrets.h at the
   HA broker when Mosquitto is installed there
6. Fit the CT clamp around a single mains conductor and calibrate
   `kAmpsPerMv` against a known load (kettle) or the utility meter
7. **[done]** OTA firmware updates via HTTP `/update` (trusted LAN, no auth yet)
8. Calibration constants in NVS
9. Optional: multi-CT circuits
