# sp-lab – Системне програмування, ЛР1–ЛР2

Motion-Mirroring Teleoperator на NUCLEO-C031C6 у симуляторі Wokwi (форма B): UART, I2C (MPU-6050), SPI (W25Q64), ADC, PWM, GPIO. Проєкт – див. [docs/PRD.md](docs/PRD.md).

ЛР2: інтеграція сенсорної підсистеми – схема підключення, верифікація інтерфейсів логічним аналізатором, шарова архітектура, host-тести і CI.

## Структура

```
docker/Dockerfile              тулчейн-образ (ubuntu 22.04: arm-none-eabi-gcc, gcc, gcovr, sigrok-cli, python3)
firmware/Core, Drivers         CubeMX-проєкт і вендорський HAL/CMSIS
firmware/Platform              HAL-адаптація, board.h (піни, таймаути)
firmware/Devices               драйвери MPU-6050, W25Q64, bus.h
firmware/App                   логіка: motion, самоперевірка, CLI
firmware/Middlewares           порожньо (див. README у теці)
tests/                         host-тести (fake-шини), coverage
tools/                         check_deps, check_pinout, decode_captures, measure, plot_captures, vcd_tool
chips/w25q64/                  Wokwi custom chip: модель SPI flash
docs/PRD.md                    вимоги до продукту
docs/hardware-components.md    компоненти, регістри
docs/pinout.md                 розпіновка, схема, бюджет живлення, шини
docs/architecture.md           шари, діаграма, модулі HAL, версії
docs/captures/                 VCD-записи, декодування sigrok, analysis.md, measurements.txt
docs/lab2-report.md            звіт ЛР2 (docs/LR2_report.docx / .pdf)
docs/lab_2/img/                рисунки до звіту
Makefile                       обгортка над Docker
.github/workflows/ci.yml       CI: build + test + check + coverage
wokwi.toml, diagram.json       конфігурація і схема Wokwi
```

## Збірка і запуск

Потрібні Docker і make (на Windows – у WSL).

```
make image          # один раз: образ stm32-build
make build          # firmware/build/firmware.elf (-Wall -Wextra -Werror для власного коду)
make test           # host-тести (69 перевірок), без плати
make coverage       # gcov/gcovr; поріг 90 % рядків, 70 % гілок
make check          # залежності між шарами, самоперевірка перевірки, узгодженість pinout
make decode         # sigrok-cli: декодування docs/captures/*.vcd
make measure        # числа для docs/captures/analysis.md
make all-checks     # build + test + check
```

Запуск симуляції: VS Code з розширенням Wokwi → F1 → `Wokwi: Start Simulator`. UART (115200 8N1) – у терміналі Wokwi. Після завантаження доступні команди CLI (`help`, `selftest`, `adc`, `reg`, `regw`, …).

Очікуваний вивід самоперевірки (фрагмент):

```
hello from STM32 (lr2)
SELFTEST BEGIN version=lr2 t_ms=4
DEV mpu6050_id status=OK addr=0x68 who=0x68 expected=0x68
...
SELFTEST RESULT pass=8 fail=0
```

## Обхід особливостей Wokwi

Модель I2C у Wokwi не повністю відповідає HAL C0: втрачається другий байт багатобайтного запису, а повторний START не підтримується. Тому `Platform/platform.c` використовує регістровий poll-драйвер і скидає I2C перед кожною транзакцією. Прапорці у `Platform/board.h`: `BOARD_I2C_RESET_BEFORE_XFER` (1 у симуляторі, 0 на залізі) і `BOARD_I2C_REPEATED_START` (0 у симуляторі, 1 на залізі).
