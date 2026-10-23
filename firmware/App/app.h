/**
 * @file app.h
 * @brief Application layer of the Motion-Mirroring Teleoperator (bring-up stage, LR2).
 *
 * Owns the device drivers, the self-test, the UART command line and the
 * periodic control loop. It reaches hardware only through app_ports_t.
 */
#ifndef APP_APP_H
#define APP_APP_H

#include "app_ports.h"

/** Bind drivers to the ports. Must be called once before anything else. */
void app_init(const app_ports_t *ports);

/** Boot sequence: banner, self-test, deliberate-anomaly demo, PWM demo. */
void app_boot(void);

/** Non-blocking main-loop step: UART commands, 50 Hz control, 1 Hz telemetry. */
void app_poll(void);

/** Bring-up self-test; prints one machine-readable line per device.
 *  @return number of failed checks (0 = all devices healthy). */
int app_selftest(void);

/** Execute one UART command line (without line terminator). */
void app_handle_line(const char *line);

#endif /* APP_APP_H */
