/**
 * @file fakes.h
 * @brief Host-side models of the bus devices and of the board ports.
 *
 * fake_i2c_t  - MPU-6050 register file behind the i2c_bus_t interface, with error injection.
 * fake_spi_t  - W25Q64 command state machine behind the spi_bus_t interface; records every
 *               command (opcode, address, payload length) so tests can assert the sequence
 *               (e.g. Write Enable before Page Program), and can emulate failures.
 * fake_board_t - the app_ports_t of the application: console capture, virtual time, ADC...
 */
#ifndef TESTS_FAKES_H
#define TESTS_FAKES_H

#include <stddef.h>
#include <stdint.h>

#include "app_ports.h"
#include "bus.h"

/* ------------------------------------------------------------------ I2C */

#define FAKE_I2C_LOG_MAX 64

typedef struct {
  uint8_t is_read;
  uint8_t addr7;
  uint8_t reg;
  size_t len; /* bytes written after the register address, or bytes read */
} fake_i2c_log_t;

typedef struct {
  uint8_t regs[128];
  uint8_t addr7;          /* address the fake device answers to */
  int present;            /* 0 = nobody home: every address NACKs */
  bus_status_t inject;    /* returned by every transfer if != BUS_OK */
  int fail_on_call;       /* >0: only the N-th xfer call fails with inject_once */
  bus_status_t inject_once;
  int calls;
  fake_i2c_log_t log[FAKE_I2C_LOG_MAX];
  int log_n;
  i2c_bus_t bus;
} fake_i2c_t;

/** Initialise as an MPU-6050 at @p addr7 (WHO_AM_I = 0x68, device awake after reset = no). */
void fake_i2c_init(fake_i2c_t *f, uint8_t addr7);
/** Load a 14-byte ACCEL_XOUT_H.. burst into the register file. */
void fake_i2c_set_burst(fake_i2c_t *f, const uint8_t burst[14]);

/* ------------------------------------------------------------------ SPI */

#define FAKE_SPI_LOG_MAX 128
#define FAKE_SPI_MEM 0x4000u /* 16 KiB of NOR is enough for the tests */

typedef struct {
  uint8_t op;
  uint32_t addr;
  size_t len; /* data bytes after opcode (+address) */
  uint8_t wel_at_start;
} fake_spi_log_t;

typedef struct {
  uint8_t mem[FAKE_SPI_MEM];
  uint8_t jedec[3];
  uint8_t wel;
  unsigned busy_polls;       /* RDSR calls that still report BUSY */
  unsigned busy_polls_after_program;
  unsigned busy_polls_after_erase;
  int stuck_busy;            /* BUSY never clears */
  int wp_stuck;              /* WEL never sets (write protect) */
  int wrong_mode;            /* answers 0xFF/0xCF.. like a part probed in the wrong SPI mode */
  bus_status_t inject;       /* transfer() fails with this status if != BUS_OK */
  int cs_low;
  size_t idx;                /* byte index inside the current CS# window */
  uint8_t op;
  uint32_t addr;
  uint32_t start_addr; /* address as received, for the log (addr itself auto-increments) */
  size_t data_n;
  uint8_t page_buf[256];
  size_t page_n;
  uint8_t wel_at_start;
  fake_spi_log_t log[FAKE_SPI_LOG_MAX];
  int log_n;
  int cs_edges;
  spi_bus_t bus;
} fake_spi_t;

void fake_spi_init(fake_spi_t *f);

/* ------------------------------------------------------------------ board ports */

#define FAKE_CONSOLE_MAX 8192

typedef struct {
  char console[FAKE_CONSOLE_MAX];
  size_t console_n;
  const char *rx_script; /* characters returned by read_char() */
  size_t rx_pos;
  uint32_t now_ms;       /* virtual time; every millis() call advances it by tick_ms */
  uint32_t tick_ms;
  uint16_t adc_value;
  int adc_fail;
  int button;
  int marker_level;
  int marker_edges;
  uint16_t pwm[3];
  unsigned spi_mode;
  fake_i2c_t *i2c;
  fake_spi_t *spi;
  app_ports_t ports;
} fake_board_t;

void fake_board_init(fake_board_t *b, fake_i2c_t *i2c, fake_spi_t *spi);
void fake_board_clear_console(fake_board_t *b);

#endif /* TESTS_FAKES_H */
