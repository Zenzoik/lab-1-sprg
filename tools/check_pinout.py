#!/usr/bin/env python3
"""Consistency check: docs/pinout.md <-> firmware/firmware.ioc <-> diagram.json.

Rules
  1. every pin that firmware.ioc routes to a signal (except debug/oscillator pins)
     has a row in the pinout table, and the signal names match;
  2. every row of the table names a pin that firmware.ioc really configures;
  3. no pin and no Nucleo connector pin appears twice;
  4. every row that names a device pin (`part:PIN`) has a wire for it in
     diagram.json (main diagram or the ADC-record variant in docs/captures/adc).

Exit status 0 = consistent, 1 = at least one violation.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IOC = os.path.join(ROOT, "firmware", "firmware.ioc")
DOC = os.path.join(ROOT, "docs", "pinout.md")
DIAGRAMS = [os.path.join(ROOT, "diagram.json"),
            os.path.join(ROOT, "docs", "captures", "adc", "diagram.json")]

SYSTEM_PINS = {"PA13", "PA14-BOOT0", "PC14-OSCX_IN (PC14)", "PC15-OSCX_OUT (PC15)",
               "PF0-OSC_IN (PF0)", "PF1-OSC_OUT (PF1)"}


def read_ioc(path):
    pins = {}
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            m = re.match(r"^(P[A-F]\d+(?:-[A-Z0-9]+)?)\.Signal=(.+)$", line)
            if m:
                pins[m.group(1)] = m.group(2)
    return pins


def read_table(path):
    rows, inside = [], False
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if "pinout-table:begin" in line:
                inside = True
                continue
            if "pinout-table:end" in line:
                break
            if inside and line.startswith("|") and not line.startswith("|---") \
                    and not line.startswith("| Роль"):
                cells = [c.strip() for c in line.strip("|").split("|")]
                rows.append(cells)
    return rows


def wires(path):
    out = set()
    with open(path, encoding="utf-8") as fh:
        for a, b, *_ in json.load(fh)["connections"]:
            out.add((a, b))
            out.add((b, a))
    return out


def main():
    errors = []
    ioc = read_ioc(IOC)
    rows = read_table(DOC)
    if not rows:
        print("ERROR: no pinout table found in", DOC)
        return 1

    doc_pins = {}
    seen_wokwi = {}
    for cells in rows:
        if len(cells) < 8:
            errors.append("row has %d cells (need >= 8): %s" % (len(cells), cells[:3]))
            continue
        role, signal, pin, _af, _periph, _nuc, wokwi, device = cells[:8]
        if pin in doc_pins:
            errors.append("pin %s listed twice" % pin)
        doc_pins[pin] = signal
        if pin not in ioc:
            errors.append("%s (%s) is not configured in firmware.ioc" % (pin, role))
        elif ioc[pin] != signal:
            errors.append("%s: pinout says %s, ioc says %s" % (pin, signal, ioc[pin]))
        if wokwi not in ("–", "-"):
            if wokwi in seen_wokwi:
                errors.append("Nucleo pin %s used by %s and %s" % (wokwi, seen_wokwi[wokwi], pin))
            seen_wokwi[wokwi] = pin

    for pin, sig in ioc.items():
        if pin in SYSTEM_PINS or pin.startswith("PC14") or pin.startswith("PC15"):
            continue
        if pin not in doc_pins:
            errors.append("%s (%s) is in firmware.ioc but missing in the pinout table" % (pin, sig))

    wire_sets = [wires(p) for p in DIAGRAMS if os.path.exists(p)]
    for cells in rows:
        if len(cells) < 8:
            continue
        pin, wokwi, device = cells[2], cells[6], cells[7].split()[0]
        if ":" not in device or wokwi in ("–", "-"):
            continue
        a, b = "nucleo:" + wokwi, device
        if not any((a, b) in w for w in wire_sets):
            errors.append("no wire %s <-> %s in any diagram.json (row %s)" % (a, b, pin))

    for e in errors:
        print("ERROR:", e)
    print("check-pinout: %d rows, %d errors" % (len(rows), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
