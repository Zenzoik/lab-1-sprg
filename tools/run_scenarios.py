#!/usr/bin/env python3
"""LR3 scenario driver: runs the load scenarios on the firmware through the Wokwi serial port
(RFC 2217, wokwi.toml: rfc2217ServerPort = 4000) or a real UART, saves the raw logs and parses
the machine-readable lines (SCN / STAT / JIT / MEM / RX / EVLOG / EVT / JOURNAL) into JSON.

  run_scenarios.py list
  run_scenarios.py run NAME [NAME...]        run scenarios, logs -> docs/metrics/NAME.log
  run_scenarios.py raw "cmd1" "cmd2" ...     send commands, print the answer

All times in the lines come from the MCU cycle counter (microseconds); the host only paces the
commands and never uses the arrival time of a line as a measurement (methodology 5.2).
Requires: pip install pyserial
"""
import argparse
import json
import os
import re
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
METRICS = os.path.join(ROOT, "docs", "metrics")
URL = os.environ.get("LR3_PORT", "rfc2217://localhost:4000")
CHAR_DELAY = float(os.environ.get("LR3_CHAR_DELAY", "0.02"))

# --------------------------------------------------------------------------- scenarios
# name -> (description, [steps]); a step is a command string, ("wait", regex, timeout_s)
# or ("flood", seconds)
CFG_S3 = ["set imu 20", "set tlm 200", "set jrn 100", "set lop 1000", "set stim 2", "set btn irq"]


def scn(n, sec):
    return ["scn %d %d" % (n, sec), ("wait", r"SCN DONE n=%d" % n, sec + 30)]


def custom(sec, cfg):
    return cfg + ["scn 0 %d" % sec, ("wait", r"SCN DONE n=0", sec + 30)]


SCENARIOS = {
    "S1": ("S1 idle: periodic tasks only", scn(1, 30)),
    "S2": ("S2 nominal: telemetry 1 Hz, journal 2 Hz, rare presses", scn(2, 30)),
    "S3": ("S3 worst: telemetry 5 Hz, journal 10 Hz, sector rewrite every 1 s, frequent presses", scn(3, 30)),
    # 6.2 (1) long operation and a periodic task
    "L1_baseline": ("6.2-1 control task alone", custom(20, ["set tlm 0", "set jrn 0", "set lop 0", "set stim 0"])),
    "L1_flash": ("6.2-1 control task + sector erase/program every 1 s",
                 custom(20, ["set tlm 0", "set jrn 0", "set lop 1000", "set stim 0"])),
    # 6.2 (2) event during a long operation, 40 presses, EXTI vs polling
    "E1_irq_idle": ("6.2-2 button events by EXTI, no long operation",
                    custom(40, ["set tlm 0", "set jrn 0", "set lop 0", "set stim 1", "set btn irq"])),
    "E2_irq_longop": ("6.2-2 button events by EXTI during sector rewrites (every 500 ms)",
                      custom(40, ["set tlm 0", "set jrn 0", "set lop 500", "set stim 1", "set btn irq"])),
    "E3_poll_longop": ("6.2-2 button events by polling during sector rewrites (every 500 ms)",
                       custom(40, ["set tlm 0", "set jrn 0", "set lop 500", "set stim 1", "set btn poll"])),
    # 6.2 (3) UART command flood
    "U1_flood_s2": ("6.2-3 flood of 'ping' commands during S2", ["scn 2 20", ("flood", 12), ("wait", r"SCN DONE n=2", 40)]),
    "U2_flood_s3": ("6.2-3 flood of 'ping' commands during S3", ["scn 3 20", ("flood", 12), ("wait", r"SCN DONE n=3", 40)]),
    # 6.2 (4) sensor failure: the SDA switch is toggled by hand/computer use while this runs
    "F1_sensor": ("6.2-4 sensor lost and reconnected (slide switch in SDA)",
                  custom(40, ["set tlm 500", "set jrn 0", "set lop 0", "set stim 0"])),
    # 6.2 (5) breaking point
    "B_imu_1": ("6.2-5 IMU period 20 ms under S3 load", custom(10, CFG_S3)),
    "B_imu_2": ("6.2-5 IMU period 10 ms under S3 load", custom(10, CFG_S3[1:] + ["set imu 10"])),
    "B_imu_3": ("6.2-5 IMU period 5 ms under S3 load", custom(10, CFG_S3[1:] + ["set imu 5"])),
    "B_imu_4": ("6.2-5 IMU period 2 ms under S3 load", custom(10, CFG_S3[1:] + ["set imu 2"])),
    "B_imu_5": ("6.2-5 IMU period 1 ms under S3 load", custom(10, CFG_S3[1:] + ["set imu 1"])),
    "B_tlm_1": ("6.2-5 telemetry every 100 ms", custom(10, ["set imu 20", "set jrn 0", "set lop 0", "set stim 0", "set tlm 100"])),
    "B_tlm_2": ("6.2-5 telemetry every 50 ms", custom(10, ["set imu 20", "set jrn 0", "set lop 0", "set stim 0", "set tlm 50"])),
    "B_tlm_3": ("6.2-5 telemetry every 20 ms", custom(10, ["set imu 20", "set jrn 0", "set lop 0", "set stim 0", "set tlm 20"])),
    "B_tlm_4": ("6.2-5 telemetry every 10 ms", custom(10, ["set imu 20", "set jrn 0", "set lop 0", "set stim 0", "set tlm 10"])),
    "B_lop_1": ("6.2-5 sector rewrite every 500 ms", custom(10, ["set imu 20", "set tlm 0", "set jrn 0", "set stim 0", "set lop 500"])),
    "B_lop_2": ("6.2-5 sector rewrite every 200 ms", custom(10, ["set imu 20", "set tlm 0", "set jrn 0", "set stim 0", "set lop 200"])),
    "B_lop_3": ("6.2-5 sector rewrite every 100 ms", custom(10, ["set imu 20", "set tlm 0", "set jrn 0", "set stim 0", "set lop 100"])),
    "B_lop_4": ("6.2-5 sector rewrite every 60 ms", custom(10, ["set imu 20", "set tlm 0", "set jrn 0", "set stim 0", "set lop 60"])),
    # 6.2 (8) journal integrity (power loss emulated by a torn write)
    "J1_journal": ("6.2-8 journal written in S2, then CRC check, torn write, CRC check",
                   ["scn 2 15", ("wait", r"SCN DONE n=2", 45), "jv", "jtorn", "jv"]),
}
# 6.2 (6) one hour of S2 and 6.2 (7) restarts are separate commands (see below)


# --------------------------------------------------------------------------- serial helper
class Dev:
    def __init__(self):
        self.p = serial.serial_for_url(URL, baudrate=115200, timeout=0.05)
        self.log = ""
        self.pos = 0

    def pump(self):
        d = self.p.read(4096)
        if d:
            self.log += d.decode("utf-8", errors="replace")
        return len(d)

    def send(self, cmd):
        self.log += ">> %s\n" % cmd
        # the Wokwi UART drops bytes of a fast burst, so commands are typed char by char
        for ch in (cmd + "\r").encode():
            self.p.write(bytes([ch]))
            time.sleep(CHAR_DELAY)
            self.pump()

    def wait(self, pattern, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            self.pump()
            m = rx.search(self.log, self.pos)
            if m:
                self.pos = m.end()
                return True
        return False

    def settle(self, sec=0.3):
        end = time.time() + sec
        while time.time() < end:
            self.pump()


def flood(dev, seconds):
    """Send 'ping' commands without pauses; count how many PONG lines came back."""
    end = time.time() + seconds
    chunk = b"ping\r" * 40
    sent = 0
    dev.log += ">> [flood of ping commands for %d s]\n" % seconds
    while time.time() < end:
        dev.p.write(chunk)
        sent += 40
        dev.pump()
    dev.log += "FLOOD sent=%d\n" % sent
    return sent


def run_steps(dev, steps):
    for st in steps:
        if isinstance(st, str):
            dev.send(st)
            dev.settle(0.4)
        elif st[0] == "wait":
            if not dev.wait(st[1], st[2]):
                dev.log += "TIMEOUT waiting for %s\n" % st[1]
                return False
        elif st[0] == "flood":
            flood(dev, st[1])
    dev.settle(0.5)
    return True


# --------------------------------------------------------------------------- parsing
KV = re.compile(r"(\w+)=(\S+)")


def kv(line):
    out = {}
    for k, v in KV.findall(line):
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = v
    return out


def parse(text):
    res = {"stats": {}, "events": [], "tlm": 0, "pong": 0}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("SCN BEGIN"):
            res["begin"] = kv(line)
        elif line.startswith("SCN END"):
            res["end"] = kv(line)
        elif line.startswith("STAT "):
            d = kv(line)
            res["stats"][d.pop("name")] = d
        elif line.startswith("JIT "):
            d = kv(line)
            h = re.search(r"hist=([\d,]+)", line)
            d["hist"] = [int(x) for x in h.group(1).split(",")] if h else []
            res["jit"] = d
        elif line.startswith("MEM "):
            res["mem"] = kv(line)
        elif line.startswith("RX "):
            res["rx"] = kv(line)
        elif line.startswith("EVLOG "):
            res["evlog"] = kv(line)
        elif line.startswith("EVT "):
            d = kv(line)
            res["events"].append((d.get("t_us"), d.get("lat_us")))
        elif line.startswith("JOURNAL "):
            res.setdefault("journal", []).append(kv(line))
        elif line.startswith("TLM "):
            res["tlm"] += 1
        elif line.startswith("PONG"):
            res["pong"] += 1
        elif line.startswith("FLOOD"):
            res["flood"] = kv(line)
        elif line.startswith("ERROR ") or line.startswith("INFO "):
            res.setdefault("notes", []).append(line)
        elif line.startswith("MODE "):
            res.setdefault("notes", []).append(line)
    return res


def pct(values, q):
    if not values:
        return None
    s = sorted(values)
    return s[min(len(s) - 1, int(round(q * (len(s) - 1))))]


def load_results():
    p = os.path.join(METRICS, "results.json")
    if os.path.exists(p):
        with open(p, encoding="utf-8") as fh:
            return json.load(fh)
    return {}


def save_results(r):
    os.makedirs(METRICS, exist_ok=True)
    with open(os.path.join(METRICS, "results.json"), "w", encoding="utf-8") as fh:
        json.dump(r, fh, indent=1, sort_keys=True)


# --------------------------------------------------------------------------- commands
def cmd_run(names):
    os.makedirs(METRICS, exist_ok=True)
    dev = Dev()
    dev.settle(0.5)
    results = load_results()
    for name in names:
        if name not in SCENARIOS:
            print("unknown scenario", name)
            continue
        desc, steps = SCENARIOS[name]
        print("==", name, "-", desc, flush=True)
        dev.log = ""
        dev.pos = 0
        dev.send("help")  # flush the line editor
        dev.settle(0.3)
        t0 = time.time()
        ok = run_steps(dev, steps)
        for extra in ("jit", "mem", "rx"):
            pass
        with open(os.path.join(METRICS, name + ".log"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(dev.log.replace("\r", ""))
        r = parse(dev.log)
        r["desc"] = desc
        r["ok"] = ok
        r["host_seconds"] = round(time.time() - t0, 1)
        lat = [l for _t, l in r["events"] if l is not None]
        if lat:
            r["event_lat_p99_us"] = pct(lat, 0.99)
            r["event_lat_p50_us"] = pct(lat, 0.50)
        results[name] = r
        save_results(results)
        s = r["stats"]
        print("   imu n=%s max=%s  loop max=%s  ev_lat n=%s max=%s  jit=%s" % (
            s.get("imu", {}).get("n"), s.get("imu", {}).get("max_us"), s.get("loop", {}).get("max_us"),
            s.get("ev_lat", {}).get("n"), s.get("ev_lat", {}).get("max_us"),
            (r.get("jit") or {}).get("max_us")), flush=True)


def cmd_boot(count):
    """6.2-7: reset the MCU `count` times and keep the boot logs (time from reset to READY by MCU clock)."""
    os.makedirs(METRICS, exist_ok=True)
    dev = Dev()
    dev.settle(0.5)
    out = []
    for i in range(count):
        dev.log = ""
        dev.pos = 0
        dev.send("reboot")
        ok = dev.wait(r"READY type", 20)
        dev.settle(0.3)
        t = dict(re.findall(r"(\w+_ms)=(\d+)", dev.log))
        res = {"run": i + 1, "ok": ok}
        m = re.search(r"SELFTEST BEGIN version=\S+ t_ms=(\d+)", dev.log)
        res["selftest_begin_ms"] = int(m.group(1)) if m else None
        m = re.search(r"DEV mpu6050_data status=OK", dev.log)
        res["imu_ok"] = bool(m)
        m = re.search(r"SELFTEST RESULT pass=(\d+) fail=(\d+) t_ms=(\d+)", dev.log)
        if m:
            res["selftest_end_ms"] = int(m.group(3))
            res["pass"], res["fail"] = int(m.group(1)), int(m.group(2))
        pw = re.findall(r"PWM_DEMO ch=1 us=1500 t_ms=(\d+)", dev.log)
        res["ready_ms"] = int(pw[0]) + 100 if pw else None
        out.append(res)
        print(res, flush=True)
        with open(os.path.join(METRICS, "boot_%d.log" % (i + 1)), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(dev.log.replace("\r", ""))
    results = load_results()
    results["boot"] = out
    save_results(results)


def cmd_long(seconds):
    """6.2-6: long S2 run, with a status line (stack / heap / journal) every 5 minutes."""
    os.makedirs(METRICS, exist_ok=True)
    dev = Dev()
    dev.settle(0.5)
    dev.log = ""
    dev.send("scn 2 %d" % seconds)
    path = os.path.join(METRICS, "long_s2.log")
    start = time.time()
    last = start
    end = start + seconds + 120
    done = False
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        pos = 0
        while time.time() < end and not done:
            dev.pump()
            tail = dev.log[pos:]
            # keep the file small: drop the 1 Hz TLM lines, keep everything else
            keep = [l for l in tail.splitlines(True) if not l.startswith("TLM ")]
            if "\n" in tail:
                cut = tail.rfind("\n") + 1
                fh.write("".join(l for l in tail[:cut].replace("\r", "").splitlines(True) if not l.startswith("TLM ")))
                pos += cut
                fh.flush()
            if "SCN DONE" in dev.log:
                done = True
            if time.time() - last > 300:
                last = time.time()
                dev.send("mem")
                dev.send("jv")
                dev.send("rx")
    print("long run finished", done, flush=True)


def restart_sim(wait=9):
    """Wokwi restarts (and exports wokwi.vcd of the finished session) when the ELF changes on disk."""
    elf = os.path.join(ROOT, "firmware", "build", "firmware.elf")
    now = time.time()
    os.utime(elf, (now, now))
    time.sleep(wait)


def set_analyzer(attrs):
    """Patch the attrs of the logic analyzer in diagram.json (trigger pin / level / buffer)."""
    p = os.path.join(ROOT, "diagram.json")
    txt = open(p, encoding="utf-8").read()
    pat = re.compile(r'("id": "logic1",\s*"top": -?\d+,\s*"left": -?\d+,\s*"attrs": )\{[^}]*\}')
    body = json.dumps(attrs, indent=2).replace("\n", "\n      ") if attrs else "{}"
    new, n = pat.subn(lambda m: m.group(1) + body, txt)
    if n != 1:
        raise SystemExit("logic1 block not found in diagram.json")
    with open(p, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(new)


def cmd_capture(name, scenario, trig="D3", level="low", buf="15000000"):
    """Trigger on one pin, run one scenario in a fresh session, restart, copy the exported VCD.

    The Wokwi analyzer keeps a fixed buffer (30 MHz sampling -> 1M samples = 33 ms), so a trigger
    and a bigger buffer decide *which* part of the scenario is recorded."""
    import shutil
    set_analyzer({"triggerMode": "edge", "triggerPin": trig, "triggerLevel": level, "bufferSize": buf})
    restart_sim()
    cmd_run([scenario])
    restart_sim()
    dst = os.path.join(ROOT, "docs", "captures", "lr3_%s.vcd" % name)
    shutil.copyfile(os.path.join(ROOT, "wokwi.vcd"), dst)
    set_analyzer({})
    print("VCD ->", dst, os.path.getsize(dst), "bytes", flush=True)


def cmd_raw(cmds):
    dev = Dev()
    dev.settle(0.3)
    for c in cmds:
        dev.send(c)
        dev.settle(0.6)
    print(dev.log.replace("\r", ""))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    r = sub.add_parser("run")
    r.add_argument("names", nargs="+")
    b = sub.add_parser("boot")
    b.add_argument("count", type=int, nargs="?", default=5)
    l = sub.add_parser("long")
    l.add_argument("seconds", type=int)
    c = sub.add_parser("capture")
    c.add_argument("name")
    c.add_argument("scenario")
    c.add_argument("trig", nargs="?", default="D3")
    c.add_argument("level", nargs="?", default="low")
    c.add_argument("buf", nargs="?", default="15000000")
    w = sub.add_parser("raw")
    w.add_argument("cmds", nargs="+")
    a = ap.parse_args()
    if a.cmd == "list":
        for n, (d, _s) in SCENARIOS.items():
            print("%-16s %s" % (n, d))
    elif a.cmd == "run":
        cmd_run(a.names)
    elif a.cmd == "boot":
        cmd_boot(a.count)
    elif a.cmd == "long":
        cmd_long(a.seconds)
    elif a.cmd == "capture":
        cmd_capture(a.name, a.scenario, a.trig, a.level, a.buf)
    elif a.cmd == "raw":
        cmd_raw(a.cmds)


if __name__ == "__main__":
    sys.exit(main())
