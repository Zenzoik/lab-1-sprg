#!/usr/bin/env python3
"""Architecture rule check: layers may only include headers of the layers below them.

Layer rules (see docs/architecture.md):

    Core      (CubeMX main.c, IRQ handlers)   may use: App, Platform, Devices(bus.h), HAL
    App       (business logic, self-test)     may use: Devices
    Devices   (sensor / flash drivers)        may use: Devices only (bus.h interfaces)
    Platform  (HAL adaptation, board.h)       may use: Devices (bus.h), App (app_ports.h), HAL
    HAL       (Drivers/, vendor code)         not checked

Additionally App/ and Devices/ must not mention any HAL_ / LL_ symbol or STM32 register name,
so the layers stay testable on the host.

Usage:
    check_deps.py              check ../firmware, exit 1 on violations
    check_deps.py --selftest   prove the checker works: a planted violation must be detected
"""
import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = os.path.join(ROOT, "firmware")

LAYER_DIRS = {
    "Core": ["Core"],
    "App": ["App"],
    "Devices": ["Devices"],
    "Platform": ["Platform"],
    "HAL": ["Drivers"],
}

ALLOWED = {
    "Core": {"Core", "App", "Platform", "Devices", "HAL"},
    "App": {"App", "Devices"},
    "Devices": {"Devices"},
    "Platform": {"Platform", "Devices", "App", "HAL", "Core"},  # Core: main.h pin labels
}

# Platform may only take these two headers from App: the port table.
PLATFORM_APP_ALLOWED = {"app_ports.h"}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+[<"]([^>"]+)[>"]', re.M)
FORBIDDEN_SYMBOLS = re.compile(r"\b(HAL_[A-Za-z0-9_]+|LL_[A-Za-z0-9_]+|__HAL_[A-Za-z0-9_]+|GPIO[A-K]\b|TIM[0-9]+\b|I2C[0-9]\b|SPI[0-9]\b|USART[0-9]\b)")


def index_headers(fw):
    """Map header basename -> layer, by where the header lives in the tree."""
    idx = {}
    for layer, dirs in LAYER_DIRS.items():
        for d in dirs:
            for base, _dirs, files in os.walk(os.path.join(fw, d)):
                for f in files:
                    if f.endswith(".h"):
                        idx.setdefault(f, layer)
    return idx


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def check(fw):
    idx = index_headers(fw)
    problems = []
    for layer in ALLOWED:
        for d in LAYER_DIRS[layer]:
            for base, _dirs, files in os.walk(os.path.join(fw, d)):
                for f in sorted(files):
                    if not f.endswith((".c", ".h")):
                        continue
                    path = os.path.join(base, f)
                    rel = os.path.relpath(path, fw)
                    with open(path, encoding="utf-8", errors="replace") as fh:
                        text = fh.read()
                    for inc in INCLUDE_RE.findall(text):
                        name = os.path.basename(inc)
                        target = idx.get(name)
                        if target is None:
                            continue  # system / toolchain header
                        if target not in ALLOWED[layer]:
                            problems.append("%s: %s layer must not include %s (%s layer)" % (rel, layer, inc, target))
                        elif layer == "Platform" and target == "App" and name not in PLATFORM_APP_ALLOWED:
                            problems.append("%s: Platform may only include app_ports.h from App, not %s" % (rel, inc))
                    if layer in ("App", "Devices"):
                        code = strip_comments(text)
                        for m in FORBIDDEN_SYMBOLS.finditer(code):
                            problems.append("%s: %s layer references hardware symbol %s" % (rel, layer, m.group(0)))
    return problems


def selftest():
    """Plant violations in a temporary copy of the layer layout and require them to be found."""
    with tempfile.TemporaryDirectory() as tmp:
        for d in ("App", "Devices", "Platform", "Core/Inc"):
            os.makedirs(os.path.join(tmp, d))
        with open(os.path.join(tmp, "Core/Inc/main.h"), "w") as fh:
            fh.write("#define X 1\n")
        with open(os.path.join(tmp, "Devices/dev.h"), "w") as fh:
            fh.write("#define D 1\n")
        with open(os.path.join(tmp, "App/ok.c"), "w") as fh:
            fh.write('#include "dev.h"\n')
        with open(os.path.join(tmp, "Devices/bad_include.c"), "w") as fh:
            fh.write('#include "main.h"\n')
        with open(os.path.join(tmp, "App/bad_symbol.c"), "w") as fh:
            fh.write("void f(void) { HAL_Delay(1); }\n")
        found = check(tmp)
        expect = ["bad_include.c", "bad_symbol.c"]
        missing = [e for e in expect if not any(e in p for p in found)]
        clean = not any("ok.c" in p for p in found)
        if missing or not clean:
            print("selftest FAILED: missing=%s false_positive=%s" % (missing, not clean))
            return 1
        print("selftest ok: %d planted violations detected, clean file accepted" % len(expect))
        return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    problems = check(FIRMWARE)
    for p in problems:
        print("VIOLATION:", p)
    if problems:
        print("%d dependency violation(s)" % len(problems))
        return 1
    print("dependency rules ok (App -> Devices; Devices -> none; Platform -> HAL, bus.h, app_ports.h)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
