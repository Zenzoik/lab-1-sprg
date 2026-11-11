/**
 * @file platform.h
 * @brief Board adaptation layer: wraps HAL into the interfaces the upper layers use.
 */
#ifndef PLATFORM_PLATFORM_H
#define PLATFORM_PLATFORM_H

#include "app_ports.h"

/** Fill the port table with HAL-backed implementations and start PWM.
 *  Call after the CubeMX MX_*_Init() functions. */
void platform_init(void);

/** Fill the unused stack area with a pattern; call first thing in main() to measure stack depth. */
void platform_stack_paint(void);

/** Called by SysTick_Handler once per millisecond (after HAL_IncTick): drives the LR3 stimulus. */
void platform_systick_hook(void);

/** Port table for app_init(). */
const app_ports_t *platform_ports(void);

#endif /* PLATFORM_PLATFORM_H */
