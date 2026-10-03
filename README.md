# sp-lab — Системне програмування, ЛР1

Bring-up STM32 (NUCLEO-C031C6) у симуляторі Wokwi: Docker-тулчейн, STM32CubeMX + HAL, UART / I2C / ADC / PWM / GPIO.

Проєкт: Motion-Mirroring Teleoperator — див. [docs/PRD.md](docs/PRD.md).

## Структура

```
docker/Dockerfile            тулчейн-образ (ubuntu 22.04 + arm-none-eabi-gcc)
firmware/                    проєкт STM32CubeMX (firmware.ioc, Core/, Drivers/, Makefile)
docs/PRD.md                  вимоги до продукту
docs/hardware-components.md  компоненти, піни, регістри
docs/screenshots/            скріншоти для звіту
Makefile                     обгортка над Docker
wokwi.toml, diagram.json     конфігурація і схема Wokwi
```

## Збірка і запуск

Потрібні Docker і make (на Windows — у WSL).

```
make image     # один раз: зібрати образ stm32-build
make build     # firmware/build/firmware.elf, .hex, .bin
make clean
```

Запуск: відкрити теку у VS Code з розширенням Wokwi → F1 → `Wokwi: Start Simulator`. Вивід USART2 (115200 8N1) — у терміналі Wokwi.

Очікуваний вивід:

```
hello from STM32
MPU6050: OK
tick 1005 adc=0 btn=0
```
