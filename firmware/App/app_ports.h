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
  void (*marker)(int level);              /**< service GPIO for logic analyzer */

  /* Servo PWM, channel 1 or 2, pulse width in microseconds. */
  void (*pwm_set_us)(unsigned ch, uint16_t us);
  uint16_t (*pwm_get_us)(unsigned ch);

  /* SPI mode switch (0..3), used only by the deliberate-anomaly demo. */
  int (*spi_set_mode)(unsigned mode);     /**< 0 = ok */
} app_ports_t;

#endif /* APP_APP_PORTS_H */
