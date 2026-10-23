import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
fig, ax = plt.subplots(figsize=(12, 7.2))
ax.set_xlim(0, 12); ax.set_ylim(0, 7.2); ax.axis("off")
def box(x, y, w, h, title, sub, fc):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.03,rounding_size=0.12", fc=fc, ec="#333", lw=1.2))
    ax.text(x + w/2, y + h - 0.28, title, ha="center", va="top", fontsize=11, fontweight="bold")
    ax.text(x + w/2, y + h - 0.68, sub, ha="center", va="top", fontsize=8.6, linespacing=1.35)
def arrow(a, b, text="", dx=0, dy=0, style="->"):
    ax.annotate("", xy=b, xytext=a, arrowprops=dict(arrowstyle=style, lw=1.4, color="#222"))
    if text:
        ax.text((a[0]+b[0])/2 + dx, (a[1]+b[1])/2 + dy, text, fontsize=8, ha="center", va="center",
                bbox=dict(fc="white", ec="none", pad=1.2))
box(3.6, 5.5, 4.8, 1.45, "Core (CubeMX)", "main.c, stm32c0xx_it.c, MSP\nUSER CODE: platform_init(), app_init(),\napp_boot(), app_poll()", "#e8e8e8")
box(0.3, 2.7, 4.6, 2.0, "App", "app.c: самоперевірка, CLI, цикл 50 Гц,\nтелеметрія 1 Гц\nmotion.c: фільтр, відображення в PWM\napp_ports.h: порти (millis, uart, adc, pwm,\nmarker, i2c, spi)", "#d6e6f5")
box(7.1, 2.7, 4.6, 2.0, "Platform", "platform.c: адаптація HAL до портів\n(I2C, SPI, UART, ADC, TIM3, GPIO)\nboard.h: піни, таймаути, прапори\nреалізує i2c_bus_t, spi_bus_t", "#f7e1c4")
box(0.3, 0.2, 4.1, 1.7, "Devices", "mpu6050.c, w25q64.c\nbus.h: інтерфейси i2c_bus_t, spi_bus_t\n(без HAL і регістрів)", "#d5ecd4")
box(7.6, 0.2, 4.1, 1.7, "Drivers (HAL, CMSIS)", "STM32C0xx_HAL_Driver, CMSIS\nвендорський код, не редагується", "#eadcf0")
box(4.75, 0.2, 2.5, 1.7, "Tests (хост)", "fakes.c: fake I2C/SPI,\nfake ports замість\nPlatform; test_*.c", "#fff3b8")
arrow((5.2, 5.5), (3.3, 4.7), "app_init, app_poll", dx=-0.55)
arrow((6.8, 5.5), (8.7, 4.7), "platform_init", dx=0.45)
arrow((7.1, 3.9), (4.9, 3.9), "app_ports.h", dy=0.22)
arrow((2.6, 2.7), (2.6, 1.9), "bus.h", dx=0.5)
arrow((9.4, 2.7), (9.4, 1.9), "HAL_*", dx=0.5)
arrow((7.9, 2.7), (4.2, 1.9), "bus.h (реалізує i2c_bus_t, spi_bus_t)", dx=0.9, dy=0.15)
arrow((4.75, 1.0), (4.4, 1.0), "")
arrow((6.0, 1.9), (4.6, 2.7), "", style="->")
ax.set_title("Component diagram: шари та напрямок залежностей (стрілка = «підключає / використовує»)", fontsize=11)
fig.tight_layout()
fig.savefig("component_diagram.png", dpi=140)
