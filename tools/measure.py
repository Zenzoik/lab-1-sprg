#!/usr/bin/env python3
"""Numbers quoted in docs/captures/analysis.md, computed from the VCD records.

  python3 tools/measure.py            (run from the repository root; `make measure`)

Two records are used:
  docs/captures/session_boot.vcd      main diagram: D0 SCL, D1 SDA, D2 SCK, D3 MOSI,
                                      D4 MISO, D5 CS#, D6 UART TX, D7 PWM1
  docs/captures/adc/session_adc.vcd   ADC diagram: D0 marker PB1, D1 SCL, D2 SDA,
                                      D3 UART TX, D4 PWM1
All timestamps are nanoseconds (Wokwi $timescale 1ns).
"""
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import vcd_tool  # noqa: E402

CAP = os.path.join(ROOT, "docs", "captures")


def load(path):
    _, events, _ = vcd_tool.parse(path)
    ch = {}
    for t, name, v in events:
        ch.setdefault(name, []).append((t, int(v)))
    return ch


def level_at(edges, t):
    v = 0
    for te, ve in edges:
        if te > t:
            break
        v = ve
    return v


def rising(edges, t0=0, t1=1 << 62):
    return [t for (t, v) in edges if v == 1 and t0 <= t <= t1]


def pulses(edges, t0=0, t1=1 << 62):
    """(start, width) of every high pulse inside [t0, t1]."""
    out, start = [], None
    for t, v in edges:
        if t < t0 or t > t1:
            continue
        if v == 1:
            start = t
        elif start is not None:
            out.append((start, t - start))
            start = None
    return out


def starts_stops(scl, sda, t0, t1, guard=1000):
    """I2C START (SDA falls while SCL high) and STOP (SDA rises while SCL high).

    SDA edges that coincide with an SCL edge (within `guard` ns) are data changes
    caught at the clock transition, not conditions, and are ignored.
    """
    scl_t = [t for t, _ in scl]
    import bisect
    st, sp = [], []
    for t, v in sda:
        if t < t0 or t > t1:
            continue
        i = bisect.bisect_left(scl_t, t - guard)
        j = bisect.bisect_right(scl_t, t + guard)
        if j > i:
            continue
        if level_at(scl, t) == 1:
            (st if v == 0 else sp).append(t)
    return st, sp


def us(x):
    return "%.2f us" % (x / 1000.0)


def main():
    main_vcd = os.path.join(CAP, "session_boot.vcd")
    adc_vcd = os.path.join(CAP, "adc", "session_adc.vcd")
    ms = 1_000_000

    c = load(main_vcd)
    scl, sda, sck, mosi, miso, cs, tx, pwm = (c[k] for k in
                                              ("D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"))

    print("== I2C (main record) ==")
    t0, t1 = int(48.5 * ms), int(50.4 * ms)
    r = rising(scl, t0, t1)
    periods = [b - a for a, b in zip(r, r[1:]) if b - a < 20000]
    print("SCL rising edges in burst window:", len(r))
    print("SCL period median %s, min %s, max %s -> %.1f kHz" % (
        us(statistics.median(periods)), us(min(periods)), us(max(periods)),
        1e6 / statistics.median(periods)))
    hi = [t for t, v in scl if v == 0 and t0 <= t <= t1]
    widths_hi = []
    for a, b in pulses(scl, t0, t1):
        widths_hi.append(b)
    print("SCL high width median %s" % us(statistics.median(widths_hi)))
    st, sp = starts_stops(scl, sda, t0, t1)
    print("START conditions:", len(st), "STOP conditions:", len(sp))
    durs = []
    for a in st:
        b = min([x for x in sp if x > a], default=None)
        if b:
            durs.append(b - a)
    print("START->STOP durations (us):", sorted(set(round(d / 1000.0, 1) for d in durs)))
    print("longest transaction (burst read) %s, shortest %s" % (us(max(durs)), us(min(durs))))

    print("\n== manual decode of first byte of i2c_whoami (SDA sampled at SCL rising) ==")
    t0, t1 = int(8.5 * ms), int(9.5 * ms)
    st, sp = starts_stops(scl, sda, t0, t1)
    if st:
        a = st[0]
        bits = []
        for t in rising(scl, a, a + 200000)[:9]:
            bits.append(level_at(sda, t - 1))
        print("START at %s; SDA at 9 SCL rising edges: %s" % (us(a - t0), bits))
        byte = 0
        for b in bits[:8]:
            byte = (byte << 1) | b
        print("first byte = 0x%02X -> addr7=0x%02X R/W=%d ACK bit=%d (0=ACK)" % (
            byte, byte >> 1, byte & 1, bits[8]))

    print("\n== SPI (main record) ==")
    t0, t1 = int(60.5 * ms), int(61.3 * ms)
    r = rising(sck, t0, t1)
    periods = [b - a for a, b in zip(r, r[1:]) if b - a < 5000]
    print("SCK rising edges:", len(r), "period median %s -> %.2f MHz" % (
        us(statistics.median(periods)), 1e3 / statistics.median(periods)))
    lows = [(t, v) for t, v in cs if t0 <= t <= t1]
    groups, cur = [], [r[0]]
    for a_, b_ in zip(r, r[1:]):
        if b_ - a_ > 2000:
            groups.append(cur)
            cur = []
        cur.append(b_)
    groups.append(cur)
    print("SCK bursts (bytes):", [len(g) for g in groups],
          "gaps between bursts:", [us(g2[0] - g1[-1]) for g1, g2 in zip(groups, groups[1:])])
    print("CS edges:", [(us(t - t0), v) for t, v in lows])
    fall = [t for t, v in lows if v == 0]
    rise_ = [t for t, v in lows if v == 1]
    if fall and rise_:
        a = fall[0]
        b = min(x for x in rise_ if x > a)
        print("CS low (JEDEC transaction) %s; first SCK edge %s after CS fall" % (
            us(b - a), us(min(x for x in r if x > a) - a)))

    print("\n== UART (main record) ==")
    t0, t1 = 0, int(16.1 * ms)
    bits = [b for _, b in pulses(tx, t0, t1)]
    lows_ = []
    prev = None
    for t, v in tx:
        if t < t0 or t > t1:
            continue
        if prev is not None and prev[1] == 0 and v == 1:
            lows_.append(t - prev[0])
        prev = (t, v)
    print("shortest low pulse (= 1 bit time) %s -> %.0f baud" % (
        us(min(lows_)), 1e9 / min(lows_)))

    print("\n== PWM (main record) ==")
    pw = pulses(pwm, int(150 * ms), int(520 * ms))
    from collections import Counter
    cnt = Counter(round(w / 1000.0 / 10.0) * 10 for _, w in pw)
    print("pulse widths (us, rounded to 10) -> count:", dict(sorted(cnt.items())))
    per = [b[0] - a[0] for a, b in zip(pw, pw[1:])]
    print("period median %s" % us(statistics.median(per)))

    if os.path.exists(adc_vcd):
        a = load(adc_vcd)
        mk, scl2, sda2, tx2, pwm2 = (a[k] for k in ("D0", "D1", "D2", "D3", "D4"))
        print("\n== ADC marker (ADC record) ==")
        pm = pulses(mk)
        w = [x[1] for x in pm[1:]]
        print("marker pulses:", len(pm), "width median %s min %s max %s" % (
            us(statistics.median(w)), us(min(w)), us(max(w))))
        per = [b[0] - a_[0] for a_, b in zip(pm[1:], pm[2:])]
        print("marker period median %s -> %.2f Hz" % (
            us(statistics.median(per)), 1e9 / statistics.median(per)))
        lat = []
        for s, wd in pm[1:60]:
            fall_t = s + wd
            st, _ = starts_stops(scl2, sda2, fall_t, fall_t + 5 * ms)
            if st:
                lat.append(st[0] - fall_t)
        print("marker fall -> next I2C START: n=%d median %s min %s max %s" % (
            len(lat), us(statistics.median(lat)), us(min(lat)), us(max(lat))))
        lat2 = []
        for s, wd in pm[1:60]:
            st, _ = starts_stops(scl2, sda2, s - 5 * ms, s)
            prevst = [x for x in st if x < s]
            if prevst:
                lat2.append(s - prevst[-1])
        if lat2:
            print("last I2C START before marker rise -> marker rise: median %s" % (
                us(statistics.median(lat2))))
        t_first = pm[1][0]
        print("first marker pulse at %s (control loop start)" % us(t_first))
    else:
        print("\n(no ADC record: %s)" % adc_vcd)


if __name__ == "__main__":
    main()
