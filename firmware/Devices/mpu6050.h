/**
 * @file mpu6050.h
 * @brief MPU-6050 6-axis IMU driver (I2C), platform independent.
 *
 * Register map: InvenSense "MPU-6000/MPU-6050 Register Map and Descriptions",
 * Rev 4.2 (RM-MPU-6000A-00). Product specification PS-MPU-6000A-00.
 */
#ifndef DEVICES_MPU6050_H
#define DEVICES_MPU6050_H

#include <stdint.h>

#include "bus.h"

#define MPU6050_ADDR_AD0_LOW 0x68u  /* AD0 = GND (our wiring) */
#define MPU6050_ADDR_AD0_HIGH 0x69u /* AD0 = VCC */

/* Register addresses used by the driver (Register Map, section 3). */
#define MPU6050_REG_SMPLRT_DIV 0x19u
#define MPU6050_REG_CONFIG 0x1Au
#define MPU6050_REG_GYRO_CONFIG 0x1Bu
#define MPU6050_REG_ACCEL_CONFIG 0x1Cu
#define MPU6050_REG_ACCEL_XOUT_H 0x3Bu /* 14 bytes: accel(6) temp(2) gyro(6) */
#define MPU6050_REG_PWR_MGMT_1 0x6Bu
#define MPU6050_REG_WHO_AM_I 0x75u

/** Value of WHO_AM_I: bits 6:1 of the 7-bit I2C address (0x68), register map
 *  section 4.32 -> 0x68 regardless of the AD0 pin. */
#define MPU6050_WHO_AM_I_VALUE 0x68u

/** Full-scale ranges. The numeric value is the FS_SEL / AFS_SEL field. */
typedef enum { MPU6050_ACCEL_2G = 0, MPU6050_ACCEL_4G, MPU6050_ACCEL_8G, MPU6050_ACCEL_16G } mpu6050_accel_fs_t;
typedef enum { MPU6050_GYRO_250DPS = 0, MPU6050_GYRO_500DPS, MPU6050_GYRO_1000DPS, MPU6050_GYRO_2000DPS } mpu6050_gyro_fs_t;

/** Result codes of the driver. */
typedef enum {
  MPU6050_OK = 0,
  MPU6050_ERR_BUS,      /**< bus error (NACK, timeout, ...) */
  MPU6050_ERR_WRONG_ID, /**< WHO_AM_I returned an unexpected value */
  MPU6050_ERR_ARG
} mpu6050_status_t;

/** Raw 14-byte burst decoded into signed 16-bit integers. */
typedef struct {
  int16_t ax, ay, az;
  int16_t temp;
  int16_t gx, gy, gz;
} mpu6050_raw_t;

/** Sample converted to physical units, integer arithmetic only
 *  (the Cortex-M0+ in the C031 has no FPU). */
typedef struct {
  int32_t ax_mg, ay_mg, az_mg; /**< milli-g */
  int32_t temp_cdeg;           /**< 0.01 degC */
  int32_t gx_mdps, gy_mdps, gz_mdps; /**< milli-degrees per second */
} mpu6050_sample_t;

/** Driver instance. */
typedef struct {
  const i2c_bus_t *bus;
  uint8_t addr7;
  mpu6050_accel_fs_t accel_fs;
  mpu6050_gyro_fs_t gyro_fs;
} mpu6050_t;

/** Bind the driver to a bus. Does not touch the hardware. */
void mpu6050_bind(mpu6050_t *dev, const i2c_bus_t *bus, uint8_t addr7);

/** Read WHO_AM_I and check it. */
mpu6050_status_t mpu6050_check_id(const mpu6050_t *dev, uint8_t *who_out);

/** Wake the chip up (PWR_MGMT_1 = 0x01, PLL with X gyro) and select ranges.
 *  Sample rate 1 kHz / (1 + 19) = 50 Hz, DLPF 44 Hz. */
mpu6050_status_t mpu6050_init(mpu6050_t *dev, mpu6050_accel_fs_t afs, mpu6050_gyro_fs_t gfs);

/** Read one 14-byte burst (ACCEL_XOUT_H ... GYRO_ZOUT_L). */
mpu6050_status_t mpu6050_read_raw(const mpu6050_t *dev, mpu6050_raw_t *raw);

/** Pure function: big-endian 14-byte burst -> raw struct. */
void mpu6050_decode_burst(const uint8_t burst[14], mpu6050_raw_t *raw);

/** Pure function: raw counts -> physical units for the configured ranges. */
void mpu6050_convert(const mpu6050_t *dev, const mpu6050_raw_t *raw, mpu6050_sample_t *out);

/** Pure function: temperature in 0.01 degC from the raw register value
 *  (datasheet: T = raw / 340 + 36.53). */
int32_t mpu6050_temp_cdeg(int16_t raw);

#endif /* DEVICES_MPU6050_H */
