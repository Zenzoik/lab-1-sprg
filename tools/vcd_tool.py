#!/usr/bin/env python3
"""Small VCD helper for logic-analyzer captures exported by Wokwi.

  vcd_tool.py summary FILE            edges per channel, first/last timestamp
  vcd_tool.py edges FILE CH [N]       first N edges of channel CH (name as in $var)
  vcd_tool.py window FILE T0 T1 OUT   cut [T0,T1] ns into OUT (timestamps re-based to 0)
  vcd_tool.py freq FILE CH [T0 T1]    mean period / frequency of rising edges of CH

Time unit follows $timescale of the file (Wokwi: 1 ns).
"""
import sys
from collections import Counter


def parse(path):
    names = {}
    events = []  # (t, name, value)
    header = []
    t = 0
    in_header = True
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if in_header:
                header.append(raw.rstrip("\n"))
                if line.startswith("$var"):
                    parts = line.split()
                    names[parts[3]] = parts[4]
                if line.startswith("$enddefinitions"):
                    in_header = False
                continue
            if not line:
                continue
            if line[0] == "#":
                t = int(line[1:])
            elif line[0] in "01xzXZ" and line[1:] in names:
                events.append((t, names[line[1:]], line[0]))
    return names, events, header


def cmd_summary(path):
    names, ev, _ = parse(path)
    cnt = Counter(e[1] for e in ev)
    print("channels:", ", ".join(sorted(names.values())))
    for ch in sorted(names.values()):
        es = [e for e in ev if e[1] == ch]
        span = (es[0][0], es[-1][0]) if es else (None, None)
        print(f"{ch}: edges={cnt[ch]} first={span[0]} last={span[1]}")
    if ev:
        print("last timestamp:", ev[-1][0])


def cmd_edges(path, ch, n=20):
    _, ev, _ = parse(path)
    for e in [x for x in ev if x[1] == ch][:n]:
        print(e[0], e[2])


def cmd_window(path, t0, t1, out):
    names, ev, header = parse(path)
    ids = {v: k for k, v in names.items()}
    state = {}
    for t, ch, v in ev:
        if t < t0:
            state[ch] = v
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(header) + "\n#0\n")
        for ch, v in sorted(state.items()):
            fh.write(f"{v}{ids[ch]}\n")
        last = None
        for t, ch, v in ev:
            if t0 <= t <= t1:
                if t != last:
                    fh.write(f"#{t - t0}\n")
                    last = t
                fh.write(f"{v}{ids[ch]}\n")
        fh.write(f"#{t1 - t0}\n")


def cmd_freq(path, ch, t0=None, t1=None):
    _, ev, _ = parse(path)
    r = [t for t, c, v in ev if c == ch and v == "1" and (t0 is None or t0 <= t <= t1)]
    if len(r) < 2:
        print("not enough rising edges")
        return
    per = [b - a for a, b in zip(r, r[1:])]
    mean = sum(per) / len(per)
    print(f"rising edges={len(r)} mean period={mean:.1f} ns freq={1e9 / mean:.1f} Hz "
          f"min={min(per)} ns max={max(per)} ns")


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    c, path = argv[1], argv[2]
    if c == "summary":
        cmd_summary(path)
    elif c == "edges":
        cmd_edges(path, argv[3], int(argv[4]) if len(argv) > 4 else 20)
    elif c == "window":
        cmd_window(path, int(argv[3]), int(argv[4]), argv[5])
    elif c == "freq":
        if len(argv) > 5:
            cmd_freq(path, argv[3], int(argv[4]), int(argv[5]))
        else:
            cmd_freq(path, argv[3])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
