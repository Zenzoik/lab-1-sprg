#include "fakes.h"

#include <string.h>

/* ================================================================= I2C fake */

static bus_status_t fi2c_xfer(void *ctx, uint8_t addr7, const uint8_t *tx, size_t len, uint8_t *rx,
                              size_t rlen) {
  fake_i2c_t *f = (fake_i2c_t *)ctx;
  f->calls++;
  if (!f->present || addr7 != f->addr7) {
    return BUS_ERR_NACK;
  }
  if (f->fail_on_call > 0 && f->calls == f->fail_on_call) {
    return f->inject_once;
  }
  if (f->inject != BUS_OK) {
    return f->inject;
  }
  if (len == 0u) {
    return BUS_ERR_IO;
  }
  const uint8_t reg = tx[0];
  if (f->log_n < FAKE_I2C_LOG_MAX) {
    fake_i2c_log_t *l = &f->log[f->log_n++];
    l->is_read = (rlen > 0u);
    l->addr7 = addr7;
    l->reg = reg;
    l->len = (rlen > 0u) ? rlen : (len - 1u);
  }
  if (rlen > 0u) {
    for (size_t i = 0; i < rlen; i++) {
      rx[i] = f->regs[(reg + i) & 0x7Fu];
    }
  } else {
    for (size_t i = 1; i < len; i++) {
      f->regs[(reg + i - 1u) & 0x7Fu] = tx[i];
    }
  }
  return BUS_OK;
}

static bus_status_t fi2c_probe(void *ctx, uint8_t addr7) {
  fake_i2c_t *f = (fake_i2c_t *)ctx;
  f->calls++;
  return (f->present && addr7 == f->addr7) ? BUS_OK : BUS_ERR_NACK;
}

void fake_i2c_init(fake_i2c_t *f, uint8_t addr7) {
  memset(f, 0, sizeof *f);
  f->addr7 = addr7;
  f->present = 1;
  f->regs[0x75] = 0x68; /* WHO_AM_I */
  f->regs[0x6B] = 0x40; /* PWR_MGMT_1 reset value: SLEEP = 1 */
  f->bus.ctx = f;
  f->bus.xfer = fi2c_xfer;
  f->bus.probe = fi2c_probe;
}

void fake_i2c_set_burst(fake_i2c_t *f, const uint8_t burst[14]) {
  memcpy(&f->regs[0x3B], burst, 14);
}

/* ================================================================= SPI fake */

static void fspi_log(fake_spi_t *f) {
  if (f->log_n < FAKE_SPI_LOG_MAX) {
    fake_spi_log_t *l = &f->log[f->log_n++];
    l->op = f->op;
    l->addr = f->start_addr;
    l->len = f->data_n;
    l->wel_at_start = f->wel_at_start;
  }
}

static void fspi_commit(fake_spi_t *f) {
  if (f->idx == 0u) {
    return;
  }
  fspi_log(f);
  switch (f->op) {
    case 0x06: /* WREN */
      f->wel = f->wp_stuck ? 0u : 1u;
      break;
    case 0x04:
      f->wel = 0u;
      break;
    case 0x02: /* page program: bits can only go 1 -> 0, wraps inside the page */
      if (f->wel) {
        const uint32_t page = f->addr & ~0xFFu;
        for (size_t i = 0; i < f->page_n; i++) {
          const uint32_t a = page + ((f->addr + (uint32_t)i) & 0xFFu);
          if (a < FAKE_SPI_MEM) {
            f->mem[a] &= f->page_buf[i];
          }
        }
        f->wel = 0u;
        f->busy_polls = f->busy_polls_after_program;
      }
      break;
    case 0x20: /* sector erase 4 KiB */
      if (f->wel) {
        const uint32_t base = f->addr & ~0xFFFu;
        for (uint32_t i = 0; i < 4096u && (base + i) < FAKE_SPI_MEM; i++) {
          f->mem[base + i] = 0xFFu;
        }
        f->wel = 0u;
        f->busy_polls = f->busy_polls_after_erase;
      }
      break;
    default:
      break;
  }
}

static void fspi_cs(void *ctx, int level) {
  fake_spi_t *f = (fake_spi_t *)ctx;
  f->cs_edges++;
  if (level == 0) {
    f->cs_low = 1;
    f->idx = 0;
    f->op = 0;
    f->addr = 0;
    f->data_n = 0;
    f->page_n = 0;
    f->wel_at_start = f->wel;
  } else {
    if (f->cs_low) {
      fspi_commit(f);
    }
    f->cs_low = 0;
  }
}

static uint8_t fspi_byte(fake_spi_t *f, uint8_t mosi) {
  uint8_t miso = 0xFFu;
  if (f->idx == 0u) {
    f->op = mosi;
    f->idx++;
    return 0xFFu;
  }
  const size_t k = f->idx - 1u; /* index after the opcode */
  switch (f->op) {
    case 0x9F:
      miso = (k < 3u) ? f->jedec[k] : 0xFFu;
      f->data_n++;
      break;
    case 0x05: {
      uint8_t sr = 0;
      if (f->stuck_busy || f->busy_polls > 0u) {
        sr |= 0x01u;
        if (!f->stuck_busy) {
          f->busy_polls--;
        }
      }
      if (f->wel) {
        sr |= 0x02u;
      }
      miso = sr;
      f->data_n++;
      break;
    }
    case 0x03:
      if (k < 3u) {
        f->addr = (f->addr << 8) | mosi;
        f->start_addr = f->addr;
      } else {
        miso = (f->addr < FAKE_SPI_MEM) ? f->mem[f->addr] : 0xFFu;
        f->addr++;
        f->data_n++;
      }
      break;
    case 0x02:
    case 0x20:
      if (k < 3u) {
        f->addr = (f->addr << 8) | mosi;
        f->start_addr = f->addr;
      } else if (f->page_n < sizeof f->page_buf) {
        f->page_buf[f->page_n++] = mosi;
        f->data_n++;
      }
      break;
    default:
      break;
  }
  f->idx++;
  return miso;
}

static bus_status_t fspi_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
  fake_spi_t *f = (fake_spi_t *)ctx;
  if (f->inject != BUS_OK) {
    return f->inject;
  }
  for (size_t i = 0; i < len; i++) {
    const uint8_t mosi = (tx != NULL) ? tx[i] : 0xFFu;
    uint8_t miso;
    if (f->wrong_mode) {
      /* A mode-0 part clocked in mode 3 is sampled half a bit late: shifted garbage. */
      static const uint8_t k_garbage[4] = {0xFF, 0xCF, 0xFF, 0xFF};
      miso = k_garbage[(f->idx + i) & 3u];
      (void)fspi_byte(f, mosi);
    } else {
      miso = fspi_byte(f, mosi);
    }
    if (rx != NULL) {
      rx[i] = miso;
    }
  }
  return BUS_OK;
}

void fake_spi_init(fake_spi_t *f) {
  memset(f, 0, sizeof *f);
  memset(f->mem, 0xFF, sizeof f->mem);
  f->jedec[0] = 0xEF;
  f->jedec[1] = 0x40;
  f->jedec[2] = 0x17;
  f->busy_polls_after_program = 2;
  f->busy_polls_after_erase = 5;
  f->bus.ctx = f;
  f->bus.transfer = fspi_transfer;
  f->bus.cs = fspi_cs;
}

/* ================================================================= board fake */


/* The ports table has no ctx pointer (a real board has exactly one instance), so the
 * fake board is a file-scope singleton behind the function pointers. */
static fake_board_t *g_b;

static void fb_write_impl(const char *s, size_t n) {
  for (size_t i = 0; i < n && g_b->console_n + 1u < FAKE_CONSOLE_MAX; i++) {
    g_b->console[g_b->console_n++] = s[i];
  }
  g_b->console[g_b->console_n] = '\0';
}

static int fb_read_char(void) {
  if (g_b->rx_script != NULL && g_b->rx_script[g_b->rx_pos] != '\0') {
    return (unsigned char)g_b->rx_script[g_b->rx_pos++];
  }
  return -1;
}

static uint32_t fb_millis(void) {
  const uint32_t t = g_b->now_ms;
  g_b->now_ms += g_b->tick_ms;
  return t;
}

static void fb_delay(uint32_t ms) {
  g_b->now_ms += ms;
}

static int fb_adc(uint16_t *raw) {
  if (g_b->adc_fail) {
    return -1;
  }
  *raw = g_b->adc_value;
  return 0;
}

static int fb_button(void) {
  return g_b->button;
}

static void fb_led(int on) {
  (void)on;
}

static void fb_marker(int level) {
  if (level != g_b->marker_level) {
    g_b->marker_edges++;
  }
  g_b->marker_level = level;
}

static void fb_pwm_set(unsigned ch, uint16_t us) {
  if (ch >= 1u && ch <= 2u) {
    g_b->pwm[ch] = us;
  }
}

static uint16_t fb_pwm_get(unsigned ch) {
  return (ch >= 1u && ch <= 2u) ? g_b->pwm[ch] : 0u;
}

static int fb_spi_mode(unsigned mode) {
  g_b->spi_mode = mode;
  g_b->spi->wrong_mode = (mode != 0u);
  return 0;
}

void fake_board_init(fake_board_t *b, fake_i2c_t *i2c, fake_spi_t *spi) {
  memset(b, 0, sizeof *b);
  g_b = b;
  b->i2c = i2c;
  b->spi = spi;
  b->tick_ms = 1;
  b->adc_value = 2048;
  b->ports.i2c = &i2c->bus;
  b->ports.spi = &spi->bus;
  b->ports.write = fb_write_impl;
  b->ports.read_char = fb_read_char;
  b->ports.millis = fb_millis;
  b->ports.delay_ms = fb_delay;
  b->ports.adc_read = fb_adc;
  b->ports.button_pressed = fb_button;
  b->ports.led_write = fb_led;
  b->ports.marker = fb_marker;
  b->ports.pwm_set_us = fb_pwm_set;
  b->ports.pwm_get_us = fb_pwm_get;
  b->ports.spi_set_mode = fb_spi_mode;
}

void fake_board_clear_console(fake_board_t *b) {
  b->console_n = 0;
  b->console[0] = '\0';
}
