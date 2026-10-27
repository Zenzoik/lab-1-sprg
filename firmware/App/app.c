#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "motion.h"
#include "mpu6050.h"
#include "w25q64.h"

#define APP_VERSION "lr2"

/* Flash scratch area used by the self-test (sector 1; sector 0 is kept for calibration later). */
#define SELFTEST_SECTOR_ADDR 0x001000u
#define SELFTEST_RW_ADDR 0x0010F8u /* 16 bytes crossing the page boundary at 0x1100 */
#define SELFTEST_RW_LEN 16u

#define CONTROL_PERIOD_MS 20u
#define TELEMETRY_PERIOD_MS 1000u

#define ACCEL_NORM_TOLERANCE_PCT 10u
#define ROOM_TEMP_MIN_CDEG 1000
#define ROOM_TEMP_MAX_CDEG 4000

typedef struct {
  const app_ports_t *p;
  mpu6050_t imu;
  w25q_t flash;
  int imu_ready;
  uint32_t last_control_ms;
  uint32_t last_telemetry_ms;
  char line[40];
  size_t line_len;
  unsigned pass;
  unsigned fail;
  int32_t pitch_cdeg;
  int32_t roll_cdeg;
  uint32_t gain_pct;
  uint16_t adc_raw;
} app_t;

static app_t g_app;

/* ------------------------------------------------------------------ output */

static void emit(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }
  if ((size_t)n >= sizeof buf) {
    n = (int)sizeof buf - 1;
  }
  g_app.p->write(buf, (size_t)n);
}

/* One self-test result line:  DEV <name> status=OK|FAIL <details> */
static void report(const char *name, int ok, const char *details) {
  emit("DEV %s status=%s %s\r\n", name, ok ? "OK" : "FAIL", details);
  if (ok) {
    g_app.pass++;
  } else {
    g_app.fail++;
  }
}

/* Diagnostic ports are optional (tests and minimal platforms may leave them NULL). */
static uint32_t port_diag(uint32_t (*fn)(void)) {
  return (fn != NULL) ? fn() : 0u;
}

static uint32_t abs_diff_u32(uint32_t a, uint32_t b) {
  return (a > b) ? (a - b) : (b - a);
}

/* ------------------------------------------------------------------ self-test */

static void test_imu(void) {
  char d[120];
  uint8_t who = 0;
  mpu6050_status_t st = mpu6050_check_id(&g_app.imu, &who);
  (void)snprintf(d, sizeof d, "addr=0x%02X who=0x%02X expected=0x%02X", (unsigned)g_app.imu.addr7,
                 (unsigned)who, (unsigned)MPU6050_WHO_AM_I_VALUE);
  report("mpu6050_id", st == MPU6050_OK, d);
  if (st != MPU6050_OK) {
    g_app.imu_ready = 0;
    report("mpu6050_data", 0, "skipped=mpu6050_id_failed");
    return;
  }

  st = mpu6050_init(&g_app.imu, MPU6050_ACCEL_2G, MPU6050_GYRO_250DPS);
  g_app.imu_ready = (st == MPU6050_OK);
  g_app.p->delay_ms(30); /* first sample after wake-up: 1 / 50 Hz + filter settling */

  mpu6050_raw_t raw;
  mpu6050_sample_t s;
  st = g_app.imu_ready ? mpu6050_read_raw(&g_app.imu, &raw) : MPU6050_ERR_BUS;
  if (st != MPU6050_OK) {
    report("mpu6050_data", 0, "read=error");
    return;
  }
  mpu6050_convert(&g_app.imu, &raw, &s);
  const uint32_t norm = motion_norm_mg(s.ax_mg, s.ay_mg, s.az_mg);
  const uint32_t err_pct = abs_diff_u32(norm, 1000u) / 10u; /* |norm - 1 g| / 1 g * 100 */
  const int ok = (err_pct <= ACCEL_NORM_TOLERANCE_PCT) && (s.temp_cdeg >= ROOM_TEMP_MIN_CDEG) &&
                 (s.temp_cdeg <= ROOM_TEMP_MAX_CDEG);
  (void)snprintf(d, sizeof d, "ax_mg=%ld ay_mg=%ld az_mg=%ld norm_mg=%lu err_pct=%lu temp_cdeg=%ld",
                 (long)s.ax_mg, (long)s.ay_mg, (long)s.az_mg, (unsigned long)norm,
                 (unsigned long)err_pct, (long)s.temp_cdeg);
  report("mpu6050_data", ok, d);
}

static void test_flash(void) {
  char d[120];
  uint8_t id[3] = {0, 0, 0};
  w25q_status_t st = w25q_check_id(&g_app.flash, id);
  (void)snprintf(d, sizeof d, "jedec=%02X%02X%02X expected=EF4017", (unsigned)id[0], (unsigned)id[1],
                 (unsigned)id[2]);
  report("w25q64_id", st == W25Q_OK, d);
  if (st != W25Q_OK) {
    report("w25q64_erase", 0, "skipped=w25q64_id_failed");
    report("w25q64_rw", 0, "skipped=w25q64_id_failed");
    return;
  }

  /* Erase the scratch sector and check it is blank. */
  uint32_t t0 = g_app.p->millis();
  st = w25q_sector_erase(&g_app.flash, SELFTEST_SECTOR_ADDR);
  const uint32_t erase_ms = g_app.p->millis() - t0;
  uint8_t blank[SELFTEST_RW_LEN];
  int is_blank = 0;
  if (st == W25Q_OK && w25q_read(&g_app.flash, SELFTEST_RW_ADDR, blank, sizeof blank) == W25Q_OK) {
    is_blank = 1;
    for (size_t i = 0; i < sizeof blank; i++) {
      if (blank[i] != 0xFFu) {
        is_blank = 0;
      }
    }
  }
  (void)snprintf(d, sizeof d, "addr=0x%06lX erase_ms=%lu blank=%d", (unsigned long)SELFTEST_SECTOR_ADDR,
                 (unsigned long)erase_ms, is_blank);
  report("w25q64_erase", st == W25Q_OK && is_blank, d);
  if (st != W25Q_OK) {
    report("w25q64_rw", 0, "skipped=w25q64_erase_failed");
    return;
  }

  /* Program a pattern across a page boundary, read it back byte by byte. */
  uint8_t wr[SELFTEST_RW_LEN];
  uint8_t rd[SELFTEST_RW_LEN];
  for (size_t i = 0; i < sizeof wr; i++) {
    wr[i] = (uint8_t)(0xA0u + i);
  }
  t0 = g_app.p->millis();
  st = w25q_write(&g_app.flash, SELFTEST_RW_ADDR, wr, sizeof wr);
  const uint32_t prog_ms = g_app.p->millis() - t0;
  int match = 0;
  if (st == W25Q_OK && w25q_read(&g_app.flash, SELFTEST_RW_ADDR, rd, sizeof rd) == W25Q_OK) {
    match = (memcmp(wr, rd, sizeof wr) == 0);
  }
  (void)snprintf(d, sizeof d, "addr=0x%06lX len=%u prog_ms=%lu match=%d", (unsigned long)SELFTEST_RW_ADDR,
                 (unsigned)SELFTEST_RW_LEN, (unsigned long)prog_ms, match);
  report("w25q64_rw", st == W25Q_OK && match, d);
}

static void test_adc_button_pwm(void) {
  char d[100];
  uint16_t raw = 0;
  g_app.p->marker(1);
  const int rc = g_app.p->adc_read(&raw);
  g_app.p->marker(0);
  (void)snprintf(d, sizeof d, "raw=%u mv=%lu gain_pct=%lu", (unsigned)raw,
                 (unsigned long)motion_adc_to_mv(raw), (unsigned long)motion_gain_pct(raw));
  report("adc", rc == 0 && raw <= 4095u, d);

  (void)snprintf(d, sizeof d, "pressed=%d", g_app.p->button_pressed() ? 1 : 0);
  report("button", 1, d);

  g_app.p->pwm_set_us(1, MOTION_SERVO_CENTER_US);
  g_app.p->pwm_set_us(2, MOTION_SERVO_CENTER_US);
  const unsigned c1 = g_app.p->pwm_get_us(1);
  const unsigned c2 = g_app.p->pwm_get_us(2);
  (void)snprintf(d, sizeof d, "ch1_us=%u ch2_us=%u period_us=20000", c1, c2);
  report("pwm", c1 == MOTION_SERVO_CENTER_US && c2 == MOTION_SERVO_CENTER_US, d);
}

int app_selftest(void) {
  g_app.pass = 0;
  g_app.fail = 0;
  emit("SELFTEST BEGIN version=%s t_ms=%lu\r\n", APP_VERSION, (unsigned long)g_app.p->millis());
  test_imu();
  test_flash();
  test_adc_button_pwm();
  emit("SELFTEST RESULT pass=%u fail=%u t_ms=%lu\r\n", g_app.pass, g_app.fail,
       (unsigned long)g_app.p->millis());
  return (int)g_app.fail;
}

/* ------------------------------------------------------------------ deliberate anomalies */

static void anomaly_demo(void) {
  /* 1) I2C: address nobody answers (MPU-6050 with AD0 high would be 0x69). */
  const i2c_bus_t *i2c = g_app.p->i2c;
  bus_status_t s = i2c->probe(i2c->ctx, MPU6050_ADDR_AD0_HIGH);
  emit("ANOMALY i2c_probe addr=0x%02X result=%s\r\n", (unsigned)MPU6050_ADDR_AD0_HIGH,
       (s == BUS_OK) ? "ACK" : "NACK");

  /* 2) SPI: flash is mode 0, the master is temporarily switched to mode 3. */
  uint8_t id[3] = {0, 0, 0};
  if (g_app.p->spi_set_mode(3) == 0) {
    (void)w25q_read_jedec(&g_app.flash, id);
    (void)g_app.p->spi_set_mode(0);
    emit("ANOMALY spi_mode=3 jedec=%02X%02X%02X expected=EF4017\r\n", (unsigned)id[0], (unsigned)id[1],
         (unsigned)id[2]);
  }
}

static void pwm_demo(void) {
  static const uint16_t steps[3] = {1000, 2000, 1500};
  for (size_t i = 0; i < 3u; i++) {
    g_app.p->pwm_set_us(1, steps[i]);
    emit("PWM_DEMO ch=1 us=%u t_ms=%lu\r\n", (unsigned)steps[i], (unsigned long)g_app.p->millis());
    g_app.p->delay_ms(100); /* five 20 ms periods at each width */
  }
}

/* ------------------------------------------------------------------ control loop */

static void control_step(void) {
  uint16_t raw = 0;
  g_app.p->marker(1);
  const int rc = g_app.p->adc_read(&raw);
  g_app.p->marker(0);
  if (rc == 0) {
    g_app.adc_raw = raw;
    g_app.gain_pct = motion_gain_pct(raw);
  }

  if (!g_app.imu_ready) {
    return;
  }
  mpu6050_raw_t r;
  mpu6050_sample_t s;
  if (mpu6050_read_raw(&g_app.imu, &r) != MPU6050_OK) {
    /* FR-11: lost sensor -> centre both servos. */
    g_app.imu_ready = 0;
    g_app.p->pwm_set_us(1, MOTION_SERVO_CENTER_US);
    g_app.p->pwm_set_us(2, MOTION_SERVO_CENTER_US);
    emit("ERROR mpu6050 lost, servos centred\r\n");
    return;
  }
  mpu6050_convert(&g_app.imu, &r, &s);
  g_app.pitch_cdeg = motion_pitch_cdeg(s.ax_mg, s.ay_mg, s.az_mg);
  g_app.roll_cdeg = motion_roll_cdeg(s.ay_mg, s.az_mg);
  g_app.p->pwm_set_us(1, motion_servo_pulse_us(g_app.pitch_cdeg, g_app.gain_pct));
  g_app.p->pwm_set_us(2, motion_servo_pulse_us(g_app.roll_cdeg, g_app.gain_pct));
}

static void telemetry(void) {
  emit("TLM t_ms=%lu pitch_cdeg=%ld roll_cdeg=%ld adc=%u gain_pct=%lu btn=%d p1_us=%u p2_us=%u\r\n",
       (unsigned long)g_app.p->millis(), (long)g_app.pitch_cdeg, (long)g_app.roll_cdeg,
       (unsigned)g_app.adc_raw, (unsigned long)g_app.gain_pct, g_app.p->button_pressed() ? 1 : 0,
       (unsigned)g_app.p->pwm_get_us(1), (unsigned)g_app.p->pwm_get_us(2));
}

/* ------------------------------------------------------------------ command line */

static int parse_uint(const char *s, int base, unsigned long *out) {
  unsigned long v = 0;
  int any = 0;
  for (; *s != '\0'; s++) {
    int d;
    if (*s >= '0' && *s <= '9') {
      d = *s - '0';
    } else if (*s >= 'a' && *s <= 'f') {
      d = *s - 'a' + 10;
    } else if (*s >= 'A' && *s <= 'F') {
      d = *s - 'A' + 10;
    } else {
      return -1;
    }
    if (d >= base) {
      return -1;
    }
    v = v * (unsigned long)base + (unsigned long)d;
    any = 1;
  }
  *out = v;
  return any ? 0 : -1;
}

static void cmd_help(void) {
  emit("CMD help | selftest | whoami | jedec | probe <hex addr7> | reg <addr7> <reg> [n] | pwm <1|2> <us> | adc | anomaly\r\n");
}

void app_handle_line(const char *line) {
  char buf[40];
  char *tok[4] = {NULL, NULL, NULL, NULL};
  size_t ntok = 0;
  size_t n = strlen(line);
  if (n >= sizeof buf) {
    n = sizeof buf - 1u;
  }
  memcpy(buf, line, n);
  buf[n] = '\0';
  for (char *p = buf; *p != '\0' && ntok < 4u;) {
    while (*p == ' ') {
      *p++ = '\0';
    }
    if (*p == '\0') {
      break;
    }
    tok[ntok++] = p;
    while (*p != '\0' && *p != ' ') {
      p++;
    }
  }
  if (ntok == 0u) {
    return;
  }

  if (strcmp(tok[0], "help") == 0) {
    cmd_help();
  } else if (strcmp(tok[0], "selftest") == 0) {
    (void)app_selftest();
  } else if (strcmp(tok[0], "whoami") == 0) {
    uint8_t who = 0;
    mpu6050_status_t st = mpu6050_check_id(&g_app.imu, &who);
    emit("CMD whoami status=%s who=0x%02X\r\n", (st == MPU6050_OK) ? "OK" : "FAIL", (unsigned)who);
  } else if (strcmp(tok[0], "jedec") == 0) {
    uint8_t id[3] = {0, 0, 0};
    w25q_status_t st = w25q_read_jedec(&g_app.flash, id);
    emit("CMD jedec status=%s id=%02X%02X%02X\r\n", (st == W25Q_OK) ? "OK" : "FAIL", (unsigned)id[0],
         (unsigned)id[1], (unsigned)id[2]);
  } else if (strcmp(tok[0], "probe") == 0 && ntok == 2u) {
    unsigned long a = 0;
    if (parse_uint(tok[1], 16, &a) != 0 || a > 0x7Fu) {
      emit("CMD error bad address\r\n");
    } else {
      bus_status_t s = g_app.p->i2c->probe(g_app.p->i2c->ctx, (uint8_t)a);
      emit("CMD probe addr=0x%02lX result=%s\r\n", a, (s == BUS_OK) ? "ACK" : "NACK");
    }
  } else if (strcmp(tok[0], "reg") == 0 && ntok >= 3u) {
    /* reg <addr7 hex> <reg hex> [n]: raw I2C register read (bring-up / diagnostics) */
    unsigned long a = 0;
    unsigned long r = 0;
    unsigned long cnt = 1;
    if (parse_uint(tok[1], 16, &a) != 0 || parse_uint(tok[2], 16, &r) != 0 || a > 0x7Fu || r > 0xFFu ||
        (ntok == 4u && (parse_uint(tok[3], 10, &cnt) != 0 || cnt < 1u || cnt > 14u))) {
      emit("CMD error usage: reg <addr7 hex> <reg hex> [1..14]\r\n");
    } else {
      uint8_t rbuf[14] = {0};
      const uint8_t reg = (uint8_t)r;
      bus_status_t s = g_app.p->i2c->xfer(g_app.p->i2c->ctx, (uint8_t)a, &reg, 1, rbuf, (size_t)cnt);
      emit("CMD reg addr=0x%02lX reg=0x%02lX status=%s rc=%d hal=0x%08lX isr=0x%08lX data=", a, r,
           (s == BUS_OK) ? "OK" : "FAIL", (int)s, (unsigned long)port_diag(g_app.p->i2c_last_error),
           (unsigned long)port_diag(g_app.p->i2c_status_reg));
      for (unsigned long i = 0; i < cnt; i++) {
        emit("%02X", (unsigned)rbuf[i]);
      }
      emit("\r\n");
    }
  } else if (strcmp(tok[0], "regw") == 0 && ntok == 4u) {
    /* regw <addr7 hex> <reg hex> <value hex>: raw I2C register write (bring-up / diagnostics) */
    unsigned long a = 0;
    unsigned long r = 0;
    unsigned long v = 0;
    if (parse_uint(tok[1], 16, &a) != 0 || parse_uint(tok[2], 16, &r) != 0 || parse_uint(tok[3], 16, &v) != 0 ||
        a > 0x7Fu || r > 0xFFu || v > 0xFFu) {
      emit("CMD error usage: regw <addr7 hex> <reg hex> <value hex>\r\n");
    } else {
      const uint8_t wbuf[2] = {(uint8_t)r, (uint8_t)v};
      bus_status_t s = g_app.p->i2c->xfer(g_app.p->i2c->ctx, (uint8_t)a, wbuf, 2u, NULL, 0u);
      emit("CMD regw addr=0x%02lX reg=0x%02lX val=0x%02lX status=%s rc=%d hal=0x%08lX isr=0x%08lX\r\n", a, r, v,
           (s == BUS_OK) ? "OK" : "FAIL", (int)s, (unsigned long)port_diag(g_app.p->i2c_last_error),
           (unsigned long)port_diag(g_app.p->i2c_status_reg));
    }
  } else if (strcmp(tok[0], "pwm") == 0 && ntok == 3u) {
    unsigned long ch = 0;
    unsigned long us = 0;
    if (parse_uint(tok[1], 10, &ch) != 0 || parse_uint(tok[2], 10, &us) != 0 || ch < 1u || ch > 2u ||
        us < MOTION_SERVO_MIN_US || us > MOTION_SERVO_MAX_US) {
      emit("CMD error usage: pwm <1|2> <1000..2000>\r\n");
    } else {
      g_app.p->pwm_set_us((unsigned)ch, (uint16_t)us);
      emit("CMD pwm ch=%lu us=%lu\r\n", ch, us);
    }
  } else if (strcmp(tok[0], "adc") == 0) {
    uint16_t raw = 0;
    g_app.p->marker(1);
    const int rc = g_app.p->adc_read(&raw);
    g_app.p->marker(0);
    emit("CMD adc status=%s raw=%u mv=%lu\r\n", (rc == 0) ? "OK" : "FAIL", (unsigned)raw,
         (unsigned long)motion_adc_to_mv(raw));
  } else if (strcmp(tok[0], "anomaly") == 0) {
    anomaly_demo();
  } else {
    emit("CMD error unknown command, try: help\r\n");
  }
}

static void poll_uart(void) {
  for (;;) {
    const int c = g_app.p->read_char();
    if (c < 0) {
      return;
    }
    if (c == '\r' || c == '\n') {
      g_app.line[g_app.line_len] = '\0';
      if (g_app.line_len > 0u) {
        app_handle_line(g_app.line);
      }
      g_app.line_len = 0;
    } else if (g_app.line_len < sizeof g_app.line - 1u) {
      g_app.line[g_app.line_len++] = (char)c;
    }
  }
}

/* ------------------------------------------------------------------ public */

void app_init(const app_ports_t *ports) {
  memset(&g_app, 0, sizeof g_app);
  g_app.p = ports;
  g_app.gain_pct = 100u;
  mpu6050_bind(&g_app.imu, ports->i2c, MPU6050_ADDR_AD0_LOW);
  w25q_bind(&g_app.flash, ports->spi, ports->millis);
}

void app_boot(void) {
  emit("\r\nhello from STM32 (%s)\r\n", APP_VERSION);
  (void)app_selftest();
  anomaly_demo();
  pwm_demo();
  g_app.last_control_ms = g_app.p->millis();
  g_app.last_telemetry_ms = g_app.last_control_ms;
  emit("READY type 'help' for commands\r\n");
}

void app_poll(void) {
  poll_uart();
  const uint32_t now = g_app.p->millis();
  if ((uint32_t)(now - g_app.last_control_ms) >= CONTROL_PERIOD_MS) {
    g_app.last_control_ms = now;
    control_step();
  }
  if ((uint32_t)(now - g_app.last_telemetry_ms) >= TELEMETRY_PERIOD_MS) {
    g_app.last_telemetry_ms = now;
    g_app.p->led_write(1);
    telemetry();
    g_app.p->led_write(0);
  }
}
