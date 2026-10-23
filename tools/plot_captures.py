#!/usr/bin/env python3
"""Render annotated waveform figures from the Wokwi VCD records (docs/lab_2/img/).

PulseView is not used (form B, no GUI analyzer on the build host): the figures are drawn
directly from the same VCD files that sigrok-cli decodes, so every label can be checked
against docs/captures/*.txt and docs/captures/measurements.txt.

  python3 tools/plot_captures.py [OUT_DIR]       (needs matplotlib)
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import measure  # noqa: E402

CAP = os.path.join(ROOT, "docs", "captures")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "lab_2", "img")
os.makedirs(OUT, exist_ok=True)
MS = 1_000_000
COL = {"clk": "#1f77b4", "data": "#d62728", "aux": "#2ca02c", "mark": "#9467bd"}


def steps(edges, t0, t1):
    """Step-plot arrays for [t0, t1] in microseconds relative to t0."""
    lv = measure.level_at(edges, t0)
    xs, ys = [0.0], [lv]
    for t, v in edges:
        if t0 < t <= t1:
            xs += [(t - t0) / 1000.0, (t - t0) / 1000.0]
            ys += [ys[-1], v]
    xs.append((t1 - t0) / 1000.0)
    ys.append(ys[-1])
    return xs, ys


def draw(ax_defs, t0, t1, title, fname, notes=(), width=13.0):
    """ax_defs: list of (label, edges, colour). notes: (x_us, row, text)."""
    fig, ax = plt.subplots(figsize=(width, 1.0 + 0.95 * len(ax_defs)))
    for i, (label, edges, col) in enumerate(ax_defs):
        base = (len(ax_defs) - 1 - i) * 1.5
        xs, ys = steps(edges, t0, t1)
        ax.plot(xs, [base + 0.9 * y for y in ys], color=col, lw=1.3, drawstyle="default")
        ax.text(-0.01, base + 0.45, label, transform=ax.get_yaxis_transform(),
                ha="right", va="center", fontsize=9)
    for x, row, text in notes:
        base = (len(ax_defs) - 1 - row) * 1.5
        ax.annotate(text, (x, base + 1.0), xytext=(x, base + 1.38), fontsize=8,
                    ha="center", va="bottom", color="black",
                    arrowprops=dict(arrowstyle="-", color="gray", lw=0.6))
    ax.set_xlim(0, (t1 - t0) / 1000.0)
    ax.set_ylim(-0.2, len(ax_defs) * 1.5 + 0.2)
    ax.set_yticks([])
    ax.set_xlabel("t, мкс (від початку вікна)")
    ax.set_title(title, fontsize=10)
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.grid(axis="x", alpha=0.25)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, fname), dpi=140)
    plt.close(fig)
    print("wrote", fname)


def i2c_frames(scl, sda, t0, t1):
    st, sp = measure.starts_stops(scl, sda, t0, t1)
    ev = sorted([(t, "S") for t in st] + [(t, "P") for t in sp])
    frames = []
    for k, (t, kind) in enumerate(ev):
        frames.append((t, kind, None, None))
        if kind == "S":
            end = ev[k + 1][0] if k + 1 < len(ev) else t1
            rises = measure.rising(scl, t, end)
            bits = [measure.level_at(sda, r - 1) for r in rises]
            for n in range(0, len(bits) - 8, 9):
                byte = 0
                for b in bits[n:n + 8]:
                    byte = (byte << 1) | b
                ack = bits[n + 8]
                frames.append((rises[n + 4], "B", byte, ack))
    return frames


def fig_i2c(scl, sda, t0_ms, t1_ms, title, fname, ch_names=("SCL (PA9)", "SDA (PA10)")):
    t0, t1 = int(t0_ms * MS), int(t1_ms * MS)
    notes = []
    first_read = True
    for t, kind, byte, ack in i2c_frames(scl, sda, t0, t1):
        x = (t - t0) / 1000.0
        if kind in "SP":
            notes.append((x, 1, kind))
        else:
            txt = "0x%02X%s" % (byte, " A" if ack == 0 else " N")
            notes.append((x, 1, txt))
    draw([(ch_names[0], scl, COL["clk"]), (ch_names[1], sda, COL["data"])], t0, t1, title,
         fname, notes)


def fig_spi_jedec(c):
    t0, t1 = int(60.7 * MS), int(61.15 * MS)
    sck, mosi, miso, cs = c["D2"], c["D3"], c["D4"], c["D5"]
    rises = measure.rising(sck, t0, t1)
    groups, cur = [], [rises[0]]
    for a, b in zip(rises, rises[1:]):
        if b - a > 2000:
            groups.append(cur)
            cur = []
        cur.append(b)
    groups.append(cur)
    notes = []
    for g in groups:
        mo = mi = 0
        for r in g[:8]:
            mo = (mo << 1) | measure.level_at(mosi, r)
            mi = (mi << 1) | measure.level_at(miso, r)
        mid = ((g[0] + g[-1]) / 2 - t0) / 1000.0
        notes.append((mid, 1, "MOSI %02X\nMISO %02X" % (mo, mi)))
    draw([("CS# (PB0)", cs, COL["aux"]), ("SCK (PB3)", sck, COL["clk"]),
          ("MOSI (PB5)", mosi, COL["data"]), ("MISO (PB4)", miso, COL["data"])],
         t0, t1, "SPI1: команда JEDEC ID (0x9F) → EF 40 17, mode 0, 3 МГц", "spi_jedec.png", notes)


def fig_spi_byte(c):
    """Zoom on the first byte (command 0x9F): bit values at SCK rising edges."""
    sck, mosi, miso, cs = c["D2"], c["D3"], c["D4"], c["D5"]
    t_lo, t_hi = int(60.7 * MS), int(61.15 * MS)
    rises = measure.rising(sck, t_lo, t_hi)
    first = rises[:8]
    t0, t1 = first[0] - 600, first[-1] + 600
    notes = []
    for r in first:
        notes.append(((r - t0) / 1000.0, 1, str(measure.level_at(mosi, r))))
    draw([("SCK (PB3)", sck, COL["clk"]), ("MOSI (PB5)", mosi, COL["data"]),
          ("MISO (PB4)", miso, COL["data"])], t0, t1,
         "SPI1: перший байт команди – біти MOSI у моменти наростаючого фронту SCK (1001 1111 = 0x9F)",
         "spi_byte.png", notes, width=11.0)


def fig_uart(c):
    tx = c["D6"]
    first = [t for t, v in tx if v == 0 and t > 100000][0]
    t0 = first - 40000
    bit = 8680
    t1 = t0 + 40000 + 10 * bit * 5
    notes = []
    t = first
    for _ in range(5):
        val = 0
        for k in range(8):
            val |= measure.level_at(tx, int(t + bit * (k + 1.5))) << k
        names = {13: "CR", 10: "LF"}
        ch = names.get(val, chr(val) if 32 <= val < 127 else "?")
        notes.append(((t + 5 * bit - t0) / 1000.0, 0, "%s (0x%02X)" % (ch, val)))
        t += 10 * bit
        # next start bit: first falling edge after the stop bit
        nxt = [x for x, v in tx if v == 0 and x >= t - bit // 2]
        if nxt:
            t = nxt[0]
    draw([("TX (PA2)", tx, COL["data"])], t0, t1,
         "USART2: перші символи банера (115200 8N1, біт ≈ 8.68 мкс, LSB першим)",
         "uart_bits.png", notes)


def fig_pwm(c):
    pwm = c["D7"]
    p = measure.pulses(pwm, int(150 * MS), int(520 * MS))
    picks = []
    for target in (1000, 1500, 2000):
        picks.append(next(x for x in p if abs(x[1] / 1000.0 - target) < 20))
    fig, axs = plt.subplots(3, 1, figsize=(13, 5.2), sharex=True)
    for ax, (s, w) in zip(axs, picks):
        t0 = s - 2_000_000
        t1 = s + 22_000_000
        xs, ys = steps(pwm, t0, t1)
        ax.plot([x / 1000.0 for x in xs], ys, color=COL["data"], lw=1.3)
        ax.annotate("ширина = %.0f мкс" % (w / 1000.0), (2.0 + w / 2000000.0, 1.0),
                    xytext=(5.0, 0.55), fontsize=9, arrowprops=dict(arrowstyle="->", lw=0.7))
        ax.set_yticks([0, 1])
        ax.set_ylabel("PA6")
        ax.grid(alpha=0.25)
    axs[0].set_title("TIM3 CH1: ширини імпульсу 1000 / 1500 / 2000 мкс, період 20 мс (50 Гц)",
                     fontsize=10)
    axs[-1].set_xlabel("t, мс")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "pwm_widths.png"), dpi=140)
    plt.close(fig)
    print("wrote pwm_widths.png")


def fig_adc(a):
    mk, scl, sda = a["D0"], a["D1"], a["D2"]
    pm = measure.pulses(mk)
    s, w = pm[3]
    t0, t1 = s - 150_000, s + w + 420_000
    notes = [(150.0 + w / 2000.0, 0, "ADC: %.1f мкс" % (w / 1000.0))]
    st, _ = measure.starts_stops(scl, sda, s + w, s + w + 400_000)
    if st:
        lat = st[0] - (s + w)
        notes.append(((st[0] - t0) / 1000.0 - 60, 2, "START: +%.1f мкс після спаду маркера" % (lat / 1000.0)))
    draw([("Маркер PB1", mk, COL["mark"]), ("SCL (PA9)", scl, COL["clk"]),
          ("SDA (PA10)", sda, COL["data"])], t0, t1,
         "Маркер ADC (PB1): тривалість перетворення і затримка до першої I2C-транзакції",
         "adc_marker.png", notes)


def main():
    c = measure.load(os.path.join(CAP, "session_boot.vcd"))
    a = measure.load(os.path.join(CAP, "adc", "session_adc.vcd"))
    fig_i2c(c["D0"], c["D1"], 8.5, 9.5, "I2C1: читання WHO_AM_I (0x75) з MPU-6050 (0x68)",
            "i2c_whoami.png")
    fig_i2c(c["D0"], c["D1"], 15.9, 18.1, "I2C1: ініціалізація MPU-6050 (запис PWR_MGMT_1, SMPLRT_DIV, CONFIG, GYRO/ACCEL_CONFIG)",
            "i2c_init.png")
    fig_i2c(c["D0"], c["D1"], 151.3, 151.7, "I2C1: аномалія – адреса 0x69 (AD0 = 1), NACK",
            "i2c_nack.png")
    fig_spi_jedec(c)
    fig_spi_byte(c)
    fig_uart(c)
    fig_pwm(c)
    fig_adc(a)


if __name__ == "__main__":
    main()
