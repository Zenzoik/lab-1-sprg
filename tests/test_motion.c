#include "motion.h"
#include "test.h"

static void test_isqrt_exact_squares(void) {
  ASSERT_EQ_INT(0, motion_isqrt(0));
  ASSERT_EQ_INT(1, motion_isqrt(1));
  ASSERT_EQ_INT(1000, motion_isqrt(1000000));
  ASSERT_EQ_INT(46340, motion_isqrt(2147395600u)); /* 46340^2 */
}

static void test_isqrt_floor_between_squares(void) {
  ASSERT_EQ_INT(31, motion_isqrt(1023)); /* 31^2 = 961, 32^2 = 1024 */
  ASSERT_EQ_INT(32, motion_isqrt(1024));
  ASSERT_EQ_INT(65535, motion_isqrt(0xFFFFFFFFu));
}

static void test_norm_of_captured_sample_is_one_g(void) {
  /* Capture i2c_burst: AZ raw 0x4000 = 16384 LSB = 1000 mg, AX = AY = 0 */
  ASSERT_EQ_INT(1000, motion_norm_mg(0, 0, 1000));
  /* 3-4-5 triangle */
  ASSERT_EQ_INT(500, motion_norm_mg(300, 400, 0));
}

static void test_atan2_cardinal_directions(void) {
  ASSERT_EQ_INT(0, motion_atan2_cdeg(0, 1000));
  ASSERT_EQ_INT(9000, motion_atan2_cdeg(1000, 0));
  ASSERT_EQ_INT(-9000, motion_atan2_cdeg(-1000, 0));
  ASSERT_EQ_INT(18000, motion_atan2_cdeg(0, -1000));
  ASSERT_EQ_INT(0, motion_atan2_cdeg(0, 0));
}

static void test_atan2_table_points(void) {
  ASSERT_EQ_INT(4500, motion_atan2_cdeg(1000, 1000));
  ASSERT_EQ_INT(-4500, motion_atan2_cdeg(-1000, 1000));
  ASSERT_EQ_INT(13500, motion_atan2_cdeg(1000, -1000));
  /* tan(30 deg) = 0.57735 -> 3000 centi-degrees, interpolation error < 0.02 deg */
  ASSERT_NEAR(3000, motion_atan2_cdeg(57735, 100000), 3);
  /* tan(10 deg) */
  ASSERT_NEAR(1000, motion_atan2_cdeg(17633, 100000), 3);
}

static void test_pitch_roll_flat_and_tilted(void) {
  ASSERT_EQ_INT(0, motion_pitch_cdeg(0, 0, 1000));
  ASSERT_EQ_INT(0, motion_roll_cdeg(0, 1000));
  /* tilted 30 deg about Y: ax = -sin(30) = -500 mg, az = cos(30) = 866 mg -> pitch +30 deg */
  ASSERT_NEAR(3000, motion_pitch_cdeg(-500, 0, 866), 5);
  /* tilted 45 deg about X: ay = sin = 707, az = cos = 707 -> roll +45 deg */
  ASSERT_NEAR(4500, motion_roll_cdeg(707, 707), 5);
}

static void test_gain_maps_adc_range_to_50_200_percent(void) {
  ASSERT_EQ_INT(50, motion_gain_pct(0));
  ASSERT_EQ_INT(200, motion_gain_pct(4095));
  ASSERT_EQ_INT(125, motion_gain_pct(2048)); /* mid-scale, floor(2048*150/4095) = 75 */
  ASSERT_EQ_INT(200, motion_gain_pct(65535)); /* out-of-range input is clamped */
}

static void test_servo_pulse_centre_and_scale(void) {
  ASSERT_EQ_INT(1500, motion_servo_pulse_us(0, 100));
  /* 90 deg at 100 % = +500 us (datasheet convention 1000..2000 us) */
  ASSERT_EQ_INT(2000, motion_servo_pulse_us(9000, 100));
  ASSERT_EQ_INT(1000, motion_servo_pulse_us(-9000, 100));
  /* 45 deg at 50 % = +125 us */
  ASSERT_EQ_INT(1625, motion_servo_pulse_us(4500, 50));
}

static void test_servo_pulse_is_clamped_to_safe_range(void) {
  ASSERT_EQ_INT(2000, motion_servo_pulse_us(18000, 200));
  ASSERT_EQ_INT(1000, motion_servo_pulse_us(-18000, 200));
}

static void test_adc_to_millivolt(void) {
  ASSERT_EQ_INT(0, motion_adc_to_mv(0));
  ASSERT_EQ_INT(3300, motion_adc_to_mv(4095));
  ASSERT_NEAR(1650, motion_adc_to_mv(2048), 1);
}

static const test_case_t k_tests[] = {
    {"isqrt exact squares", test_isqrt_exact_squares},
    {"isqrt floors between squares", test_isqrt_floor_between_squares},
    {"norm of captured sample = 1 g", test_norm_of_captured_sample_is_one_g},
    {"atan2 cardinal directions", test_atan2_cardinal_directions},
    {"atan2 table points and interpolation", test_atan2_table_points},
    {"pitch/roll flat and tilted", test_pitch_roll_flat_and_tilted},
    {"gain maps ADC range to 50..200 %", test_gain_maps_adc_range_to_50_200_percent},
    {"servo pulse centre and scale", test_servo_pulse_centre_and_scale},
    {"servo pulse clamped to 1000..2000 us", test_servo_pulse_is_clamped_to_safe_range},
    {"ADC to millivolt", test_adc_to_millivolt},
};

int suite_motion(void) {
  return RUN_SUITE("motion (pure math)", k_tests);
}
