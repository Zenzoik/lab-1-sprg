/**
 * @file w25q64.h
 * @brief Winbond W25Q64 (and compatible) SPI NOR flash driver, platform independent.
 *
 * Command set and timings: Winbond W25Q64JV datasheet, section 8.2 (instruction
 * set) and section 9.6 (AC characteristics): tPP typ. 0.7 ms, tSE typ. 45 ms.
 */
#ifndef DEVICES_W25Q64_H
#define DEVICES_W25Q64_H

#include <stddef.h>
#include <stdint.h>

#include "bus.h"

/* Instructions (W25Q64JV datasheet, Table 8.1.2 "Instruction Set"). */
#define W25Q_CMD_WRITE_ENABLE 0x06u
#define W25Q_CMD_WRITE_DISABLE 0x04u
#define W25Q_CMD_READ_STATUS1 0x05u
#define W25Q_CMD_READ_DATA 0x03u
#define W25Q_CMD_PAGE_PROGRAM 0x02u
#define W25Q_CMD_SECTOR_ERASE_4K 0x20u
#define W25Q_CMD_JEDEC_ID 0x9Fu

#define W25Q_SR1_BUSY 0x01u
#define W25Q_SR1_WEL 0x02u

#define W25Q_PAGE_SIZE 256u
#define W25Q_SECTOR_SIZE 4096u

#define W25Q_JEDEC_MANUFACTURER 0xEFu /* Winbond */
#define W25Q_JEDEC_TYPE 0x40u         /* SPI NOR, 3 V */
#define W25Q_JEDEC_CAPACITY_64MBIT 0x17u

/** Typical/maximum busy times used as polling timeouts [ms]. */
#define W25Q_TIMEOUT_PAGE_PROGRAM_MS 5u    /* tPP max 3 ms  */
#define W25Q_TIMEOUT_SECTOR_ERASE_MS 500u  /* tSE max 400 ms */

typedef enum {
  W25Q_OK = 0,
  W25Q_ERR_BUS,
  W25Q_ERR_WRONG_ID,
  W25Q_ERR_TIMEOUT,
  W25Q_ERR_NOT_WRITE_ENABLED, /**< WEL did not set after Write Enable */
  W25Q_ERR_ARG
} w25q_status_t;

typedef struct {
  const spi_bus_t *bus;
  uint32_t (*millis)(void); /**< time source for busy polling */
} w25q_t;

void w25q_bind(w25q_t *dev, const spi_bus_t *bus, uint32_t (*millis)(void));

/** Read manufacturer / type / capacity (command 9Fh). */
w25q_status_t w25q_read_jedec(const w25q_t *dev, uint8_t id[3]);

/** Read JEDEC ID and verify it is a Winbond 64-Mbit part. */
w25q_status_t w25q_check_id(const w25q_t *dev, uint8_t id_out[3]);

/** Read status register 1 (command 05h). */
w25q_status_t w25q_read_status(const w25q_t *dev, uint8_t *sr1);

/** Poll BUSY until it clears or @p timeout_ms elapses. */
w25q_status_t w25q_wait_ready(const w25q_t *dev, uint32_t timeout_ms);

/** Read @p len bytes starting at the 24-bit address @p addr (command 03h). */
w25q_status_t w25q_read(const w25q_t *dev, uint32_t addr, uint8_t *buf, size_t len);

/** Erase the 4 KiB sector containing @p addr: WREN, 20h, poll BUSY. */
w25q_status_t w25q_sector_erase(const w25q_t *dev, uint32_t addr);

/** Program up to one page (must not cross a 256-byte boundary):
 *  WREN, check WEL, 02h + data, poll BUSY. */
w25q_status_t w25q_page_program(const w25q_t *dev, uint32_t addr, const uint8_t *data, size_t len);

/** Program an arbitrary range, split on page boundaries. The area must be erased. */
w25q_status_t w25q_write(const w25q_t *dev, uint32_t addr, const uint8_t *data, size_t len);

#endif /* DEVICES_W25Q64_H */
