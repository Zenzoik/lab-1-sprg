# Архітектура програмного проєкту – ЛР2

## 1. Шари

```
firmware/
  Core/         згенерований CubeMX код: main.c, переривання, HAL MSP, конфіг HAL
  Drivers/      вендорський код: STM32C0xx_HAL_Driver, CMSIS (не редагується)
  Middlewares/  сторонні бібліотеки (зараз порожньо, див. нижче)
  Platform/     адаптація HAL до портів застосунку: platform.c, board.h (піни, таймаути)
  Devices/      драйвери пристроїв: mpu6050.c, w25q64.c і інтерфейси шин bus.h
  App/          бізнес-логіка: motion.c (фільтр, відображення у PWM), app.c (самоперевірка, CLI, цикл керування), app_ports.h
```

Залежності спрямовані тільки вниз:

```
Core ─────────► App ─────► Devices ◄───── Platform
  │              ▲  (app_ports.h)   (bus.h)    │
  └──► Platform ─┘                             │
                 └──────────► HAL (Drivers) ◄──┘
```

| Шар | Може підключати | Не може |
|---|---|---|
| Core | App, Platform, Devices (bus.h), HAL | – |
| App | Devices | HAL, регістри STM32, Platform, Core |
| Devices | Devices (інтерфейси `i2c_bus_t`, `spi_bus_t` у `bus.h`) | HAL, регістри, App |
| Platform | Devices (bus.h), App (app_ports.h), Core (main.h – мітки пінів), HAL | – |

**Інверсія залежностей.** Драйвери (`Devices/`) працюють через структури-інтерфейси `i2c_bus_t` і `spi_bus_t` (`bus.h`): вказівники на функції `write`, `read`, `probe`, `xfer`, `cs`. Застосунок (`App/`) отримує від Platform таблицю портів `app_ports_t` (`millis`, `uart_write`, `adc_read`, `pwm_set`, `marker`, `i2c`, `spi`, діагностика). У прошивці ці функції реалізує `Platform/platform.c` поверх HAL, у host-тестах – підроблені шини `tests/fakes.c`. Тому `Devices/` і `App/` компілюються і тестуються на хості без жодного заголовка STM32.

Правила перевіряє `tools/check_deps.py` (`make check-deps`): розбирає `#include` у кожному шарі та шукає символи `HAL_`, `LL_` і імена регістрів у `App/` і `Devices/`. Прапор `--selftest` (`make check-deps-selftest`) створює у тимчасовій теці файл із навмисним порушенням і перевіряє, що перевірка завершується помилкою, тобто що перевірка справді працює. Цей же набір входить у CI.

### Чому `Middlewares/` порожній

У проєкті немає RTOS, файлової системи, USB-стеку чи іншого стороннього компонента. Фільтр руху і CLI написані власноруч у `App/` (тестуються на хості). Порожній шар залишено в структурі, щоб підключення сторонніх бібліотек (наприклад, FreeRTOS або FatFS для flash) не змінювало схему, а файл `firmware/Middlewares/README.md` це пояснює.

## 2. Діаграма компонентів

```
 +---------------------------------------------------------------+
 |  Core/main.c   (CubeMX: тактування, GPIO, I2C1, SPI1, TIM3,    |
 |                 USART2, ADC1;  USER CODE: platform_init(),     |
 |                 app_init(), app_boot(), app_poll())            |
 +--------------------+------------------------------+-----------+
                      | app_poll()                   | platform_init()
                      v                              v
 +------------------------------+   app_ports_t   +-------------------------------+
 |  App/app.c  (self-test, CLI, |<----------------|  Platform/platform.c          |
 |  50 Hz control loop, TLM)    |                 |  HAL: I2C (регістровий poll), |
 |  App/motion.c (filter, map)  |                 |  SPI, UART, ADC, TIM3, GPIO   |
 +--------------+---------------+                 |  board.h (піни, таймаути)     |
                | i2c_bus_t / spi_bus_t           +--------------+----------------+
                v                                                |
 +------------------------------+      реалізує i2c_bus_t, spi_bus_t
 |  Devices/mpu6050.c           |<--------------------------------+
 |  Devices/w25q64.c            |
 |  Devices/bus.h               |
 +------------------------------+
        ^ ті самі файли у host-тестах (tests/): fakes.c підставляє шини
```

## 3. Увімкнені модулі HAL і обґрунтування

Список – `HAL_*_MODULE_ENABLED` у `firmware/Core/Inc/stm32c0xx_hal_conf.h` (генерується CubeMX).

| Модуль HAL | Для чого | Де використовується |
|---|---|---|
| `HAL_GPIO` | піни: CS (PB0), маркер (PB1), LED (PA5), кнопка (PC13), конфігурація AF | `Core/main.c`, `Platform/platform.c` |
| `HAL_RCC` | HSI 48 МГц, дільники шин, увімкнення тактування периферії | `Core/main.c` (`SystemClock_Config`) |
| `HAL_CORTEX` | SysTick (1 мс), NVIC | `HAL_Init`, `stm32c0xx_it.c` |
| `HAL_I2C` | I2C1, MPU-6050; `HAL_I2C_IsDeviceReady` для probe | `Platform/platform.c` |
| `HAL_SPI` | SPI1, W25Q64 | `Platform/platform.c` |
| `HAL_TIM` | TIM3: два канали PWM (50 Гц) | `Platform/platform.c` |
| `HAL_UART` | USART2 на ST-LINK VCP: протокол самоперевірки, CLI, TLM | `Platform/platform.c` |
| `HAL_ADC` | ADC1 IN0: потенціометр (коефіцієнт підсилення) | `Platform/platform.c` |
| `HAL_DMA`, `HAL_EXTI`, `HAL_FLASH`, `HAL_PWR` | увімкнені CubeMX за замовчуванням: хендли I2C/SPI/UART/ADC містять вказівники на DMA, `HAL_GPIO` і `HAL_RCC` посилаються на EXTI, FLASH і PWR. У коді застосунку не викликаються (DMA не потрібен: обсяги передачі малі, а Wokwi не емулює DMA) | залежності вендорського HAL |

Не увімкнено: `HAL_COMP`, `HAL_CRC`, `HAL_IWDG`, `HAL_RTC`, `HAL_WWDG`, `HAL_SMBUS`, `HAL_IRDA`, `HAL_SMARTCARD`, `HAL_USART` – вони не потрібні (немає апаратного сторожового таймера, бо Wokwi його не емулює; IWDG слід додати на реальному обладнанні).

## 4. Версії бібліотек і інструментів

| Компонент | Версія | Де зафіксовано |
|---|---|---|
| STM32CubeMX | 6.18.1 | `firmware/firmware.ioc` |
| STM32Cube FW_C0 (HAL, CMSIS) | V1.4.1 | `ProjectManager.FirmwarePackage` у `firmware.ioc`, код у `firmware/Drivers/` у репозиторії |
| arm-none-eabi-gcc | 10.3.1 20210621 (Ubuntu `gcc-arm-none-eabi 15:10.3-2021.07-4`) | `docker/Dockerfile`, образ на базі `ubuntu:22.04` |
| gcc для host-тестів | системний gcc образу | `docker/Dockerfile` |
| sigrok-cli | 0.7.2 | образ (пакет Ubuntu) |
| gcovr / lcov | пакети Ubuntu образу | `docker/Dockerfile` |
| Wokwi для VS Code | остання на момент виконання; симуляція – `wokwi.toml` | – |
| Wokwi custom chip W25Q64 | `chips/w25q64/` (wasm збирається `make chips`) | `chips/w25q64/Makefile` |

Збірку відтворює тільки Docker-образ: `make image && make build`. Сторонніх бібліотек поза HAL/CMSIS у проєкті немає, тому окремого `lock`-файлу не потрібно.

## 5. Збірка

`make build` запускає `make -C firmware -f Makefile.app all` у контейнері. `Makefile.app` підключає згенерований CubeMX `Makefile` і додає власні шари:

- `APP_SOURCES` – `Platform/platform.c`, `Devices/*.c`, `App/*.c`;
- для власних об'єктів `CFLAGS += -Wextra -Wshadow -Wpedantic -Werror` на додачу до `-Wall` з CubeMX: код шарів Platform/Devices/App збирається без жодного попередження;
- вендорський код підключається через `-isystem`, тому його попередження не показуються;
- **автоматична перевірка залежностей**: `make check-deps` і `make check` входять у CI та `make all-checks`.

Розмір: `.text` ≈ 26.6 КБ із 32 КБ flash (SPI HAL + CLI + самоперевірка), RAM у межах 12 КБ (виводить `arm-none-eabi-size` наприкінці збірки).

## 6. Тести

`make test` (у контейнері, `tests/Makefile`) збирає `Devices/` і `App/` разом із підробленими шинами та запускає тести без плати: 69 перевірок у чотирьох наборах – `test_motion.c` (10), `test_mpu6050.c` (16), `test_w25q64.c` (18), `test_app.c` (25). `make coverage` додає gcov/gcovr; поріг – 90 % рядків і 70 % гілок (аргументація у `docs/lab2-report.md`).

## 7. CI

`.github/workflows/ci.yml`: на кожен push і pull request збирається Docker-образ, виконуються `make build`, `make test`, `make check`, `make coverage` (з порогом), артефакти (`firmware.elf`, звіт покриття) завантажуються у GitHub Actions.
