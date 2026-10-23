/**
 * @file motion.h
 * @brief Pure integer math of the teleoperator: orientation angles and servo mapping.
 *
 * No hardware access, no floating point (Cortex-M0+ has no FPU).
 * Angles are in centi-degrees (0.01 deg), accelerations in milli-g.
 */
#ifndef APP_MOTION_H
#define APP_MOTION_H

#include <stdint.h>

#define MOTION_SERVO_MIN_US 1000u
#define MOTION_SERVO_CENTER_US 1500u
#define MOTION_SERVO_MAX_US 2000u

#define MOTION_GAIN_MIN_PCT 50u
#define MOTION_GAIN_MAX_PCT 200u

/** floor(sqrt(v)). */
uint32_t motion_isqrt(uint32_t v);

/** Length of the acceleration vector in milli-g. */
uint32_t motion_norm_mg(int32_t ax, int32_t ay, int32_t az);

/** atan2(y, x) in centi-degrees, range (-18000, 18000]. Error < 0.02 deg. */
int32_t motion_atan2_cdeg(int32_t y, int32_t x);

/** Pitch = atan2(-ax, sqrt(ay^2 + az^2)), roll = atan2(ay, az). */
int32_t motion_pitch_cdeg(int32_t ax, int32_t ay, int32_t az);
int32_t motion_roll_cdeg(int32_t ay, int32_t az);

/** Potentiometer (12-bit) -> sensitivity in percent, 50..200. */
uint32_t motion_gain_pct(uint16_t adc_raw);

/** angle * gain -> servo pulse; +-90 deg at 100 % gain = 1000..2000 us, clamped. */
uint16_t motion_servo_pulse_us(int32_t angle_cdeg, uint32_t gain_pct);

/** ADC counts -> millivolts for a 3.3 V reference. */
uint32_t motion_adc_to_mv(uint16_t adc_raw);

#endif /* APP_MOTION_H */
