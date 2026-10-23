#include "fakes.h"
#include "mpu6050.h"
#include "test.h"

/* 14-byte burst recorded with the logic analyzer (docs/captures/i2c_burst.txt):
 * ACCEL 0,0,0x4000 (1 g on Z), TEMP 0xEF5C, GYRO 0,0,0 */
static const uint8_t k_captured_burst[14] = {0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0xEF,
                                             0x5C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static void setup(fake_i2c_t *f, mpu6050_t *dev) {
  fake_i2c_init(f, MPU6050_ADDR_AD0_LOW);
  mpu6050_bind(dev, &f->bus, MPU6050_ADDR_AD0_LOW);
}

/* ---- pure conversions ------------------------------------------------- */

static void test_decode_burst_big_endian_signed(void) {
  const uint8_t b[14] = {0x80, 0x00, 0x7F, 0xFF, 0xFF, 0xFF, 0x12, 0x34, 0x00, 0x01, 0xFF, 0xFE, 0x01, 0x00};
  mpu6050_raw_t r;
  mpu6050_decode_burst(b, &r);
  ASSERT_EQ_INT(-32768, r.ax);
  ASSERT_EQ_INT(32767, r.ay);
  ASSERT_EQ_INT(-1, r.az);
  ASSERT_EQ_INT(0x1234, r.temp);
  ASSERT_EQ_INT(1, r.gx);
  ASSERT_EQ_INT(-2, r.gy);
  ASSERT_EQ_INT(256, r.gz);
}

static void test_convert_captured_vector(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  mpu6050_raw_t raw;
  mpu6050_sample_t s;
  mpu6050_decode_burst(k_captured_burst, &raw);
  mpu6050_convert(&dev, &raw, &s);
  ASSERT_EQ_INT(0, s.ax_mg);
  ASSERT_EQ_INT(0, s.ay_mg);
  ASSERT_EQ_INT(1000, s.az_mg);                 /* 16384 LSB / (16384 LSB/g) = 1 g */
  ASSERT_NEAR(2400, s.temp_cdeg, 2);            /* 0xEF5C = -4260: -4260/340 + 36.53 = 24.00 degC */
  ASSERT_EQ_INT(0, s.gz_mdps);
}

static void test_temperature_datasheet_formula(void) {
  /* T = raw/340 + 36.53 degC (Register Map, TEMP_OUT): raw 0 is 36.53 degC */
  ASSERT_EQ_INT(3653, mpu6050_temp_cdeg(0));
  ASSERT_NEAR(3753, mpu6050_temp_cdeg(340), 1);   /* +1 degC per 340 LSB */
  ASSERT_NEAR(3553, mpu6050_temp_cdeg(-340), 1);
}

static void test_convert_all_full_scale_ranges(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  mpu6050_raw_t raw = {.ax = 16384, .ay = 0, .az = 0, .temp = 0, .gx = 131, .gy = 0, .gz = 0};
  mpu6050_sample_t s;
  const int32_t expect_mg[4] = {1000, 2000, 4000, 8000}; /* LSB/g = 16384, 8192, 4096, 2048 */
  const int32_t expect_mdps[4] = {1000, 2000, 3993, 7987}; /* 131, 65.5, 32.8, 16.4 LSB/(deg/s) */
  for (unsigned i = 0; i < 4; i++) {
    dev.accel_fs = (mpu6050_accel_fs_t)i;
    dev.gyro_fs = (mpu6050_gyro_fs_t)i;
    mpu6050_convert(&dev, &raw, &s);
    ASSERT_EQ_INT(expect_mg[i], s.ax_mg);
    ASSERT_NEAR(expect_mdps[i], s.gx_mdps, 8);
  }
}

/* ---- identification --------------------------------------------------- */

static void test_check_id_ok_reads_register_0x75(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  uint8_t who = 0;
  ASSERT_EQ_INT(MPU6050_OK, mpu6050_check_id(&dev, &who));
  ASSERT_EQ_INT(0x68, who);
  ASSERT_EQ_INT(1, f.log_n);
  ASSERT_EQ_INT(1, f.log[0].is_read);
  ASSERT_EQ_INT(0x75, f.log[0].reg);
  ASSERT_EQ_INT(1, f.log[0].len);
}

static void test_check_id_unexpected_value(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  f.regs[0x75] = 0x70; /* e.g. an MPU-6500 / clone */
  uint8_t who = 0;
  ASSERT_EQ_INT(MPU6050_ERR_WRONG_ID, mpu6050_check_id(&dev, &who));
  ASSERT_EQ_INT(0x70, who);
}

static void test_check_id_nack_when_device_missing(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  fake_i2c_init(&f, MPU6050_ADDR_AD0_LOW);
  f.present = 0;
  mpu6050_bind(&dev, &f.bus, MPU6050_ADDR_AD0_LOW);
  ASSERT_EQ_INT(MPU6050_ERR_BUS, mpu6050_check_id(&dev, NULL));
}

static void test_check_id_wrong_address_nack(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  fake_i2c_init(&f, MPU6050_ADDR_AD0_LOW);
  mpu6050_bind(&dev, &f.bus, MPU6050_ADDR_AD0_HIGH); /* AD0 strapped differently than wired */
  ASSERT_EQ_INT(MPU6050_ERR_BUS, mpu6050_check_id(&dev, NULL));
  ASSERT_EQ_INT(BUS_ERR_NACK, f.bus.probe(f.bus.ctx, MPU6050_ADDR_AD0_HIGH));
}

static void test_check_id_bus_timeout(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  f.inject = BUS_ERR_TIMEOUT;
  ASSERT_EQ_INT(MPU6050_ERR_BUS, mpu6050_check_id(&dev, NULL));
}

/* ---- initialisation --------------------------------------------------- */

static void test_init_register_sequence_matches_capture(void) {
  /* docs/captures/i2c_init.txt: after WHO_AM_I the master wrote
   * 6B<-01, 19<-13, 1A<-03, 1B<-00, 1C<-00 */
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  ASSERT_EQ_INT(MPU6050_OK, mpu6050_init(&dev, MPU6050_ACCEL_2G, MPU6050_GYRO_250DPS));
  ASSERT_EQ_INT(6, f.log_n);
  ASSERT_EQ_INT(0x75, f.log[0].reg);
  const uint8_t regs[5] = {0x6B, 0x19, 0x1A, 0x1B, 0x1C};
  const uint8_t vals[5] = {0x01, 0x13, 0x03, 0x00, 0x00};
  for (int i = 0; i < 5; i++) {
    ASSERT_EQ_INT(0, f.log[i + 1].is_read);
    ASSERT_EQ_INT(regs[i], f.log[i + 1].reg);
    ASSERT_EQ_INT(vals[i], f.regs[regs[i]]);
  }
}

static void test_init_selects_requested_full_scale(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  ASSERT_EQ_INT(MPU6050_OK, mpu6050_init(&dev, MPU6050_ACCEL_8G, MPU6050_GYRO_1000DPS));
  ASSERT_EQ_INT(2 << 3, f.regs[0x1C]); /* AFS_SEL = 2 */
  ASSERT_EQ_INT(2 << 3, f.regs[0x1B]); /* FS_SEL  = 2 */
  ASSERT_EQ_INT(MPU6050_ACCEL_8G, dev.accel_fs);
  ASSERT_EQ_INT(MPU6050_GYRO_1000DPS, dev.gyro_fs);
}

static void test_init_stops_on_wrong_id_and_writes_nothing(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  f.regs[0x75] = 0x00; /* what a dead/unpowered bus would read as */
  ASSERT_EQ_INT(MPU6050_ERR_WRONG_ID, mpu6050_init(&dev, MPU6050_ACCEL_2G, MPU6050_GYRO_250DPS));
  ASSERT_EQ_INT(1, f.log_n); /* only the WHO_AM_I read happened */
  ASSERT_EQ_INT(0x40, f.regs[0x6B]); /* still asleep */
}

static void test_init_rejects_bad_range_argument(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  ASSERT_EQ_INT(MPU6050_ERR_ARG, mpu6050_init(&dev, (mpu6050_accel_fs_t)4, MPU6050_GYRO_250DPS));
  ASSERT_EQ_INT(MPU6050_ERR_ARG, mpu6050_init(&dev, MPU6050_ACCEL_2G, (mpu6050_gyro_fs_t)9));
  ASSERT_EQ_INT(0, f.calls);
}

static void test_init_reports_bus_error_in_the_middle(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  f.fail_on_call = 3; /* WHO_AM_I ok, PWR_MGMT_1 ok, SMPLRT_DIV fails */
  f.inject_once = BUS_ERR_NACK;
  ASSERT_EQ_INT(MPU6050_ERR_BUS, mpu6050_init(&dev, MPU6050_ACCEL_2G, MPU6050_GYRO_250DPS));
  ASSERT_EQ_INT(0x01, f.regs[0x6B]);
  ASSERT_EQ_INT(0x00, f.regs[0x1A]); /* later registers untouched */
}

/* ---- reading ---------------------------------------------------------- */

static void test_read_raw_is_one_14_byte_burst_from_0x3b(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  fake_i2c_set_burst(&f, k_captured_burst);
  mpu6050_raw_t raw;
  ASSERT_EQ_INT(MPU6050_OK, mpu6050_read_raw(&dev, &raw));
  ASSERT_EQ_INT(1, f.log_n);
  ASSERT_EQ_INT(1, f.log[0].is_read);
  ASSERT_EQ_INT(0x3B, f.log[0].reg);
  ASSERT_EQ_INT(14, f.log[0].len);
  ASSERT_EQ_INT(0x4000, raw.az);
  ASSERT_EQ_INT((int16_t)0xEF5C, raw.temp);
}

static void test_read_raw_propagates_bus_errors(void) {
  fake_i2c_t f;
  mpu6050_t dev;
  setup(&f, &dev);
  mpu6050_raw_t raw;
  f.inject = BUS_ERR_NACK;
  ASSERT_EQ_INT(MPU6050_ERR_BUS, mpu6050_read_raw(&dev, &raw));
  f.inject = BUS_ERR_TIMEOUT;
  ASSERT_EQ_INT(MPU6050_ERR_BUS, mpu6050_read_raw(&dev, &raw));
}

static const test_case_t k_tests[] = {
    {"decode burst: big-endian, signed", test_decode_burst_big_endian_signed},
    {"convert: vector captured on the wire", test_convert_captured_vector},
    {"temperature: datasheet formula", test_temperature_datasheet_formula},
    {"convert: all full-scale ranges", test_convert_all_full_scale_ranges},
    {"WHO_AM_I: ok, register 0x75", test_check_id_ok_reads_register_0x75},
    {"WHO_AM_I: unexpected value", test_check_id_unexpected_value},
    {"WHO_AM_I: NACK when device missing", test_check_id_nack_when_device_missing},
    {"WHO_AM_I: wrong I2C address -> NACK", test_check_id_wrong_address_nack},
    {"WHO_AM_I: bus timeout", test_check_id_bus_timeout},
    {"init: register sequence equals capture", test_init_register_sequence_matches_capture},
    {"init: requested full-scale ranges", test_init_selects_requested_full_scale},
    {"init: wrong ID -> nothing written", test_init_stops_on_wrong_id_and_writes_nothing},
    {"init: bad range argument", test_init_rejects_bad_range_argument},
    {"init: bus error half-way", test_init_reports_bus_error_in_the_middle},
    {"read: one 14-byte burst from 0x3B", test_read_raw_is_one_14_byte_burst_from_0x3b},
    {"read: bus errors propagate", test_read_raw_propagates_bus_errors},
};

int suite_mpu6050(void) {
  return RUN_SUITE("mpu6050 driver (fake I2C)", k_tests);
}
