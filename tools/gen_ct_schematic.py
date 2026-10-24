#!/usr/bin/env python3
"""Generate docs/ct-frontend.circuitjs (CircuitJS1 XML) + a ctz= URL.

Circuit: SCT-013 100A/1V CT clamp frontend for the ESP32 ADC.
  - CT modelled as a 50 Hz AC source (1.414 V peak = 1 V RMS at 100 A)
    behind its ~20 ohm internal sampling resistor
  - 10k/10k divider biases the ADC node to mid-rail (1.65 V)
  - 10uF stabilises the bias midpoint, 100nF decouples HF
  - GPIO34 = ADC1_CH6 input-only pin

Run:  uv run python tools/gen_ct_schematic.py
"""
import urllib.parse
import xml.etree.ElementTree as ET

import lzstring

# (tag, attrs) — file order matters: the scope's `en` index is positional.
ELEMENTS = [
    ("v", {"x": "32 32 32 96", "wf": "0", "maxv": "3.3"}),        # 3V3 rail source
    ("w", {"x": "32 32 432 32"}),                                 # 3V3 rail
    ("g", {"x": "32 96 32 128"}),                                 # rail source gnd
    ("ln", {"x": "208 32 208 8", "te": "3V3"}),                   # rail label
    ("r", {"x": "240 32 240 112", "r": "10000"}),                 # R1 (top)
    ("r", {"x": "240 112 240 192", "r": "10000"}),                # R2 (bottom)
    ("g", {"x": "240 192 240 224"}),                              # divider gnd
    ("w", {"x": "240 112 448 112"}),                              # ADC node bus
    ("v", {"x": "96 112 96 208", "wf": "1", "fr": "50", "maxv": "1.414"}),  # CT @100A
    ("r", {"x": "96 112 240 112", "r": "20"}),                    # internal burden (AC model)
    ("ln", {"x": "96 112 64 112", "te": "CT tip"}),               # tip label
    ("g", {"x": "96 208 96 240"}),                                # sleeve gnd
    ("c", {"x": "336 112 336 192", "c": "1e-5"}),                 # C1 10uF bias cap
    ("g", {"x": "336 192 336 224"}),
    ("c", {"x": "400 112 400 192", "c": "1e-7"}),                 # C2 100nF HF
    ("g", {"x": "400 192 400 224"}),
    ("ln", {"x": "448 112 512 112", "te": "GPIO34 (ADC1_CH6)"}),  # ADC pin label
    ("O", {"x": "448 112 448 144", "f": "1", "sc": "0"}),         # live V readout
]

CIR_OPTIONS = {"f": "31", "ts": "5e-6", "ic": "0.5", "cb": "50", "vr": "5",
               "pb": "50", "mts": "1e-9"}

SCOPE_TARGET_TE = "GPIO34 (ADC1_CH6)"  # scope plots this element's node voltage


def build_xml() -> str:
    root = ET.Element("cir", CIR_OPTIONS)
    for tag, attrs in ELEMENTS:
        ET.SubElement(root, tag, attrs)
    scope_en = next(i for i, (tag, a) in enumerate(ELEMENTS)
                    if tag == "ln" and a.get("te") == SCOPE_TARGET_TE)
    scope = ET.SubElement(root, "o", {"en": str(scope_en), "sp": "32", "f": "2",
                                      "p": "0", "x": "GPIO34"})
    ET.SubElement(scope, "p", {"v": "0", "sc": "2"})

    # validate well-formedness by round-tripping through a parser
    body = ET.tostring(root, encoding="unicode")
    ET.fromstring(body)
    return body


def main() -> None:
    xml = build_xml()
    out_path = "docs/ct-frontend.circuitjs"
    with open(out_path, "w") as f:
        f.write(xml)

    compressed = lzstring.LZString().compressToEncodedURIComponent(xml)
    url = ("https://pfalstad.github.io/circuitjs1/circuitjs.html?ctz="
           + urllib.parse.quote(compressed, safe=""))
    with open("docs/ct-frontend.url", "w") as f:
        f.write(url + "\n")

    print(f"wrote {out_path} ({len(xml)} bytes)")
    print(f"url: {url}")


if __name__ == "__main__":
    main()
