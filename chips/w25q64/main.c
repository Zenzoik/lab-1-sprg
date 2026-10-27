// SPDX-License-Identifier: MIT
//
// W25Q64-style SPI NOR flash model for the Wokwi simulator (custom chip).
// Wokwi has no SPI flash part, and LR2 needs one (JEDEC ID, write enable,
// page program, sector erase, busy polling), so this chip implements the
// subset of the Winbond W25Q64 command set used by the firmware.
//
// Modelled:   9Fh JEDEC ID, 90h manufacturer/device ID, ABh release/ID,
//             05h/35h read status, 06h/04h write enable/disable,
//             03h read, 0Bh fast read, 02h page program, 20h sector erase,
//             C7h/60h chip erase, BUSY (WIP) bit with realistic durations.
// Simplified: capacity is FLASH_BYTES (addresses wrap), SPI mode 0 only,
//             single-bit SPI only (WP#/HOLD# pins exist but are ignored),
//             DO is driven 0xFF while the chip is not answering (the real
//             pin is Hi-Z).
//
// No libc: the chip is built freestanding, state is static.

#include "wokwi-api.h"

#define FLASH_BYTES 65536u /* modelled capacity (real W25Q64: 8 MiB) */
#define SECTOR_BYTES 4096u
#define PAGE_BYTES 256u

#define SR1_BUSY 0x01u
#define SR1_WEL 0x02u

#define T_PAGE_PROGRAM_US 700u  /* tPP typ.  0.7 ms */
#define T_SECTOR_ERASE_US 45000u /* tSE typ.  45 ms  */
#define T_CHIP_ERASE_US 400000u /* shortened (real: ~20 s) */

enum phase { PH_CMD, PH_ADDR, PH_DUMMY, PH_DATA, PH_IDLE };

typedef struct {
  pin_t cs;
  spi_dev_t spi;
  timer_t busy_timer;
  uint8_t buf[1];

  uint8_t cmd;
  uint8_t phase;
  uint8_t addr_cnt;
  uint32_t addr;
  uint32_t idx;
  uint8_t sr1;

  uint8_t pp_buf[PAGE_BYTES];
  uint8_t pp_set[PAGE_BYTES];
  uint32_t pp_base;
  uint32_t pp_off;
  uint32_t pp_n;

  uint8_t mem[FLASH_BYTES];
} chip_t;

static chip_t g;

static void fill(uint8_t *p, uint8_t v, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    p[i] = v;
  }
}

static void busy_done(void *user_data) {
  (void)user_data;
  g.sr1 = (uint8_t)(g.sr1 & ~SR1_BUSY);
}

static void start_busy(uint32_t micros) {
  g.sr1 = (uint8_t)((g.sr1 | SR1_BUSY) & ~SR1_WEL);
  timer_start(g.busy_timer, micros, false);
}

/* Byte the chip drives during the next transfer (data phase). */
static uint8_t next_out(void) {
  static const uint8_t jedec[3] = {0xEFu, 0x40u, 0x17u};
  switch (g.cmd) {
  case 0x9F:
    return jedec[g.idx++ % 3u];
  case 0x05:
    return g.sr1;
  case 0x35:
    return 0x00u;
  case 0x90:
    return (g.idx++ % 2u) ? 0x16u : 0xEFu;
  case 0xAB:
    return 0x16u;
  case 0x03:
  case 0x0B:
    return g.mem[(g.addr++) % FLASH_BYTES];
  default:
    return 0xFFu;
  }
}

/* A byte was received from the master; decide what to drive next. */
static void feed(uint8_t b) {
  switch (g.phase) {
  case PH_CMD:
    g.cmd = b;
    g.addr = 0;
    g.addr_cnt = 0;
    g.idx = 0;
    g.buf[0] = 0xFFu;
    switch (b) {
    case 0x9F:
    case 0x05:
    case 0x35:
      g.phase = PH_DATA;
      g.buf[0] = next_out();
      break;
    case 0x03:
    case 0x0B:
    case 0x02:
    case 0x20:
    case 0x90:
    case 0xAB:
      g.phase = PH_ADDR;
      break;
    default: /* 06h, 04h, C7h, 60h and unknown: act on CS rising edge */
      g.phase = PH_IDLE;
      break;
    }
    break;

  case PH_ADDR:
    g.addr = (g.addr << 8) | b;
    g.addr_cnt++;
    g.buf[0] = 0xFFu;
    if (g.addr_cnt == 3u) {
      g.addr %= FLASH_BYTES;
      switch (g.cmd) {
      case 0x03:
      case 0x90:
      case 0xAB:
        g.phase = PH_DATA;
        g.buf[0] = next_out();
        break;
      case 0x0B:
        g.phase = PH_DUMMY;
        break;
      case 0x02:
        g.phase = PH_DATA;
        g.pp_base = g.addr & ~(PAGE_BYTES - 1u);
        g.pp_off = g.addr & (PAGE_BYTES - 1u);
        g.pp_n = 0;
        fill(g.pp_set, 0, PAGE_BYTES);
        break;
      default: /* 20h sector erase */
        g.phase = PH_IDLE;
        break;
      }
    }
    break;

  case PH_DUMMY:
    g.phase = PH_DATA;
    g.buf[0] = next_out();
    break;

  case PH_DATA:
    if (g.cmd == 0x02) {
      uint32_t i = (g.pp_off + g.pp_n) & (PAGE_BYTES - 1u);
      g.pp_buf[i] = b;
      g.pp_set[i] = 1u;
      g.pp_n++;
      g.buf[0] = 0xFFu;
    } else {
      g.buf[0] = next_out();
    }
    break;

  default:
    g.buf[0] = 0xFFu;
    break;
  }
}

/* CS# went high: the command is complete, execute it. */
static void finish_command(void) {
  switch (g.cmd) {
  case 0x06:
    if (g.phase == PH_IDLE) {
      g.sr1 |= SR1_WEL;
    }
    break;
  case 0x04:
    if (g.phase == PH_IDLE) {
      g.sr1 = (uint8_t)(g.sr1 & ~SR1_WEL);
    }
    break;
  case 0x02:
    if ((g.sr1 & SR1_WEL) && !(g.sr1 & SR1_BUSY) && g.pp_n > 0u) {
      for (uint32_t i = 0; i < PAGE_BYTES; i++) {
        if (g.pp_set[i]) {
          g.mem[g.pp_base + i] &= g.pp_buf[i]; /* NOR: programming only clears bits */
        }
      }
      start_busy(T_PAGE_PROGRAM_US);
    }
    break;
  case 0x20:
    if (g.phase == PH_IDLE && (g.sr1 & SR1_WEL) && !(g.sr1 & SR1_BUSY)) {
      fill(&g.mem[g.addr & ~(SECTOR_BYTES - 1u)], 0xFFu, SECTOR_BYTES);
      start_busy(T_SECTOR_ERASE_US);
    }
    break;
  case 0xC7:
  case 0x60:
    if (g.phase == PH_IDLE && (g.sr1 & SR1_WEL) && !(g.sr1 & SR1_BUSY)) {
      fill(g.mem, 0xFFu, FLASH_BYTES);
      start_busy(T_CHIP_ERASE_US);
    }
    break;
  default:
    break;
  }
  g.cmd = 0;
  g.phase = PH_CMD;
}

static void chip_spi_done(void *user_data, uint8_t *buffer, uint32_t count) {
  (void)user_data;
  (void)buffer;
  if (count == 0u) {
    return; /* spi_stop() after CS# rose */
  }
  feed(g.buf[0]);
  if (pin_read(g.cs) == LOW) {
    spi_start(g.spi, g.buf, 1);
  }
}

static void chip_pin_change(void *user_data, pin_t pin, uint32_t value) {
  (void)user_data;
  if (pin != g.cs) {
    return;
  }
  if (value == LOW) {
    g.phase = PH_CMD;
    g.cmd = 0;
    g.buf[0] = 0xFFu;
    spi_start(g.spi, g.buf, 1);
  } else {
    spi_stop(g.spi);
    finish_command();
  }
}

void chip_init(void) {
  fill(g.mem, 0xFFu, FLASH_BYTES);
  g.sr1 = 0;
  g.phase = PH_CMD;

  g.cs = pin_init("CS", INPUT_PULLUP);

  const timer_config_t timer_cfg = {
      .user_data = &g,
      .callback = busy_done,
  };
  g.busy_timer = timer_init(&timer_cfg);

  const spi_config_t spi_cfg = {
      .sck = pin_init("CLK", INPUT),
      .mosi = pin_init("DI", INPUT),
      .miso = pin_init("DO", INPUT),
      .mode = 0,
      .done = chip_spi_done,
      .user_data = &g,
  };
  g.spi = spi_init(&spi_cfg);

  const pin_watch_config_t watch_cfg = {
      .user_data = &g,
      .edge = BOTH,
      .pin_change = chip_pin_change,
  };
  pin_watch(g.cs, &watch_cfg);
}
