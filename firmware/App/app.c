#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "journal.h"
#include "motion.h"
#include "mpu6050.h"
#include "perf.h"
#include "w25q64.h"

#define APP_VERSION "lr3"
#ifndef GIT_HASH
#define GIT_HASH "dev"
#endif

/* Flash map: sector 0 calibration (reserved), sector 1 scratch (self-test, long-operation generator),
 * sectors 2..5 journal (see journal.h). */
#define SELFTEST_SECTOR_ADDR 0x001000u
#define SELFTEST_RW_ADDR 0x0010F8u /* 16 bytes crossing the page boundary at 0x1100 */
#define SELFTEST_RW_LEN 16u
#define LOP_SECTOR_ADDR 0x001000u

#define ACCEL_NORM_TOLERANCE_PCT 10u
#define ROOM_TEMP_MIN_CDEG 1000
#define ROOM_TEMP_MAX_CDEG 4000

#define LONG_PRESS_MS 1000u
#define RECOVER_PERIOD_MS 1000u
#define EVLOG_MAX 128u
#define SCN_DEFAULT_SEC 30u

/* ------------------------------------------------------------------ configuration */

typedef enum { MODE_LIVE = 0, MODE_HOLD = 1, MODE_ERROR = 2 } app_mode_t;
static const char *const k_mode_name[3] = {"live", "hold", "error"};

typedef struct {
  uint32_t imu_ms; /**< IMU / control period */
  uint32_t tlm_ms; /**< telemetry period, 0 = off */
  uint32_t jrn_ms; /**< flash journal period, 0 = off */
  uint32_t lop_ms; /**< long-operation generator (sector erase + program), 0 = off */
  unsigned stim;   /**< stimulus generator mode 0..2 */
  int btn_irq;     /**< 1 = EXTI, 0 = polling */
} cfg_t;

/* Index 0 is the product default; 1..3 are the mandatory load scenarios S1..S3. */
static const cfg_t k_cfg[4] = {
    /* default */ {20u, 1000u, 0u, 0u, 0u, 1},
    /* S1 idle  */ {20u, 0u, 0u, 0u, 0u, 1},
    /* S2 nominal */ {20u, 1000u, 500u, 0u, 1u, 1},
    /* S3 worst */ {20u, 200u, 100u, 1000u, 2u, 1},
};

typedef enum {
  ST_IMU,
  ST_LOOP,
  ST_CTL,
  ST_TLM,
  ST_JRN,
  ST_LOP,
  ST_CLI,
  ST_REC,
  ST_EVT,
  ST_ISR,
  ST_LOAD,
  ST_COUNT
} stat_id_t;

static const char *const k_stat_name[ST_COUNT] = {"imu",     "loop",  "ctl_lat", "telemetry", "journal", "longop",
                                                  "cli",     "recover", "ev_lat",  "ev_irq",    "load"};

typedef struct {
  uint32_t t_us;   /**< event time since the start of the measurement */
  uint32_t lat_us; /**< edge -> handler */
} evlog_t;

typedef struct {
  const app_ports_t *p;
  mpu6050_t imu;
  w25q_t flash;
  journal_t jrn;
  cfg_t cfg;

  int imu_ready;
  char line[40];
  size_t line_len;
  unsigned pass;
  unsigned fail;

  /* orientation (FR-04, FR-08) */
  int32_t pitch_cdeg; /**< filtered, before calibration offset */
  int32_t roll_cdeg;
  int32_t pitch0_cdeg; /**< calibration offsets */
  int32_t roll0_cdeg;
  int cf_valid;
  uint32_t gain_pct;
  uint16_t adc_raw;
  app_mode_t mode;

  /* schedule */
  uint32_t imu_due;
  uint32_t imu_prev;
  int imu_have_prev;
  uint32_t loop_prev;
  int loop_have_prev;
  uint32_t next_tlm_ms;
  uint32_t next_jrn_ms;
  uint32_t next_lop_ms;
  uint32_t next_led_ms;
  uint32_t next_recover_ms;
  int led;

  /* button (FR-08) */
  int press_active;
  int press_long_done;
  uint32_t press_start_ms;
  uint32_t cal_count;

  /* measurement */
  int meas;
  unsigned scn_id;
  uint32_t scn_end_ms;
  uint32_t scn_t0;
  perf_stat_t st[ST_COUNT];
  uint32_t hist[PERF_HIST_BINS];
  int32_t jit_min;
  int32_t jit_max;
  uint32_t imu_missed;
  uint32_t busy_cyc;
  uint32_t load_start;
  evlog_t ev[EVLOG_MAX];
  uint32_t ev_n;
  uint32_t cmds;
  uint32_t flash_err;
} app_t;

static app_t g_app;

/* ------------------------------------------------------------------ helpers */

static uint32_t cyc(void) {
  return g_app.p->cycles();
}

static uint32_t to_us(uint32_t cycles) {
  return cycles / g_app.p->cycles_per_us;
}

static void probe(unsigned ch, int level) {
  if (g_app.p->probe != NULL) {
    g_app.p->probe(ch, level);
  }
}

static void stat_add(stat_id_t id, uint32_t v) {
  if (g_app.meas != 0) {
    perf_stat_add(&g_app.st[id], v);
  }
}

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
  g_app.mode = g_app.imu_ready ? MODE_LIVE : MODE_ERROR;
  g_app.next_recover_ms = g_app.p->millis() + RECOVER_PERIOD_MS;
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

/* ------------------------------------------------------------------ tasks of the super loop */

static void set_pwm_centre(void) {
  g_app.p->pwm_set_us(1, MOTION_SERVO_CENTER_US);
  g_app.p->pwm_set_us(2, MOTION_SERVO_CENTER_US);
}

/* FR-11: the sensor stopped answering -> centre both servos, report, retry later. */
static void imu_lost(void) {
  g_app.imu_ready = 0;
  g_app.mode = MODE_ERROR;
  g_app.cf_valid = 0;
  set_pwm_centre();
  g_app.next_recover_ms = g_app.p->millis() + RECOVER_PERIOD_MS;
  emit("ERROR mpu6050 lost, servos centred\r\n");
}

/* Re-probe the sensor once per second while it is missing. The attempt blocks for up to the
 * I2C timeout (BOARD_I2C_TIMEOUT_MS) when the bus is dead: measured as "recover". */
static int recover_task(void) {
  if (g_app.imu_ready != 0 || g_app.mode != MODE_ERROR) {
    return 0;
  }
  const uint32_t ms = g_app.p->millis();
  if ((int32_t)(ms - g_app.next_recover_ms) < 0) {
    return 0;
  }
  g_app.next_recover_ms = ms + RECOVER_PERIOD_MS;
  const uint32_t t0 = cyc();
  uint8_t who = 0;
  if (mpu6050_check_id(&g_app.imu, &who) == MPU6050_OK &&
      mpu6050_init(&g_app.imu, MPU6050_ACCEL_2G, MPU6050_GYRO_250DPS) == MPU6050_OK) {
    g_app.imu_ready = 1;
    g_app.mode = MODE_LIVE;
    emit("INFO mpu6050 recovered\r\n");
  }
  stat_add(ST_REC, to_us(cyc() - t0));
  return 1;
}

static void imu_step(uint32_t planned) {
  uint16_t raw_adc = 0;
  g_app.p->marker(1);
  const int rc = g_app.p->adc_read(&raw_adc);
  g_app.p->marker(0);
  if (rc == 0) {
    g_app.adc_raw = raw_adc;
    g_app.gain_pct = motion_gain_pct(raw_adc);
  }
  mpu6050_raw_t r;
  mpu6050_sample_t s;
  if (mpu6050_read_raw(&g_app.imu, &r) != MPU6050_OK) {
    imu_lost();
    return;
  }
  mpu6050_convert(&g_app.imu, &r, &s);
  const int32_t acc_pitch = motion_pitch_cdeg(s.ax_mg, s.ay_mg, s.az_mg);
  const int32_t acc_roll = motion_roll_cdeg(s.ay_mg, s.az_mg);
  const uint32_t now = cyc();
  if (g_app.cf_valid == 0) {
    g_app.pitch_cdeg = acc_pitch;
    g_app.roll_cdeg = acc_roll;
    g_app.cf_valid = 1;
  } else {
    const uint32_t dt_ms = (to_us(now - g_app.imu_prev) + 500u) / 1000u;
    g_app.pitch_cdeg = motion_cf_update(g_app.pitch_cdeg, s.gy_mdps, dt_ms, acc_pitch);
    g_app.roll_cdeg = motion_cf_update(g_app.roll_cdeg, s.gx_mdps, dt_ms, acc_roll);
  }
  if (g_app.mode == MODE_LIVE) {
    g_app.p->pwm_set_us(1, motion_servo_pulse_us(motion_wrap_diff_cdeg(g_app.pitch_cdeg, g_app.pitch0_cdeg),
                                                 g_app.gain_pct));
    g_app.p->pwm_set_us(2, motion_servo_pulse_us(motion_wrap_diff_cdeg(g_app.roll_cdeg, g_app.roll0_cdeg),
                                                 g_app.gain_pct));
  }
  stat_add(ST_CTL, to_us(cyc() - planned));
}

/* Periodic control task (FR-03..FR-07, FR-11), time-tagged in CPU cycles so that jitter and
 * skipped periods can be measured. */
static int imu_task(void) {
  if (g_app.imu_ready == 0) {
    return 0;
  }
  const uint32_t now = cyc();
  if ((int32_t)(now - g_app.imu_due) < 0) {
    return 0;
  }
  const uint32_t period = g_app.cfg.imu_ms * 1000u * g_app.p->cycles_per_us;
  const uint32_t late = now - g_app.imu_due;
  if (late >= period) { /* whole periods that were skipped: lost samples */
    const uint32_t skipped = late / period;
    if (g_app.meas != 0) {
      g_app.imu_missed += skipped;
    }
    g_app.imu_due += skipped * period;
  }
  const uint32_t planned = g_app.imu_due;
  g_app.imu_due += period;

  if (g_app.imu_have_prev != 0) {
    const int32_t jit = (int32_t)to_us(now - g_app.imu_prev) - (int32_t)(g_app.cfg.imu_ms * 1000u);
    if (g_app.meas != 0) {
      const uint32_t a = (uint32_t)((jit < 0) ? -jit : jit);
      g_app.hist[perf_hist_bin(a)]++;
      if (g_app.st[ST_IMU].count == 0u || jit < g_app.jit_min) {
        g_app.jit_min = jit;
      }
      if (g_app.st[ST_IMU].count == 0u || jit > g_app.jit_max) {
        g_app.jit_max = jit;
      }
    }
  }
  g_app.imu_have_prev = 1;

  probe(0, 1);
  imu_step(planned);
  probe(0, 0);
  g_app.imu_prev = now;
  stat_add(ST_IMU, to_us(cyc() - now));
  return 1;
}

static int led_task(void) {
  if (g_app.mode == MODE_HOLD) { /* FR-09: solid on */
    g_app.led = 1;
    g_app.p->led_write(1);
    return 0;
  }
  const uint32_t ms = g_app.p->millis();
  if ((int32_t)(ms - g_app.next_led_ms) < 0) {
    return 0;
  }
  g_app.next_led_ms = ms + ((g_app.mode == MODE_ERROR) ? 100u : 500u); /* 5 Hz error, 1 Hz live */
  g_app.led = !g_app.led;
  g_app.p->led_write(g_app.led);
  return 1;
}

static void telemetry(void) {
  emit("TLM t_ms=%lu pitch_cdeg=%ld roll_cdeg=%ld adc=%u gain_pct=%lu btn=%d mode=%s p1_us=%u p2_us=%u\r\n",
       (unsigned long)g_app.p->millis(), (long)(g_app.pitch_cdeg - g_app.pitch0_cdeg),
       (long)(g_app.roll_cdeg - g_app.roll0_cdeg), (unsigned)g_app.adc_raw, (unsigned long)g_app.gain_pct,
       g_app.p->button_pressed() ? 1 : 0, k_mode_name[g_app.mode], (unsigned)g_app.p->pwm_get_us(1),
       (unsigned)g_app.p->pwm_get_us(2));
}

static int tlm_task(void) {
  if (g_app.cfg.tlm_ms == 0u) {
    return 0;
  }
  const uint32_t ms = g_app.p->millis();
  if ((int32_t)(ms - g_app.next_tlm_ms) < 0) {
    return 0;
  }
  g_app.next_tlm_ms = ms + g_app.cfg.tlm_ms;
  const uint32_t t0 = cyc();
  telemetry();
  stat_add(ST_TLM, to_us(cyc() - t0));
  return 1;
}

/* Button events (FR-08): the press edge calibrates (current orientation becomes zero) at once;
 * holding the button longer than LONG_PRESS_MS toggles Live / Hold. */
static void record_event(const btn_event_t *ev, uint32_t now) {
  const uint32_t lat = to_us(now - ev->t_edge);
  stat_add(ST_EVT, lat);
  stat_add(ST_ISR, to_us(ev->t_isr - ev->t_edge));
  if (g_app.meas != 0 && g_app.ev_n < EVLOG_MAX) {
    g_app.ev[g_app.ev_n].t_us = to_us(ev->t_edge - g_app.scn_t0);
    g_app.ev[g_app.ev_n].lat_us = lat;
  }
  if (g_app.meas != 0) {
    g_app.ev_n++;
  }
}

static int event_task(void) {
  int worked = 0;
  btn_event_t ev;
  if (g_app.p->button_event != NULL && g_app.p->button_event(&ev) != 0) {
    const uint32_t now = cyc();
    probe(2, 1); /* reaction starts: the logic analyzer measures edge -> this pulse */
    g_app.pitch0_cdeg = g_app.pitch_cdeg;
    g_app.roll0_cdeg = g_app.roll_cdeg;
    g_app.cal_count++;
    if (g_app.mode == MODE_LIVE) {
      set_pwm_centre();
    }
    probe(2, 0);
    record_event(&ev, now);
    g_app.press_active = 1;
    g_app.press_long_done = 0;
    g_app.press_start_ms = g_app.p->millis();
    worked = 1;
  }
  if (g_app.press_active != 0) {
    if (g_app.p->button_pressed() == 0) {
      g_app.press_active = 0;
    } else if (g_app.press_long_done == 0 && (g_app.p->millis() - g_app.press_start_ms) >= LONG_PRESS_MS) {
      g_app.press_long_done = 1;
      if (g_app.mode != MODE_ERROR) {
        g_app.mode = (g_app.mode == MODE_LIVE) ? MODE_HOLD : MODE_LIVE;
        emit("MODE %s\r\n", k_mode_name[g_app.mode]);
      }
      worked = 1;
    }
  }
  return worked;
}

static int jrn_task(void) {
  if (g_app.cfg.jrn_ms == 0u) {
    return 0;
  }
  const uint32_t ms = g_app.p->millis();
  if ((int32_t)(ms - g_app.next_jrn_ms) < 0) {
    return 0;
  }
  g_app.next_jrn_ms = ms + g_app.cfg.jrn_ms;
  journal_entry_t e;
  e.seq = g_app.jrn.seq;
  e.t_ms = ms;
  e.pitch_cdeg = (int16_t)(g_app.pitch_cdeg - g_app.pitch0_cdeg);
  e.roll_cdeg = (int16_t)(g_app.roll_cdeg - g_app.roll0_cdeg);
  e.mode = (uint8_t)g_app.mode;
  e.flags = 0;
  int erased = 0;
  probe(1, 1);
  const uint32_t t0 = cyc();
  if (journal_append(&g_app.jrn, &e, &erased) != W25Q_OK) {
    g_app.flash_err++;
  }
  const uint32_t dt = cyc() - t0;
  probe(1, 0);
  stat_add(ST_JRN, to_us(dt));
  return 1;
}

/* Long-operation generator for scenario S3: rewrite a configuration sector (erase + program),
 * the way a product that stores settings in flash would. tSE typ. 45 ms blocks the loop. */
static int lop_task(void) {
  if (g_app.cfg.lop_ms == 0u) {
    return 0;
  }
  const uint32_t ms = g_app.p->millis();
  if ((int32_t)(ms - g_app.next_lop_ms) < 0) {
    return 0;
  }
  g_app.next_lop_ms = ms + g_app.cfg.lop_ms;
  uint8_t blob[16];
  memcpy(blob, "LR3-LONGOP-BLOB!", sizeof blob);
  probe(1, 1);
  const uint32_t t0 = cyc();
  if (w25q_sector_erase(&g_app.flash, LOP_SECTOR_ADDR) != W25Q_OK ||
      w25q_write(&g_app.flash, LOP_SECTOR_ADDR, blob, sizeof blob) != W25Q_OK) {
    g_app.flash_err++;
  }
  const uint32_t dt = cyc() - t0;
  probe(1, 0);
  stat_add(ST_LOP, to_us(dt));
  return 1;
}

/* ------------------------------------------------------------------ measurement reports */

static void print_stat(stat_id_t id) {
  const perf_stat_t *s = &g_app.st[id];
  const char *u = (id == ST_LOAD) ? "pct" : "us";
  emit("STAT name=%s n=%lu min_%s=%lu avg_%s=%lu max_%s=%lu\r\n", k_stat_name[id], (unsigned long)s->count, u,
       (unsigned long)s->min, u, (unsigned long)perf_stat_avg(s), u, (unsigned long)s->max);
}

static void print_all_stats(void) {
  for (unsigned i = 0; i < (unsigned)ST_COUNT; i++) {
    if (g_app.st[i].count != 0u) {
      print_stat((stat_id_t)i);
    }
  }
}

static void print_jitter(void) {
  emit("JIT period_us=%lu min_us=%ld max_us=%ld missed=%lu hist=%lu,%lu,%lu,%lu,%lu,%lu\r\n",
       (unsigned long)(g_app.cfg.imu_ms * 1000u), (long)g_app.jit_min, (long)g_app.jit_max,
       (unsigned long)g_app.imu_missed, (unsigned long)g_app.hist[0], (unsigned long)g_app.hist[1],
       (unsigned long)g_app.hist[2], (unsigned long)g_app.hist[3], (unsigned long)g_app.hist[4],
       (unsigned long)g_app.hist[5]);
}

static void print_mem(void) {
  const app_ports_t *p = g_app.p;
  emit("MEM stack_used=%lu stack_size=%lu heap_used=%lu\r\n",
       (unsigned long)((p->stack_used != NULL) ? p->stack_used() : 0u),
       (unsigned long)((p->stack_size != NULL) ? p->stack_size() : 0u),
       (unsigned long)((p->heap_used != NULL) ? p->heap_used() : 0u));
}

static void print_rx(void) {
  uint32_t bytes = 0;
  uint32_t ore = 0;
  if (g_app.p->uart_rx_stats != NULL) {
    g_app.p->uart_rx_stats(&bytes, &ore);
  }
  emit("RX bytes=%lu overruns=%lu cmds=%lu\r\n", (unsigned long)bytes, (unsigned long)ore,
       (unsigned long)g_app.cmds);
}

static void print_evlog(void) {
  const uint32_t n = (g_app.ev_n < EVLOG_MAX) ? g_app.ev_n : EVLOG_MAX;
  emit("EVLOG total=%lu stored=%lu\r\n", (unsigned long)g_app.ev_n, (unsigned long)n);
  for (uint32_t i = 0; i < n; i++) {
    emit("EVT n=%lu t_us=%lu lat_us=%lu\r\n", (unsigned long)(i + 1u), (unsigned long)g_app.ev[i].t_us,
         (unsigned long)g_app.ev[i].lat_us);
  }
}

static void print_journal_check(void) {
  journal_check_t c;
  const w25q_status_t st = journal_verify(&g_app.jrn, &c);
  emit("JOURNAL status=%s good=%lu bad=%lu blank=%lu erases=%lu errors=%lu next=0x%06lX\r\n",
       (st == W25Q_OK) ? "OK" : "FAIL", (unsigned long)c.good, (unsigned long)c.bad, (unsigned long)c.blank,
       (unsigned long)g_app.jrn.erases, (unsigned long)g_app.jrn.errors, (unsigned long)g_app.jrn.next);
}

static void apply_cfg(const cfg_t *c) {
  g_app.cfg = *c;
  if (g_app.p->btn_mode_set != NULL) {
    g_app.p->btn_mode_set(c->btn_irq);
  }
  if (g_app.p->stim_set != NULL) {
    g_app.p->stim_set(c->stim);
  }
  const uint32_t ms = g_app.p->millis();
  g_app.next_tlm_ms = ms + c->tlm_ms;
  g_app.next_jrn_ms = ms + c->jrn_ms;
  g_app.next_lop_ms = ms + c->lop_ms;
  g_app.imu_due = cyc() + c->imu_ms * 1000u * g_app.p->cycles_per_us;
  g_app.imu_have_prev = 0;
}

static void meas_reset(void) {
  for (unsigned i = 0; i < (unsigned)ST_COUNT; i++) {
    perf_stat_reset(&g_app.st[i]);
  }
  memset(g_app.hist, 0, sizeof g_app.hist);
  g_app.jit_min = 0;
  g_app.jit_max = 0;
  g_app.imu_missed = 0;
  g_app.ev_n = 0;
  g_app.cmds = 0;
  g_app.busy_cyc = 0;
  g_app.load_start = cyc();
  g_app.loop_have_prev = 0;
  g_app.scn_t0 = cyc();
}

static void scenario_begin(unsigned id, uint32_t sec) {
  if (id >= 1u) {
    apply_cfg(&k_cfg[id]);
  } else {
    apply_cfg(&g_app.cfg); /* "scn 0": keep the settings made with `set` */
  }
  meas_reset();
  g_app.scn_id = id;
  g_app.scn_end_ms = g_app.p->millis() + sec * 1000u;
  g_app.meas = 1;
  emit("SCN BEGIN n=%u sec=%lu version=%s git=%s imu_ms=%lu tlm_ms=%lu jrn_ms=%lu lop_ms=%lu stim=%u btn=%s\r\n", id,
       (unsigned long)sec, APP_VERSION, GIT_HASH, (unsigned long)g_app.cfg.imu_ms,
       (unsigned long)g_app.cfg.tlm_ms, (unsigned long)g_app.cfg.jrn_ms, (unsigned long)g_app.cfg.lop_ms,
       g_app.cfg.stim, g_app.cfg.btn_irq ? "irq" : "poll");
}

static void scenario_end(void) {
  g_app.meas = 0; /* freeze the numbers before the (long) report is printed */
  if (g_app.p->stim_set != NULL) {
    g_app.p->stim_set(0);
  }
  emit("SCN END n=%u t_ms=%lu\r\n", g_app.scn_id, (unsigned long)g_app.p->millis());
  print_all_stats();
  print_jitter();
  print_mem();
  print_rx();
  print_evlog();
  emit("SCN DONE n=%u\r\n", g_app.scn_id);
  g_app.scn_id = 0;
  apply_cfg(&k_cfg[0]);
}

static void load_window(void) {
  const uint32_t now = cyc();
  const uint32_t window = now - g_app.load_start;
  const uint32_t one_s = g_app.p->cycles_per_us * 1000000u;
  if (window >= one_s) {
    stat_add(ST_LOAD, g_app.busy_cyc / (window / 100u));
    g_app.busy_cyc = 0;
    g_app.load_start = now;
  }
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
  emit("CMD help | selftest | whoami | jedec | probe <hex addr7> | reg <addr7> <reg> [n] | regw <addr7> <reg> <val> | "
       "pwm <1|2> <us> | adc | anomaly\r\n");
  emit("CMD lr3: scn <0..3> [sec] | set <imu|tlm|jrn|lop|stim|btn> <v> | stats | jit | mem | rx | evlog | "
       "press <ms> | jv | jtorn | ping | reboot\r\n");
}

static void cmd_set(const char *key, const char *val) {
  unsigned long v = 0;
  cfg_t *c = &g_app.cfg;
  if (strcmp(key, "btn") == 0) {
    if (strcmp(val, "irq") != 0 && strcmp(val, "poll") != 0) {
      emit("CMD error usage: set btn <irq|poll>\r\n");
      return;
    }
    c->btn_irq = (strcmp(val, "irq") == 0);
  } else if (parse_uint(val, 10, &v) != 0) {
    emit("CMD error usage: set <imu|tlm|jrn|lop|stim|btn> <value>\r\n");
    return;
  } else if (strcmp(key, "imu") == 0 && v >= 1u && v <= 1000u) {
    c->imu_ms = (uint32_t)v;
  } else if (strcmp(key, "tlm") == 0 && v <= 60000u) {
    c->tlm_ms = (uint32_t)v;
  } else if (strcmp(key, "jrn") == 0 && v <= 60000u) {
    c->jrn_ms = (uint32_t)v;
  } else if (strcmp(key, "lop") == 0 && v <= 60000u) {
    c->lop_ms = (uint32_t)v;
  } else if (strcmp(key, "stim") == 0 && v <= 2u) {
    c->stim = (unsigned)v;
  } else {
    emit("CMD error usage: set <imu|tlm|jrn|lop|stim|btn> <value>\r\n");
    return;
  }
  apply_cfg(c);
  emit("CMD set imu_ms=%lu tlm_ms=%lu jrn_ms=%lu lop_ms=%lu stim=%u btn=%s\r\n", (unsigned long)c->imu_ms,
       (unsigned long)c->tlm_ms, (unsigned long)c->jrn_ms, (unsigned long)c->lop_ms, c->stim,
       c->btn_irq ? "irq" : "poll");
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
  } else if (strcmp(tok[0], "scn") == 0 && ntok >= 2u) {
    unsigned long id = 0;
    unsigned long sec = SCN_DEFAULT_SEC;
    if (parse_uint(tok[1], 10, &id) != 0 || id > 3u || (ntok == 3u && (parse_uint(tok[2], 10, &sec) != 0 || sec < 1u))) {
      emit("CMD error usage: scn <0..3> [seconds]\r\n");
    } else {
      scenario_begin((unsigned)id, (uint32_t)sec);
    }
  } else if (strcmp(tok[0], "set") == 0 && ntok == 3u) {
    cmd_set(tok[1], tok[2]);
  } else if (strcmp(tok[0], "stats") == 0) {
    print_all_stats();
  } else if (strcmp(tok[0], "jit") == 0) {
    print_jitter();
  } else if (strcmp(tok[0], "mem") == 0) {
    print_mem();
  } else if (strcmp(tok[0], "rx") == 0) {
    print_rx();
  } else if (strcmp(tok[0], "evlog") == 0) {
    print_evlog();
  } else if (strcmp(tok[0], "press") == 0 && ntok == 2u) {
    unsigned long ms = 0;
    if (parse_uint(tok[1], 10, &ms) != 0 || ms < 1u || ms > 5000u || g_app.p->stim_press == NULL) {
      emit("CMD error usage: press <1..5000 ms>\r\n");
    } else {
      g_app.p->stim_press((uint32_t)ms);
      emit("CMD press ms=%lu\r\n", ms);
    }
  } else if (strcmp(tok[0], "jv") == 0) {
    print_journal_check();
  } else if (strcmp(tok[0], "jtorn") == 0) {
    journal_entry_t e;
    e.seq = g_app.jrn.seq;
    e.t_ms = g_app.p->millis();
    e.pitch_cdeg = 0;
    e.roll_cdeg = 0;
    e.mode = (uint8_t)g_app.mode;
    e.flags = 0xA5u;
    emit("CMD jtorn status=%s\r\n", (journal_append_torn(&g_app.jrn, &e) == W25Q_OK) ? "OK" : "FAIL");
  } else if (strcmp(tok[0], "ping") == 0) {
    emit("PONG n=%lu\r\n", (unsigned long)g_app.cmds);
  } else if (strcmp(tok[0], "reboot") == 0 && g_app.p->reboot != NULL) {
    emit("CMD reboot\r\n");
    g_app.p->reboot();
  } else {
    emit("CMD error unknown command, try: help\r\n");
  }
}

static int cli_task(void) {
  int worked = 0;
  for (;;) {
    const int c = g_app.p->read_char();
    if (c < 0) {
      return worked;
    }
    worked = 1;
    if (c == '\r' || c == '\n') {
      g_app.line[g_app.line_len] = '\0';
      if (g_app.line_len > 0u) {
        const uint32_t t0 = cyc();
        g_app.cmds++;
        app_handle_line(g_app.line);
        stat_add(ST_CLI, to_us(cyc() - t0));
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
  g_app.cfg = k_cfg[0];
  g_app.next_tlm_ms = k_cfg[0].tlm_ms;
  for (unsigned i = 0; i < (unsigned)ST_COUNT; i++) {
    perf_stat_init(&g_app.st[i], k_stat_name[i]);
  }
  mpu6050_bind(&g_app.imu, ports->i2c, MPU6050_ADDR_AD0_LOW);
  w25q_bind(&g_app.flash, ports->spi, ports->millis);
  journal_init(&g_app.jrn, &g_app.flash);
}

void app_boot(void) {
  emit("\r\nhello from STM32 (%s) git=%s\r\n", APP_VERSION, GIT_HASH);
  (void)app_selftest();
  anomaly_demo();
  pwm_demo();
  apply_cfg(&k_cfg[0]);
  g_app.imu_due = cyc();
  if (g_app.mode == MODE_ERROR) {
    g_app.next_recover_ms = g_app.p->millis() + RECOVER_PERIOD_MS;
  }
  emit("READY type 'help' for commands\r\n");
}

/* The super loop body. Order matters: every task that runs before another one adds its full
 * duration to the other's reaction time. Commands and button events come first because they are
 * the least predictable, the periodic tasks follow, the slow operations are last. */
#define RUN(task)                      \
  do {                                 \
    const uint32_t t0_ = cyc();        \
    if ((task)() != 0) {               \
      g_app.busy_cyc += cyc() - t0_;   \
    }                                  \
  } while (0)

void app_poll(void) {
  const uint32_t t_iter = cyc();
  if (g_app.loop_have_prev != 0) {
    stat_add(ST_LOOP, to_us(t_iter - g_app.loop_prev));
  }
  g_app.loop_prev = t_iter;
  g_app.loop_have_prev = 1;

  RUN(cli_task);
  RUN(event_task);
  RUN(imu_task);
  RUN(led_task);
  RUN(tlm_task);
  RUN(jrn_task);
  RUN(lop_task);
  RUN(recover_task);
  load_window();

  if (g_app.scn_id != 0u || g_app.meas != 0) {
    if ((int32_t)(g_app.p->millis() - g_app.scn_end_ms) >= 0) {
      scenario_end();
    }
  }
}
