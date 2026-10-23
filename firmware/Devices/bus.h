/**
 * @file bus.h
 * @brief Hardware-independent bus interfaces used by the device drivers.
 *
 * Device drivers (Devices/) talk to a bus ONLY through these structures.
 * The board layer (Platform/) fills them with HAL-backed functions on the
 * target; host tests fill them with fakes/mocks. Nothing in here may include
 * HAL or CMSIS headers.
 */
#ifndef DEVICES_BUS_H
#define DEVICES_BUS_H

#include <stddef.h>
#include <stdint.h>

/** Result of a bus transaction. */
typedef enum {
  BUS_OK = 0,
  BUS_ERR_NACK,    /**< device did not acknowledge */
  BUS_ERR_TIMEOUT, /**< transaction did not finish in time */
  BUS_ERR_IO       /**< any other bus-level failure */
} bus_status_t;

/** I2C master. Addresses are 7-bit; the implementation shifts them. */
typedef struct i2c_bus {
  void *ctx; /**< implementation context (e.g. HAL handle) */
  /** Write @p len bytes, then read @p rlen bytes with a repeated START.
   *  rlen == 0 means write-only, len == 0 means read-only. */
  bus_status_t (*xfer)(void *ctx, uint8_t addr7, const uint8_t *tx, size_t len,
                       uint8_t *rx, size_t rlen);
  /** Probe an address (address phase only). */
  bus_status_t (*probe)(void *ctx, uint8_t addr7);
} i2c_bus_t;

/** SPI master with software-controlled chip select (CS# held across calls). */
typedef struct spi_bus {
  void *ctx;
  /** Full-duplex transfer of @p len bytes; tx and rx may be NULL (then 0xFF
   *  is sent / data are discarded). */
  bus_status_t (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
  /** Drive CS#: 0 = selected (low), 1 = deselected (high). */
  void (*cs)(void *ctx, int level);
} spi_bus_t;

#endif /* DEVICES_BUS_H */
