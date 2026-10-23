#include "motion.h"

/* atan(i/32) for i = 0..32 in centi-degrees: round(degrees(atan(i/32)) * 100).
 * Linear interpolation between entries gives < 0.02 deg error. */
static const uint16_t k_atan_cdeg[33] = {
    0,    179,  358,  536,  713,  888,  1062, 1234, 1404, 1571, 1735,
    1897, 2056, 2211, 2363, 2511, 2657, 2798, 2936, 3070, 3201, 3327,
    3451, 3571, 3687, 3800, 3909, 4016, 4119, 4218, 4315, 4409, 4500};

uint32_t motion_isqrt(uint32_t v) {
  uint32_t res = 0;
  uint32_t bit = 1u << 30;
  while (bit > v) {
    bit >>= 2;
  }
  while (bit != 0u) {
    if (v >= res + bit) {
      v -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return res;
}

uint32_t motion_norm_mg(int32_t ax, int32_t ay, int32_t az) {
  /* Inputs are limited to +-16 g (16000 mg), so the sum fits in 32 bits. */
  const uint32_t sum = (uint32_t)(ax * ax) + (uint32_t)(ay * ay) + (uint32_t)(az * az);
  return motion_isqrt(sum);
}

/* atan(num/den) for 0 <= num <= den, den > 0, result in centi-degrees. */
static int32_t atan_ratio_cdeg(uint32_t num, uint32_t den) {
  const uint32_t scaled = num << 5; /* num * 32, num < 2^16 */
  uint32_t idx = scaled / den;
  const uint32_t rem = scaled % den;
  if (idx >= 32u) {
    return (int32_t)k_atan_cdeg[32];
  }
  const int32_t lo = (int32_t)k_atan_cdeg[idx];
  const int32_t hi = (int32_t)k_atan_cdeg[idx + 1u];
  return lo + (int32_t)(((uint32_t)(hi - lo) * rem + den / 2u) / den);
}

int32_t motion_atan2_cdeg(int32_t y, int32_t x) {
  if (x == 0 && y == 0) {
    return 0;
  }
  const uint32_t ax = (uint32_t)((x < 0) ? -x : x);
  const uint32_t ay = (uint32_t)((y < 0) ? -y : y);
  int32_t a; /* angle in the first octant pair: 0..90 deg */
  if (ay <= ax) {
    a = atan_ratio_cdeg(ay, ax);
  } else {
    a = 9000 - atan_ratio_cdeg(ax, ay);
  }
  if (x < 0) {
    a = 18000 - a;
  }
  return (y < 0) ? -a : a;
}

int32_t motion_pitch_cdeg(int32_t ax, int32_t ay, int32_t az) {
  const uint32_t yz = motion_isqrt((uint32_t)(ay * ay) + (uint32_t)(az * az));
  return motion_atan2_cdeg(-ax, (int32_t)yz);
}

int32_t motion_roll_cdeg(int32_t ay, int32_t az) {
  return motion_atan2_cdeg(ay, az);
}

uint32_t motion_gain_pct(uint16_t adc_raw) {
  uint32_t r = (adc_raw > 4095u) ? 4095u : adc_raw;
  return MOTION_GAIN_MIN_PCT + (r * (MOTION_GAIN_MAX_PCT - MOTION_GAIN_MIN_PCT)) / 4095u;
}

uint16_t motion_servo_pulse_us(int32_t angle_cdeg, uint32_t gain_pct) {
  /* 90 deg (9000 cdeg) at 100 % -> 500 us:  delta = angle * gain / 1800 */
  const int32_t delta = (angle_cdeg * (int32_t)gain_pct) / 1800;
  int32_t us = (int32_t)MOTION_SERVO_CENTER_US + delta;
  if (us < (int32_t)MOTION_SERVO_MIN_US) {
    us = (int32_t)MOTION_SERVO_MIN_US;
  }
  if (us > (int32_t)MOTION_SERVO_MAX_US) {
    us = (int32_t)MOTION_SERVO_MAX_US;
  }
  return (uint16_t)us;
}

uint32_t motion_adc_to_mv(uint16_t adc_raw) {
  return ((uint32_t)adc_raw * 3300u + 2047u) / 4095u;
}
