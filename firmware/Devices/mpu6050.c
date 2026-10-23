#include "mpu6050.h"

/* LSB per unit from the Product Specification (accel: LSB/g, gyro: LSB/(deg/s)).
 * Gyro sensitivities are 131, 65.5, 32.8, 16.4 LSB/(deg/s); to stay in
 * integers they are stored multiplied by 10. */
static const int32_t k_accel_lsb_per_g[4] = {16384, 8192, 4096, 2048};
static const int32_t k_gyro_lsb_per_dps_x10[4] = {1310, 655, 328, 164};

void mpu6050_bind(mpu6050_t *dev, const i2c_bus_t *bus, uint8_t addr7) {
  dev->bus = bus;
  dev->addr7 = addr7;
  dev->accel_fs = MPU6050_ACCEL_2G;
  dev->gyro_fs = MPU6050_GYRO_250DPS;
}

static mpu6050_status_t read_regs(const mpu6050_t *dev, uint8_t reg, uint8_t *buf, size_t n) {
  bus_status_t s = dev->bus->xfer(dev->bus->ctx, dev->addr7, &reg, 1, buf, n);
  return (s == BUS_OK) ? MPU6050_OK : MPU6050_ERR_BUS;
}

static mpu6050_status_t write_reg(const mpu6050_t *dev, uint8_t reg, uint8_t val) {
  const uint8_t tx[2] = {reg, val};
  bus_status_t s = dev->bus->xfer(dev->bus->ctx, dev->addr7, tx, 2, NULL, 0);
  return (s == BUS_OK) ? MPU6050_OK : MPU6050_ERR_BUS;
}

mpu6050_status_t mpu6050_check_id(const mpu6050_t *dev, uint8_t *who_out) {
  uint8_t who = 0;
  mpu6050_status_t st = read_regs(dev, MPU6050_REG_WHO_AM_I, &who, 1);
  if (st != MPU6050_OK) {
    return st;
  }
  if (who_out != NULL) {
    *who_out = who;
  }
  return (who == MPU6050_WHO_AM_I_VALUE) ? MPU6050_OK : MPU6050_ERR_WRONG_ID;
}

mpu6050_status_t mpu6050_init(mpu6050_t *dev, mpu6050_accel_fs_t afs, mpu6050_gyro_fs_t gfs) {
  if ((unsigned)afs > 3u || (unsigned)gfs > 3u) {
    return MPU6050_ERR_ARG;
  }
  mpu6050_status_t st = mpu6050_check_id(dev, NULL);
  if (st != MPU6050_OK) {
    return st;
  }
  /* Leave sleep mode, clock from the X gyro PLL (recommended by the datasheet). */
  if ((st = write_reg(dev, MPU6050_REG_PWR_MGMT_1, 0x01u)) != MPU6050_OK) {
    return st;
  }
  if ((st = write_reg(dev, MPU6050_REG_SMPLRT_DIV, 19u)) != MPU6050_OK) {
    return st;
  }
  if ((st = write_reg(dev, MPU6050_REG_CONFIG, 0x03u)) != MPU6050_OK) { /* DLPF 44 Hz */
    return st;
  }
  if ((st = write_reg(dev, MPU6050_REG_GYRO_CONFIG, (uint8_t)((unsigned)gfs << 3))) != MPU6050_OK) {
    return st;
  }
  if ((st = write_reg(dev, MPU6050_REG_ACCEL_CONFIG, (uint8_t)((unsigned)afs << 3))) != MPU6050_OK) {
    return st;
  }
  dev->accel_fs = afs;
  dev->gyro_fs = gfs;
  return MPU6050_OK;
}

void mpu6050_decode_burst(const uint8_t b[14], mpu6050_raw_t *raw) {
  raw->ax = (int16_t)(((unsigned)b[0] << 8) | b[1]);
  raw->ay = (int16_t)(((unsigned)b[2] << 8) | b[3]);
  raw->az = (int16_t)(((unsigned)b[4] << 8) | b[5]);
  raw->temp = (int16_t)(((unsigned)b[6] << 8) | b[7]);
  raw->gx = (int16_t)(((unsigned)b[8] << 8) | b[9]);
  raw->gy = (int16_t)(((unsigned)b[10] << 8) | b[11]);
  raw->gz = (int16_t)(((unsigned)b[12] << 8) | b[13]);
}

mpu6050_status_t mpu6050_read_raw(const mpu6050_t *dev, mpu6050_raw_t *raw) {
  uint8_t burst[14];
  mpu6050_status_t st = read_regs(dev, MPU6050_REG_ACCEL_XOUT_H, burst, sizeof burst);
  if (st != MPU6050_OK) {
    return st;
  }
  mpu6050_decode_burst(burst, raw);
  return MPU6050_OK;
}

int32_t mpu6050_temp_cdeg(int16_t raw) {
  /* T[degC] = raw / 340 + 36.53  ->  in 0.01 degC: raw * 100 / 340 + 3653 */
  return ((int32_t)raw * 100) / 340 + 3653;
}

static int32_t accel_mg(int16_t raw, int32_t lsb_per_g) {
  return ((int32_t)raw * 1000) / lsb_per_g;
}

static int32_t gyro_mdps(int16_t raw, int32_t lsb_per_dps_x10) {
  /* raw / (lsb_x10 / 10) deg/s -> * 1000 mdps = raw * 10000 / lsb_x10 */
  return ((int32_t)raw * 10000) / lsb_per_dps_x10;
}

void mpu6050_convert(const mpu6050_t *dev, const mpu6050_raw_t *raw, mpu6050_sample_t *out) {
  const int32_t a = k_accel_lsb_per_g[dev->accel_fs & 3u];
  const int32_t g = k_gyro_lsb_per_dps_x10[dev->gyro_fs & 3u];
  out->ax_mg = accel_mg(raw->ax, a);
  out->ay_mg = accel_mg(raw->ay, a);
  out->az_mg = accel_mg(raw->az, a);
  out->temp_cdeg = mpu6050_temp_cdeg(raw->temp);
  out->gx_mdps = gyro_mdps(raw->gx, g);
  out->gy_mdps = gyro_mdps(raw->gy, g);
  out->gz_mdps = gyro_mdps(raw->gz, g);
}
