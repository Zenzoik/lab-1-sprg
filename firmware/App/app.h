/**
 * @file app.h
 * @brief Application layer of the Motion-Mirroring Teleoperator (LR3: super loop with measurements).
 *
 * Owns the device drivers, the self-test, the UART command line, the task table of the
 * super loop and the measurement module. It reaches hardware only through app_ports_t.
 */
#ifndef APP_APP_H
#define APP_APP_H

#include "app_ports.h"

/** Bind drivers to the ports. Must be called once before anything else. */
void app_init(const app_ports_t *ports);

/** Boot sequence: banner, self-test, deliberate-anomaly demo, PWM demo. */
void app_boot(void);

/** One pass of the super loop: UART commands, button events, 50 Hz control, LED, telemetry,
 *  flash journal, long-operation generator. Each task is a time-tagged non-blocking step
 *  except where it has to wait for a device (flash erase, UART transmit, I2C timeout). */
void app_poll(void);

/** Bring-up self-test; prints one machine-readable line per device.
 *  @return number of failed checks (0 = all devices healthy). */
int app_selftest(void);

/** Execute one UART command line (without line terminator). */
void app_handle_line(const char *line);

#endif /* APP_APP_H */
