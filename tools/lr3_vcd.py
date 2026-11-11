#!/usr/bin/env python3
"""LR3: measurements from the logic-analyzer VCD (independent of the MCU cycle counter).

Channels (diagram.json): D0 = PB6 IMU task window, D1 = PB7 long operation, D2 = PA1 event reaction,
D3 = PB2 stimulus (falling edge = button press edge).

  lr3_vcd.py pulses FILE CH            width / period statistics of the pulses on CH
  lr3_vcd.py events FILE [LOG]         PB2 fall -> PA1 rise latency; compared with EVT lines of LOG
  lr3_vcd.py window FILE T0_MS T1_MS OUT   cut a window (ms from the first sample) into OUT
"""
import re
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from vcd_tool import cmd_window, parse  # noqa: E402


def edges(ev, ch):
    return [(t, v) for t, c, v in ev if c == ch]


def pulses(ev, ch):
    """List of (t_rise, width) for high pulses."""
    out = []
    up = None
    for t, v in edges(ev, ch):
        if v == "1" and up is None:
            up = t
        elif v == "0" and up is not None:
            out.append((up, t - up))
            up = None
    return out


def stats(vals):
    vals = sorted(vals)
    n = len(vals)
    if not n:
        return {}
    return {"n": n, "min": vals[0], "avg": sum(vals) / n, "p50": vals[n // 2],
            "p99": vals[min(n - 1, int(0.99 * (n - 1) + 0.5))], "max": vals[-1]}


def cmd_pulses(path, ch):
    _, ev, _ = parse(path)
    pl = pulses(ev, ch)
    w = [x[1] / 1000.0 for x in pl]
    per = [(pl[i + 1][0] - pl[i][0]) / 1000.0 for i in range(len(pl) - 1)]
    print("%s pulses: %d" % (ch, len(pl)))
    print("width  us:", {k: round(v, 1) for k, v in stats(w).items()})
    print("period us:", {k: round(v, 1) for k, v in stats(per).items()})
    if per:
        nominal = sorted(per)[len(per) // 2]
        late = [p for p in per if p > 1.5 * nominal]
        print("nominal period %.0f us; periods > 1.5x nominal: %d (max %.0f us)" % (
            nominal, len(late), max(per)))


def evt_from_log(log):
    out = []
    for line in open(log, encoding="utf-8", errors="replace"):
        m = re.match(r"EVT n=(\d+) t_us=(\d+) lat_us=(\d+)", line)
        if m:
            out.append(int(m.group(3)))
    return out


def cmd_events(path, log=None):
    _, ev, _ = parse(path)
    # the pin is low for a few ms while the MCU is held in reset / before GPIO init: ignore edges before 100 ms
    fall = [t for t, v in edges(ev, "D3") if v == "0" and t > 100000000]
    rise = [t for t, v in edges(ev, "D2") if v == "1"]
    lat = []
    j = 0
    for k, t in enumerate(fall):
        while j < len(rise) and rise[j] < t:
            j += 1
        nxt = fall[k + 1] if k + 1 < len(fall) else None
        if j < len(rise) and (nxt is None or rise[j] < nxt):
            lat.append((rise[j] - t) / 1000.0)
    print("PB2 falling edges: %d, PA1 rising edges: %d" % (len(fall), len(rise)))
    print("analyzer latency us (PB2 fall -> PA1 rise):", {k: round(v, 1) for k, v in stats(lat).items()})
    if log:
        fw = evt_from_log(log)
        n = min(len(fw), len(lat))
        print("firmware EVT lines: %d; compared first %d events" % (len(fw), n))
        print("firmware latency us:", {k: round(v, 1) for k, v in stats(fw[:n]).items()})
        print("analyzer latency us:", {k: round(v, 1) for k, v in stats(lat[:n]).items()})
        d = [lat[i] - fw[i] for i in range(n)]
        print("analyzer - firmware, us:", {k: round(v, 1) for k, v in stats([abs(x) for x in d]).items()},
              "signed mean %.1f" % (sum(d) / n if n else 0))
        for i in range(min(8, n)):
            print("  event %d: firmware %d us, analyzer %.1f us" % (i + 1, fw[i], lat[i]))


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        print(__doc__)
    elif a[0] == "pulses":
        cmd_pulses(a[1], a[2])
    elif a[0] == "events":
        cmd_events(a[1], a[2] if len(a) > 2 else None)
    elif a[0] == "window":
        names, ev, _ = parse(a[1])
        base = ev[0][0]
        cmd_window(a[1], base + int(float(a[2]) * 1e6), base + int(float(a[3]) * 1e6), a[4])
