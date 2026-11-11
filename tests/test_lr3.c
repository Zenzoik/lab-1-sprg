/* LR3: measurement module, journal, complementary filter, button handling, scenarios. */
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "crc16.h"
#include "fakes.h"
#include "journal.h"
#include "motion.h"
#include "perf.h"
#include "test.h"

static fake_i2c_t g_i2c;
static fake_spi_t g_spi;
static fake_board_t g_board;

static const uint8_t k_burst_flat[14] = {0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0xEF,
                                         0x5C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
/* 30 deg roll: ay = 500 mg, az = 866 mg */
static const uint8_t k_burst_roll30[14] = {0x00, 0x00, 0x20, 0x00, 0x37, 0x6D, 0xEF,
                                           0x5C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static void setup(void) {
  fake_i2c_init(&g_i2c, 0x68);
  fake_i2c_set_burst(&g_i2c, k_burst_flat);
  fake_spi_init(&g_spi);
  fake_board_init(&g_board, &g_i2c, &g_spi);
  g_board.tick_ms = 0; /* tests drive the virtual clock themselves */
  app_init(&g_board.ports);
  (void)app_selftest();
  fake_board_clear_console(&g_board);
}

static void run_ms(unsigned ms) {
  for (unsigned i = 0; i < ms; i++) {
    g_board.now_ms++;
    app_poll();
  }
}

static int count_of(const char *text, const char *needle) {
  int n = 0;
  for (const char *p = text; (p = strstr(p, needle)) != NULL; p += strlen(needle)) {
    n++;
  }
  return n;
}

/* ---- perf module ------------------------------------------------------- */

static void test_perf_stat_min_avg_max(void) {
  perf_stat_t s;
  perf_stat_init(&s, "x");
  ASSERT_EQ_INT(0, perf_stat_avg(&s));
  perf_stat_add(&s, 10);
  perf_stat_add(&s, 30);
  perf_stat_add(&s, 20);
  ASSERT_EQ_INT(3, s.count);
  ASSERT_EQ_INT(10, s.min);
  ASSERT_EQ_INT(30, s.max);
  ASSERT_EQ_INT(20, perf_stat_avg(&s));
  perf_stat_reset(&s);
  ASSERT_EQ_INT(0, s.count);
  perf_stat_add(&s, 7);
  ASSERT_EQ_INT(7, s.min); /* min is not stuck at the value from before the reset */
}

static void test_perf_stat_avg_rounds_and_does_not_overflow(void) {
  perf_stat_t s;
  perf_stat_init(&s, "x");
  perf_stat_add(&s, 1);
  perf_stat_add(&s, 2);
  ASSERT_EQ_INT(2, perf_stat_avg(&s)); /* 1.5 -> 2 */
  perf_stat_reset(&s);
  for (int i = 0; i < 4; i++) {
    perf_stat_add(&s, 0xFFFFFFFFu); /* 64-bit sum */
  }
  ASSERT_EQ_INT(0xFFFFFFFFu, perf_stat_avg(&s));
}

static void test_perf_hist_bins(void) {
  ASSERT_EQ_INT(0, perf_hist_bin(0));
  ASSERT_EQ_INT(0, perf_hist_bin(100));
  ASSERT_EQ_INT(1, perf_hist_bin(101));
  ASSERT_EQ_INT(1, perf_hist_bin(500));
  ASSERT_EQ_INT(2, perf_hist_bin(1000));
  ASSERT_EQ_INT(3, perf_hist_bin(5000));
  ASSERT_EQ_INT(4, perf_hist_bin(20000));
  ASSERT_EQ_INT(5, perf_hist_bin(20001));
}

/* ---- crc16 and journal ------------------------------------------------- */

static void test_crc16_check_value(void) {
  ASSERT_EQ_INT(0x29B1, crc16_ccitt((const uint8_t *)"123456789", 9));
  ASSERT_EQ_INT(0xFFFF, crc16_ccitt((const uint8_t *)"", 0));
}

static void test_journal_record_roundtrip_and_corruption(void) {
  const journal_entry_t e = {.seq = 0x01020304u, .t_ms = 123456u, .pitch_cdeg = -1234, .roll_cdeg = 4321,
                             .mode = 1, .flags = 0xA5};
  uint8_t rec[JOURNAL_REC_SIZE];
  journal_encode(&e, rec);
  journal_entry_t d;
  ASSERT_EQ_INT(1, journal_decode(rec, &d));
  ASSERT_EQ_INT(e.seq, d.seq);
  ASSERT_EQ_INT(e.t_ms, d.t_ms);
  ASSERT_EQ_INT(-1234, d.pitch_cdeg);
  ASSERT_EQ_INT(4321, d.roll_cdeg);
  ASSERT_EQ_INT(0xA5, d.flags);
  rec[5] ^= 0x01u; /* one flipped bit */
  ASSERT_EQ_INT(0, journal_decode(rec, &d));
}

static void journal_setup(w25q_t *f, journal_t *j) {
  setup();
  w25q_bind(f, g_board.ports.spi, g_board.ports.millis);
  journal_init(j, f);
}

static void append_n(journal_t *j, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    journal_entry_t e = {.seq = j->seq, .t_ms = i, .pitch_cdeg = 0, .roll_cdeg = 0, .mode = 0, .flags = 0};
    int erased = 0;
    (void)journal_append(j, &e, &erased);
  }
}

static void test_journal_append_erases_per_sector_and_verifies(void) {
  w25q_t f;
  journal_t j;
  journal_setup(&f, &j);
  append_n(&j, 300); /* 256 records per 4 KiB sector */
  ASSERT_EQ_INT(2, j.erases);
  journal_check_t c;
  ASSERT_EQ_INT(W25Q_OK, journal_verify(&j, &c));
  ASSERT_EQ_INT(300, c.good);
  ASSERT_EQ_INT(0, c.bad);
  ASSERT_EQ_INT(300, j.seq);
}

static void test_journal_wraps_after_last_sector(void) {
  w25q_t f;
  journal_t j;
  journal_setup(&f, &j);
  append_n(&j, JOURNAL_SECTORS * 256u + 10u);
  ASSERT_EQ_INT(1, j.wrapped);
  ASSERT_EQ_INT(JOURNAL_SECTORS + 1u, j.erases);
  journal_check_t c;
  ASSERT_EQ_INT(W25Q_OK, journal_verify(&j, &c));
  ASSERT_EQ_INT(JOURNAL_SECTORS * 256u, c.good + c.bad + c.blank);
  ASSERT_EQ_INT(0, c.bad);
}

static void test_journal_torn_write_is_detected(void) {
  w25q_t f;
  journal_t j;
  journal_setup(&f, &j);
  append_n(&j, 5);
  const journal_entry_t e = {.seq = j.seq, .t_ms = 99, .pitch_cdeg = 1, .roll_cdeg = 2, .mode = 0, .flags = 0};
  ASSERT_EQ_INT(W25Q_OK, journal_append_torn(&j, &e));
  journal_check_t c;
  ASSERT_EQ_INT(W25Q_OK, journal_verify(&j, &c));
  ASSERT_EQ_INT(5, c.good);
  ASSERT_EQ_INT(1, c.bad); /* the half-written record fails its CRC */
}

static void test_journal_flash_error_is_counted(void) {
  w25q_t f;
  journal_t j;
  journal_setup(&f, &j);
  g_spi.wp_stuck = 1;
  const journal_entry_t e = {0};
  int erased = 0;
  ASSERT_TRUE(journal_append(&j, &e, &erased) != W25Q_OK);
  ASSERT_EQ_INT(1, j.errors);
  ASSERT_EQ_INT(0, j.seq); /* the sequence number is not consumed by a failed write */
}

/* ---- complementary filter ---------------------------------------------- */

static void test_cf_converges_to_accelerometer(void) {
  /* no gyro, accel says 10 deg, filter starts at 0: new = 10 + 0.98 * (0 - 10) = 0.2 deg */
  ASSERT_EQ_INT(20, motion_cf_update(0, 0, 20, 1000));
  int32_t a = 0;
  for (int i = 0; i < 200; i++) {
    a = motion_cf_update(a, 0, 20, 1000);
  }
  ASSERT_EQ_INT(1000, a); /* with every step the estimate moves a bit closer */
}

static void test_cf_integrates_gyro_rate(void) {
  /* 100 dps for 20 ms = 2 deg, accel agrees (200 cdeg): the estimate follows exactly */
  ASSERT_EQ_INT(200, motion_cf_update(0, 100000, 20, 200));
}

static void test_cf_wraps_around_180_degrees(void) {
  /* prev 179 deg, +2 deg of rotation = -179 deg after the wrap, accel agrees */
  ASSERT_EQ_INT(-17900, motion_cf_update(17900, 100000, 20, -17900));
  ASSERT_EQ_INT(-9000, motion_wrap_diff_cdeg(27000, 0));
  ASSERT_EQ_INT(18000, motion_wrap_diff_cdeg(-18000, 0));
}

/* ---- scenarios and measurement ---------------------------------------- */

static void test_scenario_s1_reports_all_metric_families(void) {
  setup();
  app_handle_line("scn 1 3");
  run_ms(3100);
  const char *o = g_board.console;
  ASSERT_STR_CONTAINS(o, "SCN BEGIN n=1 sec=3 version=lr3");
  ASSERT_STR_CONTAINS(o, "imu_ms=20 tlm_ms=0 jrn_ms=0 lop_ms=0 stim=0 btn=irq");
  ASSERT_STR_CONTAINS(o, "SCN END n=1");
  ASSERT_STR_CONTAINS(o, "STAT name=imu n=");
  ASSERT_STR_CONTAINS(o, "STAT name=loop n=");
  ASSERT_STR_CONTAINS(o, "STAT name=ctl_lat n=");
  ASSERT_STR_CONTAINS(o, "STAT name=load n=");
  ASSERT_STR_CONTAINS(o, "JIT period_us=20000 min_us=0 max_us=0 missed=0");
  ASSERT_STR_CONTAINS(o, "MEM stack_used=");
  ASSERT_STR_CONTAINS(o, "RX bytes=");
  ASSERT_STR_CONTAINS(o, "EVLOG total=0 stored=0");
  ASSERT_STR_CONTAINS(o, "SCN DONE n=1");
  ASSERT_EQ_INT(0, count_of(o, "TLM t_ms=")); /* S1 has no telemetry */
}

static void test_scenario_s1_imu_rate_is_50_hz(void) {
  setup();
  app_handle_line("scn 1 3");
  run_ms(3100);
  const char *p = strstr(g_board.console, "STAT name=imu n=");
  ASSERT_TRUE(p != NULL);
  const int n = atoi(p + strlen("STAT name=imu n="));
  ASSERT_NEAR(150, n, 2);
}

static void test_scenario_restores_default_configuration(void) {
  setup();
  app_handle_line("scn 3 1");
  ASSERT_EQ_INT(2, g_board.stim_mode);
  run_ms(1100);
  ASSERT_EQ_INT(0, g_board.stim_mode); /* generator off after the run */
  fake_board_clear_console(&g_board);
  run_ms(1500);
  ASSERT_TRUE(count_of(g_board.console, "TLM t_ms=") >= 1); /* default telemetry is back */
}

static void test_missed_periods_and_jitter_after_blocking(void) {
  setup();
  app_handle_line("scn 1 5");
  run_ms(25);                /* first sample at +20 ms, next one due at +40 ms */
  g_board.now_ms += 100;     /* the loop was blocked for 100 ms (long operation) */
  run_ms(5100);
  const char *o = g_board.console;
  ASSERT_STR_CONTAINS(o, "missed=4");
  ASSERT_STR_CONTAINS(o, "max_us=86000"); /* the late sample came 106 ms after its predecessor */
}

static void test_event_latency_is_edge_to_handler(void) {
  setup();
  app_handle_line("scn 2 2");
  const uint32_t t = g_board.now_ms * 48000u;
  fake_board_push_event(&g_board, t, t + 48u); /* edge now, ISR 1 us later */
  run_ms(2100);
  const char *o = g_board.console;
  ASSERT_STR_CONTAINS(o, "STAT name=ev_lat n=1 min_us=1000 avg_us=1000 max_us=1000");
  ASSERT_STR_CONTAINS(o, "STAT name=ev_irq n=1 min_us=1 avg_us=1 max_us=1");
  ASSERT_STR_CONTAINS(o, "EVLOG total=1 stored=1");
  ASSERT_STR_CONTAINS(o, "EVT n=1 t_us=0 lat_us=1000");
  ASSERT_EQ_INT(0, g_board.probe_level[2]);
  ASSERT_EQ_INT(2, g_board.probe_edges[2]); /* reaction pulse for the logic analyzer */
}

static void test_s2_runs_journal_and_telemetry(void) {
  setup();
  app_handle_line("scn 2 3");
  run_ms(3100);
  const char *o = g_board.console;
  ASSERT_TRUE(count_of(o, "TLM t_ms=") == 3);
  const char *p = strstr(o, "STAT name=journal n=");
  ASSERT_TRUE(p != NULL);
  ASSERT_NEAR(6, atoi(p + strlen("STAT name=journal n=")), 1);
  fake_board_clear_console(&g_board);
  app_handle_line("jv");
  ASSERT_STR_CONTAINS(g_board.console, "JOURNAL status=OK good=");
  ASSERT_STR_CONTAINS(g_board.console, "bad=0");
}

static void test_s3_long_operation_blocks_the_control_task(void) {
  setup();
  g_spi.busy_polls_after_erase = 5;
  app_handle_line("scn 3 3");
  run_ms(3100);
  const char *o = g_board.console;
  ASSERT_STR_CONTAINS(o, "lop_ms=1000");
  ASSERT_STR_CONTAINS(o, "STAT name=longop n=");
  ASSERT_STR_CONTAINS(o, "STAT name=journal n=");
  ASSERT_TRUE(g_board.probe_edges[1] > 0);
}

/* ---- button (FR-08) ---------------------------------------------------- */

static void test_press_calibrates_zero_orientation(void) {
  setup();
  fake_i2c_set_burst(&g_i2c, k_burst_roll30);
  g_board.adc_value = 0; /* gain 50 % */
  run_ms(100);
  ASSERT_NEAR(1583, g_board.pwm[2], 3);
  fake_board_push_event(&g_board, g_board.now_ms * 48000u, g_board.now_ms * 48000u);
  run_ms(100);
  ASSERT_NEAR(1500, g_board.pwm[2], 3); /* the 30 degrees are now the new zero */
}

static void test_long_press_toggles_hold_and_live(void) {
  setup();
  g_board.button = 1;
  fake_board_push_event(&g_board, g_board.now_ms * 48000u, g_board.now_ms * 48000u);
  run_ms(1100);
  ASSERT_EQ_INT(1, count_of(g_board.console, "MODE hold"));
  g_board.button = 0;
  run_ms(50);
  g_board.button = 1;
  fake_board_push_event(&g_board, g_board.now_ms * 48000u, g_board.now_ms * 48000u);
  run_ms(1100);
  ASSERT_EQ_INT(1, count_of(g_board.console, "MODE live"));
}

static void test_short_press_does_not_toggle_mode(void) {
  setup();
  g_board.button = 1;
  fake_board_push_event(&g_board, g_board.now_ms * 48000u, g_board.now_ms * 48000u);
  run_ms(300);
  g_board.button = 0;
  run_ms(1500);
  ASSERT_EQ_INT(0, count_of(g_board.console, "MODE "));
}

static void test_hold_mode_freezes_servos(void) {
  setup();
  g_board.button = 1;
  fake_board_push_event(&g_board, g_board.now_ms * 48000u, g_board.now_ms * 48000u);
  run_ms(1100); /* now in Hold */
  g_board.button = 0;
  fake_i2c_set_burst(&g_i2c, k_burst_roll30);
  g_board.adc_value = 0;
  const uint16_t before = g_board.pwm[2];
  run_ms(200);
  ASSERT_EQ_INT(before, g_board.pwm[2]);
}

/* ---- failure and recovery (FR-11) -------------------------------------- */

static void test_sensor_recovers_after_reconnect(void) {
  setup();
  run_ms(100);
  g_i2c.inject = BUS_ERR_NACK;
  run_ms(100);
  ASSERT_EQ_INT(1, count_of(g_board.console, "ERROR mpu6050 lost"));
  g_i2c.inject = BUS_OK; /* cable plugged back in */
  run_ms(1500);
  ASSERT_EQ_INT(1, count_of(g_board.console, "INFO mpu6050 recovered"));
  const int calls = g_i2c.calls;
  run_ms(100);
  ASSERT_TRUE(g_i2c.calls > calls); /* the control task polls the sensor again */
}

static void test_recovery_is_attempted_once_per_second(void) {
  setup();
  run_ms(100);
  g_i2c.inject = BUS_ERR_NACK;
  run_ms(100);
  const int c0 = g_i2c.calls;
  run_ms(3000);
  ASSERT_NEAR(3, g_i2c.calls - c0, 1);
}

/* ---- new commands ------------------------------------------------------ */

static void test_cmd_set_changes_configuration(void) {
  setup();
  app_handle_line("set imu 10");
  ASSERT_STR_CONTAINS(g_board.console, "CMD set imu_ms=10");
  app_handle_line("set btn poll");
  ASSERT_STR_CONTAINS(g_board.console, "btn=poll");
  ASSERT_EQ_INT(0, g_board.btn_irq);
  app_handle_line("set imu 0");
  app_handle_line("set bogus 1");
  app_handle_line("set btn maybe");
  ASSERT_EQ_INT(3, count_of(g_board.console, "CMD error usage: set"));
}

static void test_cmd_scn_validates_arguments(void) {
  setup();
  app_handle_line("scn 9");
  app_handle_line("scn 1 0");
  app_handle_line("scn x");
  ASSERT_EQ_INT(3, count_of(g_board.console, "CMD error usage: scn"));
}

static void test_cmd_misc_lr3(void) {
  setup();
  app_handle_line("ping");
  ASSERT_STR_CONTAINS(g_board.console, "PONG n=");
  app_handle_line("press 1500");
  ASSERT_EQ_INT(1500, g_board.stim_press_ms);
  app_handle_line("press 0");
  ASSERT_STR_CONTAINS(g_board.console, "CMD error usage: press");
  app_handle_line("mem");
  ASSERT_STR_CONTAINS(g_board.console, "MEM stack_used=0 stack_size=1024 heap_used=0");
  app_handle_line("rx");
  ASSERT_STR_CONTAINS(g_board.console, "RX bytes=0 overruns=0 cmds=");
  app_handle_line("jv");
  ASSERT_STR_CONTAINS(g_board.console, "JOURNAL status=OK good=0 bad=0");
  app_handle_line("jtorn");
  ASSERT_STR_CONTAINS(g_board.console, "CMD jtorn status=OK");
  app_handle_line("jv");
  ASSERT_STR_CONTAINS(g_board.console, "good=0 bad=1");
  app_handle_line("reboot");
  ASSERT_EQ_INT(1, g_board.reboots);
}

static void test_rx_statistics_are_reported(void) {
  setup();
  g_board.rx_bytes = 1234;
  g_board.rx_overruns = 7;
  app_handle_line("scn 1 1");
  run_ms(1100);
  ASSERT_STR_CONTAINS(g_board.console, "RX bytes=1234 overruns=7");
}

static const test_case_t k_tests[] = {
    {"perf: min/avg/max and reset", test_perf_stat_min_avg_max},
    {"perf: rounding and 64-bit sum", test_perf_stat_avg_rounds_and_does_not_overflow},
    {"perf: jitter histogram bins", test_perf_hist_bins},
    {"crc16: CCITT-FALSE check value 0x29B1", test_crc16_check_value},
    {"journal: record round trip, bit flip detected", test_journal_record_roundtrip_and_corruption},
    {"journal: erase per sector, verify", test_journal_append_erases_per_sector_and_verifies},
    {"journal: wraps after the last sector", test_journal_wraps_after_last_sector},
    {"journal: torn write detected by CRC", test_journal_torn_write_is_detected},
    {"journal: flash error counted", test_journal_flash_error_is_counted},
    {"filter: converges to accelerometer", test_cf_converges_to_accelerometer},
    {"filter: integrates gyro rate", test_cf_integrates_gyro_rate},
    {"filter: wraps around 180 degrees", test_cf_wraps_around_180_degrees},
    {"scenario S1: all metric families reported", test_scenario_s1_reports_all_metric_families},
    {"scenario S1: 50 Hz IMU rate", test_scenario_s1_imu_rate_is_50_hz},
    {"scenario: default configuration restored", test_scenario_restores_default_configuration},
    {"scenario: blocking -> missed periods, jitter", test_missed_periods_and_jitter_after_blocking},
    {"scenario: event latency edge -> handler", test_event_latency_is_edge_to_handler},
    {"scenario S2: journal + telemetry", test_s2_runs_journal_and_telemetry},
    {"scenario S3: long operation", test_s3_long_operation_blocks_the_control_task},
    {"button: press calibrates zero", test_press_calibrates_zero_orientation},
    {"button: long press toggles Hold/Live", test_long_press_toggles_hold_and_live},
    {"button: short press keeps the mode", test_short_press_does_not_toggle_mode},
    {"button: Hold freezes the servos", test_hold_mode_freezes_servos},
    {"recovery: sensor comes back (FR-11)", test_sensor_recovers_after_reconnect},
    {"recovery: one attempt per second", test_recovery_is_attempted_once_per_second},
    {"cmd: set", test_cmd_set_changes_configuration},
    {"cmd: scn argument check", test_cmd_scn_validates_arguments},
    {"cmd: ping, press, mem, rx, jv, jtorn, reboot", test_cmd_misc_lr3},
    {"scenario: UART receiver statistics", test_rx_statistics_are_reported},
};

int suite_lr3(void) {
  return RUN_SUITE("lr3 (measurement, journal, button, scenarios)", k_tests);
}
