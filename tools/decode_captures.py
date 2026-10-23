#!/usr/bin/env python3
"""Decode the logic-analyzer captures in docs/captures with sigrok-cli.

For every entry of CAPTURES the VCD slice is cut out of the recorded session
(docs/captures/session_boot.vcd) with tools/vcd_tool.py, written next to the
analysis as <name>.vcd, and decoded with the protocol decoder that matches the
interface. The decoder text is saved as <name>.txt, so the numbers quoted in
docs/captures/analysis.md can be reproduced with `make decode`.

Channel map (diagram.json, wokwi-logic-analyzer):
  D0 SCL(PA9)  D1 SDA(PA10)  D2 SCK(PB3)  D3 MOSI(PB5)  D4 MISO(PB4)
  D5 CS#(PB0)  D6 UART TX(PA2)  D7 PWM1(PA6)
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CAP = os.path.join(ROOT, "docs", "captures")
SESSION = os.path.join(CAP, "session_boot.vcd")
sys.path.insert(0, HERE)
import vcd_tool  # noqa: E402

MS = 1_000_000  # ns per ms

# name: (t0_ms, t1_ms, sigrok decoder arguments)
I2C_ANN = "i2c=start:repeat-start:address-read:address-write:data-read:data-write:ack:nack:stop"
CAPTURES = {
    "i2c_whoami": (8.5, 9.5, ["-P", "i2c:scl=D0:sda=D1", "-A", I2C_ANN]),
    "i2c_init": (15.9, 18.1, ["-P", "i2c:scl=D0:sda=D1", "-A", I2C_ANN]),
    "i2c_burst": (48.5, 50.4, ["-P", "i2c:scl=D0:sda=D1", "-A", I2C_ANN]),
    "spi_jedec": (60.5, 61.3, ["-P", "spi:clk=D2:mosi=D3:miso=D4:cs=D5,spiflash", "-A", "spiflash"]),
    "spi_erase": (66.9, 114.0, ["-P", "spi:clk=D2:mosi=D3:miso=D4:cs=D5,spiflash", "-A", "spiflash"]),
    "spi_program_read": (120.3, 125.0, ["-P", "spi:clk=D2:mosi=D3:miso=D4:cs=D5,spiflash", "-A", "spiflash"]),
    "spi_anomaly_mode3": (175.7, 176.4, ["-P", "spi:clk=D2:mosi=D3:miso=D4:cs=D5,spiflash", "-A", "spiflash"]),
    "spi_anomaly_mode3_cpol1": (175.7, 176.4, ["-P", "spi:clk=D2:mosi=D3:miso=D4:cs=D5:cpol=1:cpha=1", "-A", "spi=mosi-data:miso-data"]),
    "i2c_anomaly_nack": (151.3, 151.7, ["-P", "i2c:scl=D0:sda=D1", "-A", I2C_ANN]),
    "uart_banner": (0.0, 16.1, ["-P", "uart:rx=D6:baudrate=115200:format=ascii", "-A", "uart=rx-data"]),
    "pwm_demo": (150.0, 520.0, ["-P", "pwm:data=D7"]),
}


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=300)


def main(argv):
    names = argv[1:] or list(CAPTURES)
    if not os.path.exists(SESSION):
        print("missing", SESSION)
        return 1
    for name in names:
        t0, t1, args = CAPTURES[name]
        out = os.path.join(CAP, name + ".vcd")
        vcd_tool.cmd_window(SESSION, int(t0 * MS), int(t1 * MS), out)
        res = run(["sigrok-cli", "-i", out, "-I", "vcd"] + args)
        with open(os.path.join(CAP, name + ".txt"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(res.stdout)
            if res.returncode != 0:
                fh.write("\n# sigrok-cli exit code %d\n%s" % (res.returncode, res.stderr))
        lines = len(res.stdout.splitlines())
        print("%-18s %7.1f..%-7.1f ms  %5d annotation lines  rc=%d" % (name, t0, t1, lines, res.returncode))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
