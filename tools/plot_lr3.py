#!/usr/bin/env python3
"""LR3 figures (docs/lab_3/img/): waveforms from the VCD windows and charts from docs/metrics/results.json.

  python tools/plot_lr3.py [OUT_DIR]            (needs matplotlib)
"""
import json
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from vcd_tool import parse  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "lab_3", "img")
CAP = os.path.join(ROOT, "docs", "captures")
MET = os.path.join(ROOT, "docs", "metrics")
os.makedirs(OUT, exist_ok=True)

LABELS = [("D3", "PB2 stimulus (button edge)"), ("D2", "PA1 event reaction"),
          ("D1", "PB7 long operation"), ("D0", "PB6 IMU task")]


def wave(path, title, out, annotate=None):
    names, ev, _ = parse(path)
    t_end = ev[-1][0]
    fig, ax = plt.subplots(figsize=(11, 4.2))
    for i, (ch, lab) in enumerate(LABELS):
        ts, vs = [0], []
        cur = 0
        pts = [(0, 0)]
        for t, c, v in ev:
            if c != ch:
                continue
            val = int(v)
            pts.append((t, cur))
            pts.append((t, val))
            cur = val
        pts.append((t_end, cur))
        base = i * 1.5
        ax.plot([p[0] / 1e6 for p in pts], [p[1] + base for p in pts], lw=1.4, color="#1f4e79")
        ax.text(-0.01, base + 0.5, lab, ha="right", va="center", fontsize=9, transform=ax.get_yaxis_transform())
    if annotate:
        for x0, x1, y, txt in annotate:
            ax.annotate("", xy=(x1, y), xytext=(x0, y), arrowprops=dict(arrowstyle="<->", color="#c00000"))
            ax.text((x0 + x1) / 2, y + 0.12, txt, ha="center", fontsize=9, color="#c00000")
    ax.set_yticks([])
    ax.set_xlabel("time, ms (logic analyzer, 30 MHz)")
    ax.set_title(title, fontsize=11)
    ax.grid(axis="x", alpha=0.3)
    ax.set_xlim(0, t_end / 1e6)
    fig.subplots_adjust(left=0.2, right=0.98, bottom=0.14, top=0.9)
    fig.savefig(os.path.join(OUT, out), dpi=150)
    plt.close(fig)


def first_event(path):
    _, ev, _ = parse(path)
    fall = [t for t, c, v in ev if c == "D3" and v == "0" and t > 1e6]
    rise = [t for t, c, v in ev if c == "D2" and v == "1"]
    if not fall:
        return None
    t0 = fall[0]
    t1 = next((r for r in rise if r >= t0), None)
    return t0 / 1e6, (t1 / 1e6 if t1 else None)


def waves():
    p = os.path.join(CAP, "lr3_event_during_longop.vcd")
    if os.path.exists(p):
        e = first_event(p)
        ann = [(e[0], e[1], 5.2, "%.1f ms" % (e[1] - e[0]))] if e and e[1] else None
        wave(p, "Event during a long operation (sector erase): PB2 falling edge -> PA1 reaction", "lr3_wave_event_longop.png", ann)
    p = os.path.join(CAP, "lr3_event_idle_loop.vcd")
    if os.path.exists(p):
        e = first_event(p)
        ann = [(e[0], e[1], 5.2, "%.0f us" % ((e[1] - e[0]) * 1000))] if e and e[1] else None
        wave(p, "Event when the loop is idle: PB2 falling edge -> PA1 reaction", "lr3_wave_event_idle.png", ann)


def load():
    return json.load(open(os.path.join(MET, "results.json"), encoding="utf-8"))


def breaking(res):
    fig, axs = plt.subplots(1, 3, figsize=(12, 3.8))
    groups = [("B_imu_", "IMU period under S3 load, ms", [20, 10, 5, 2, 1]),
              ("B_tlm_", "telemetry period, ms (IMU 20 ms)", [100, 50, 20, 10]),
              ("B_lop_", "sector rewrite period, ms (IMU 20 ms)", [500, 200, 100, 60])]
    for ax, (pre, xl, xs) in zip(axs, groups):
        load_, jit, miss = [], [], []
        for i in range(len(xs)):
            r = res[pre + str(i + 1)]
            load_.append(r["stats"]["load"]["avg_pct"])
            jit.append(r["jit"]["max_us"] / 1000.0)
            miss.append(r["jit"]["missed"])
        idx = list(range(len(xs)))
        ax.bar(idx, load_, color="#9dc3e6")
        ax.set_ylabel("CPU load, %")
        ax.set_xticks(idx)
        ax.set_xticklabels([str(x) for x in xs])
        ax.set_xlabel(xl, fontsize=9)
        ax.set_ylim(0, 105)
        ax2 = ax.twinx()
        ax2.plot(idx, jit, "o-", color="#c00000")
        ax2.set_ylabel("max IMU jitter, ms", color="#c00000")
        ax2.set_yscale("log")
    fig.suptitle("Breaking-point search: CPU load (bars) and max IMU jitter (line, log scale)", fontsize=11)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "lr3_breaking_point.png"), dpi=150)
    plt.close(fig)


def evt(log):
    out = []
    for line in open(os.path.join(MET, log), encoding="utf-8", errors="replace"):
        m = re.match(r"EVT n=\d+ t_us=\d+ lat_us=(\d+)", line)
        if m:
            out.append(int(m.group(1)))
    return out


def latency():
    sets = [("E1 EXTI, idle", "E1_irq_idle.log"), ("E2 EXTI, sector rewrite", "E2_irq_longop.log"),
            ("E3 polling, sector rewrite", "E3_poll_longop.log"), ("S3 EXTI, worst case", "S3.log")]
    fig, ax = plt.subplots(figsize=(9, 4))
    data = [[x / 1000.0 for x in evt(f)] for _, f in sets]
    ax.boxplot(data, vert=False, whis=(1, 99), showfliers=True)
    ax.set_yticklabels(["%s (n=%d)" % (s[0], len(d)) for s, d in zip(sets, data)])
    ax.set_xscale("log")
    ax.set_xlabel("event latency (edge -> handler start), ms, log scale")
    ax.axvline(50, color="#c00000", ls="--")
    ax.text(50, 0.62, " 50 ms limit", color="#c00000", fontsize=9)
    ax.grid(axis="x", alpha=0.3)
    ax.set_title("Reaction latency to the button edge", fontsize=11)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "lr3_event_latency.png"), dpi=150)
    plt.close(fig)


if __name__ == "__main__":
    waves()
    res = load()
    breaking(res)
    latency()
    print("figures ->", OUT)
