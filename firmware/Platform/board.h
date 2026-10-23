/**
 * @file board.h
 * @brief The ONE place that knows which MCU pin and which peripheral handle
 *        serves which function on this board (NUCLEO-C031C6 + our wiring).
 *
 * Keep in sync with docs/pinout.md (the table there is the contract; every
 * line below carries the pinout row it implements). tools/check_pinout.py
 * verifies that both files agree.
 */
#ifndef PLATFORM_BOARD_H
#define PLATFORM_BOARD_H

#include "main.h" /* CubeMX: pulls in the HAL */

/* Peripheral handles are created by CubeMX in Core/Src/main.c. */
extern ADC_HandleTypeDef hadc1;
extern I2C_HandleTypeDef hi2c1;
extern SPI_HandleTypeDef hspi1;
extern TIM_HandleTypeDef htim3;
extern UART_HandleTypeDef huart2;

/* --- GPIO ------------------------------------------------------------- */
/* PINOUT: PB0 | GPIO output | W25Q64 /CS (software CS#), Arduino D10 (CN5) */
#define BOARD_FLASH_CS_PORT GPIOB
#define BOARD_FLASH_CS_PIN GPIO_PIN_0
/* PINOUT: PB1 | GPIO output | service marker for ADC/loop timing, Arduino A3 (CN8) */
#define BOARD_MARKER_PORT GPIOB
#define BOARD_MARKER_PIN GPIO_PIN_1
/* PINOUT: PA5 | GPIO output | user LED LD4, Nucleo D13 */
#define BOARD_LED_PORT GPIOA
#define BOARD_LED_PIN GPIO_PIN_5
/* PINOUT: PC13 | GPIO input pull-up | user button B1 (active low) */
#define BOARD_BUTTON_PORT GPIOC
#define BOARD_BUTTON_PIN GPIO_PIN_13

/* --- Peripherals ------------------------------------------------------ */
/* PINOUT: PA9/PA10 | I2C1 SCL/SDA AF6 | MPU-6050 */
#define BOARD_I2C (&hi2c1)
/* PINOUT: PB3/PB4/PB5 | SPI1 SCK/MISO/MOSI AF0 | W25Q64 */
#define BOARD_SPI (&hspi1)
/* PINOUT: PA6/PA7 | TIM3 CH1/CH2 AF1 | servos */
#define BOARD_PWM_TIM (&htim3)
/* PINOUT: PA2/PA3 | USART2 TX/RX AF1 | ST-LINK VCP / Wokwi serial monitor */
#define BOARD_UART (&huart2)
/* PINOUT: PA0 | ADC1_IN0 analog | potentiometer */
#define BOARD_ADC (&hadc1)

/* --- Bus behaviour and timeouts ---------------------------------------- */
#define BOARD_I2C_TIMEOUT_MS 20u
#define BOARD_SPI_TIMEOUT_MS 20u
#define BOARD_UART_TX_TIMEOUT_MS 100u
#define BOARD_ADC_TIMEOUT_MS 10u

/* Simulator workarounds (Wokwi). Real hardware: define both as 0 and 1 respectively
 * (see docs/lab2-report.md, section "What cannot be verified in the simulator").
 *
 * BOARD_I2C_RESET_BEFORE_XFER = 1: reset the I2C state machine (PE 1 -> 0 -> 1) before every
 *   transfer. Wokwi's STM32C0 I2C model loses the second TXIS of a multi-byte write otherwise.
 * BOARD_I2C_REPEATED_START = 1: register read as START, addr+W, reg, REPEATED START, addr+R
 *   (datasheet burst-read figure). 0: write, STOP, read; the MPU-6050 keeps its register
 *   pointer, and the simulator does not set TC for the repeated START. */
#ifndef BOARD_I2C_RESET_BEFORE_XFER
#define BOARD_I2C_RESET_BEFORE_XFER 1
#endif
#ifndef BOARD_I2C_REPEATED_START
#define BOARD_I2C_REPEATED_START 0
#endif

#endif /* PLATFORM_BOARD_H */
