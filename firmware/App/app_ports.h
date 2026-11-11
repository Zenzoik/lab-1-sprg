/**
 * @file app_ports.h
 * @brief Everything the application layer needs from the board, as an interface.
 *
 * The application (App/) must not know that HAL exists. Platform/ implements
 * this structure with HAL calls; tests/ implements it with fakes. This is the
 * dependency inversion point between the business logic and the hardware.
 */
#ifndef APP_APP_PORTS_H
#define APP_APP_PORTS_H

#include <stddef.h>
#include <stdint.h>

#include "bus.h"

/** One button event. Times are CPU-cycle counter values (see app_ports_t::cycles). */
typedef struct {
  uint32_t t_edge; /**< when the physical edge happened (stimulus generator) or was first seen */
  uint32_t t_isr;  /**< when the interrupt handler ran (equals the poll time in polling mode) */
} btn_event_t;

typedef struct {
  /* Buses (device drivers receive these, the app never calls them directly). */
  const i2c_bus_t *i2c;
  const spi_bus_t *spi;

  /* Console. */
  void (*write)(const char *s, size_t n); /**< UART TX */
  int (*read_char)(void);                 /**< UART RX, -1 if nothing is waiting */

  /* Time. */
  uint32_t (*millis)(void);
  /** Diagnostic only: raw platform status of the last I2C transfer, (hal_status << 16) | hal_error. */
  uint32_t (*i2c_last_error)(void);
  /** Diagnostic only: raw peripheral status register of the I2C controller (ISR). */
  uint32_t (*i2c_status_reg)(void);
  void (*delay_ms)(uint32_t ms);

  /* Analog and discrete I/O. */
  int (*adc_read)(uint16_t *raw12);       /**< 0 = ok; raw in 0..4095 */
  int (*button_pressed)(void);            /**< 1 while pressed */
  void (*led_write)(int on);
  void (*marker)(int level);              /**< service GPIO for logic analyzer (ADC window) */

  /* Servo PWM, channel 1 or 2, pulse width in microseconds. */
  void (*pwm_set_us)(unsigned ch, uint16_t us);
  uint16_t (*pwm_get_us)(unsigned ch);

  /* SPI mode switch (0..3), used only by the deliberate-anomaly demo. */
  int (*spi_set_mode)(unsigned mode);     /**< 0 = ok */

  /* ---- LR3: measurement and stimulus ports -------------------------------------------- */

  /** Free-running CPU-cycle counter (32 bit, wraps; differences of unsigned values are valid
   *  across one wrap). The Cortex-M0+ in the C031 has no DWT, the platform builds it from SysTick. */
  uint32_t (*cycles)(void);
  uint32_t cycles_per_us; /**< counter ticks per microsecond (= core clock in MHz) */
  /** Service GPIO for the logic analyzer: 0 = IMU task window, 1 = long operation, 2 = event reaction. */
  void (*probe)(unsigned ch, int level);
  /** Fetch the next button event, if any (1 = event filled in). */
  int (*button_event)(btn_event_t *ev);
  /** 1 = events by EXTI interrupt, 0 = events by polling the pin from the main loop. */
  void (*btn_mode_set)(int irq);
  /** Stimulus generator that drives the button line: 0 off, 1 rare presses, 2 frequent presses. */
  void (*stim_set)(unsigned mode);
  /** One press of the given length (ms) right now. */
  void (*stim_press)(uint32_t ms);
  /** UART receiver statistics: bytes read and overrun errors. */
  void (*uart_rx_stats)(uint32_t *bytes, uint32_t *overruns);
  /** Memory: measured stack depth, reserved stack size and heap in use [bytes]. */
  uint32_t (*stack_used)(void);
  uint32_t (*stack_size)(void);
  uint32_t (*heap_used)(void);
  void (*reboot)(void);
} app_ports_t;

#endif /* APP_APP_PORTS_H */
