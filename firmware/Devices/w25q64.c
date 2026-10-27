#include "w25q64.h"

static w25q_status_t from_bus(bus_status_t s) {
  if (s == BUS_OK) {
    return W25Q_OK;
  }
  return (s == BUS_ERR_TIMEOUT) ? W25Q_ERR_TIMEOUT : W25Q_ERR_BUS;
}

/* One command with CS# held low for the whole transaction. */
static w25q_status_t cmd_xfer(const w25q_t *dev, const uint8_t *hdr, size_t hlen, const uint8_t *wr,
                              uint8_t *rd, size_t dlen) {
  const spi_bus_t *b = dev->bus;
  w25q_status_t st = W25Q_OK;
  b->cs(b->ctx, 0);
  bus_status_t s = b->transfer(b->ctx, hdr, NULL, hlen);
  if (s == BUS_OK && dlen > 0u) {
    s = b->transfer(b->ctx, wr, rd, dlen);
  }
  b->cs(b->ctx, 1);
  st = from_bus(s);
  return st;
}

void w25q_bind(w25q_t *dev, const spi_bus_t *bus, uint32_t (*millis)(void)) {
  dev->bus = bus;
  dev->millis = millis;
}

w25q_status_t w25q_read_jedec(const w25q_t *dev, uint8_t id[3]) {
  const uint8_t cmd = W25Q_CMD_JEDEC_ID;
  return cmd_xfer(dev, &cmd, 1, NULL, id, 3);
}

w25q_status_t w25q_check_id(const w25q_t *dev, uint8_t id_out[3]) {
  uint8_t id[3] = {0, 0, 0};
  w25q_status_t st = w25q_read_jedec(dev, id);
  if (st != W25Q_OK) {
    return st;
  }
  if (id_out != NULL) {
    id_out[0] = id[0];
    id_out[1] = id[1];
    id_out[2] = id[2];
  }
  if (id[0] != W25Q_JEDEC_MANUFACTURER || id[1] != W25Q_JEDEC_TYPE || id[2] != W25Q_JEDEC_CAPACITY_64MBIT) {
    return W25Q_ERR_WRONG_ID;
  }
  return W25Q_OK;
}

w25q_status_t w25q_read_status(const w25q_t *dev, uint8_t *sr1) {
  const uint8_t cmd = W25Q_CMD_READ_STATUS1;
  return cmd_xfer(dev, &cmd, 1, NULL, sr1, 1);
}

static w25q_status_t write_enable(const w25q_t *dev) {
  const uint8_t cmd = W25Q_CMD_WRITE_ENABLE;
  return cmd_xfer(dev, &cmd, 1, NULL, NULL, 0);
}

w25q_status_t w25q_wait_ready(const w25q_t *dev, uint32_t timeout_ms) {
  const uint32_t start = dev->millis();
  for (;;) {
    uint8_t sr1 = 0;
    w25q_status_t st = w25q_read_status(dev, &sr1);
    if (st != W25Q_OK) {
      return st;
    }
    if ((sr1 & W25Q_SR1_BUSY) == 0u) {
      return W25Q_OK;
    }
    if ((uint32_t)(dev->millis() - start) > timeout_ms) {
      return W25Q_ERR_TIMEOUT;
    }
  }
}

static void addr24(uint8_t cmd, uint32_t addr, uint8_t hdr[4]) {
  hdr[0] = cmd;
  hdr[1] = (uint8_t)(addr >> 16);
  hdr[2] = (uint8_t)(addr >> 8);
  hdr[3] = (uint8_t)addr;
}

w25q_status_t w25q_read(const w25q_t *dev, uint32_t addr, uint8_t *buf, size_t len) {
  uint8_t hdr[4];
  if (buf == NULL || len == 0u) {
    return W25Q_ERR_ARG;
  }
  addr24(W25Q_CMD_READ_DATA, addr, hdr);
  return cmd_xfer(dev, hdr, sizeof hdr, NULL, buf, len);
}

/* WREN followed by a check that WEL is set (a stuck WP#/protection shows up here). */
static w25q_status_t enable_and_verify(const w25q_t *dev) {
  uint8_t sr1 = 0;
  w25q_status_t st = write_enable(dev);
  if (st != W25Q_OK) {
    return st;
  }
  st = w25q_read_status(dev, &sr1);
  if (st != W25Q_OK) {
    return st;
  }
  return ((sr1 & W25Q_SR1_WEL) != 0u) ? W25Q_OK : W25Q_ERR_NOT_WRITE_ENABLED;
}

w25q_status_t w25q_sector_erase(const w25q_t *dev, uint32_t addr) {
  uint8_t hdr[4];
  w25q_status_t st = enable_and_verify(dev);
  if (st != W25Q_OK) {
    return st;
  }
  addr24(W25Q_CMD_SECTOR_ERASE_4K, addr, hdr);
  st = cmd_xfer(dev, hdr, sizeof hdr, NULL, NULL, 0);
  if (st != W25Q_OK) {
    return st;
  }
  return w25q_wait_ready(dev, W25Q_TIMEOUT_SECTOR_ERASE_MS);
}

w25q_status_t w25q_page_program(const w25q_t *dev, uint32_t addr, const uint8_t *data, size_t len) {
  uint8_t hdr[4];
  if (data == NULL || len == 0u || len > W25Q_PAGE_SIZE ||
      ((addr % W25Q_PAGE_SIZE) + len) > W25Q_PAGE_SIZE) {
    return W25Q_ERR_ARG;
  }
  w25q_status_t st = enable_and_verify(dev);
  if (st != W25Q_OK) {
    return st;
  }
  addr24(W25Q_CMD_PAGE_PROGRAM, addr, hdr);
  st = cmd_xfer(dev, hdr, sizeof hdr, data, NULL, len);
  if (st != W25Q_OK) {
    return st;
  }
  return w25q_wait_ready(dev, W25Q_TIMEOUT_PAGE_PROGRAM_MS);
}

w25q_status_t w25q_write(const w25q_t *dev, uint32_t addr, const uint8_t *data, size_t len) {
  if (data == NULL || len == 0u) {
    return W25Q_ERR_ARG;
  }
  while (len > 0u) {
    size_t room = W25Q_PAGE_SIZE - (addr % W25Q_PAGE_SIZE);
    size_t chunk = (len < room) ? len : room;
    w25q_status_t st = w25q_page_program(dev, addr, data, chunk);
    if (st != W25Q_OK) {
      return st;
    }
    addr += (uint32_t)chunk;
    data += chunk;
    len -= chunk;
  }
  return W25Q_OK;
}
