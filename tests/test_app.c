#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "fakes.h"
#include "test.h"

/* App has file-scope state (one instance per firmware), so each test re-initialises it. */
static fake_i2c_t g_i2c;
static fake_spi_t g_spi;
static fake_board_t g_board;

static const uint8_t k_burst_flat[14] = {0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0xEF,
                                         0x5C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static void setup(void) {
  fake_i2c_init(&g_i2c, 0x68);
  fake_i2c_set_burst(&g_i2c, k_burst_flat);
  fake_spi_init(&g_spi);
  fake_board_init(&g_board, &g_i2c, &g_spi);
  app_init(&g_board.ports);
}

static int count_lines_with(const char *text, const char *needle) {
  int n = 0;
  for (const char *p = text; (p = strstr(p, needle)) != NULL; p += strlen(needle)) {
    n++;
  }
  return n;
}

/* ---- self-test --------------------------------------------------------- */

static void test_selftest_all_devices_ok(void) {
  setup();
  ASSERT_EQ_INT(0, app_selftest());
  const char *o = g_board.console;
  ASSERT_STR_CONTAINS(o, "SELFTEST BEGIN version=lr2");
  ASSERT_STR_CONTAINS(o, "DEV mpu6050_id status=OK addr=0x68 who=0x68 expected=0x68");
  ASSERT_STR_CONTAINS(o, "DEV mpu6050_data status=OK ax_mg=0 ay_mg=0 az_mg=1000 norm_mg=1000");
  ASSERT_STR_CONTAINS(o, "DEV w25q64_id status=OK jedec=EF4017 expected=EF4017");
  ASSERT_STR_CONTAINS(o, "DEV w25q64_erase status=OK addr=0x001000");
  ASSERT_STR_CONTAINS(o, "DEV w25q64_rw status=OK addr=0x0010F8 len=16");
  ASSERT_STR_CONTAINS(o, "match=1");
  ASSERT_STR_CONTAINS(o, "DEV adc status=OK");
  ASSERT_STR_CONTAINS(o, "DEV button status=OK pressed=0");
  ASSERT_STR_CONTAINS(o, "DEV pwm status=OK ch1_us=1500 ch2_us=1500");
  ASSERT_STR_CONTAINS(o, "SELFTEST RESULT pass=8 fail=0");
}

static void test_selftest_lines_are_machine_parseable(void) {
  setup();
  (void)app_selftest();
  /* every DEV line: "DEV <name> status=OK|FAIL ..." with CRLF termination */
  int dev_lines = 0;
  for (const char *p = g_board.console; (p = strstr(p, "DEV ")) != NULL; p += 4) {
    const char *eol = strstr(p, "\r\n");
    ASSERT_TRUE(eol != NULL);
    const char *st = strstr(p, " status=");
    ASSERT_TRUE(st != NULL && st < eol);
    ASSERT_TRUE(strncmp(st + 8, "OK", 2) == 0 || strncmp(st + 8, "FAIL", 4) == 0);
    dev_lines++;
  }
  ASSERT_EQ_INT(8, dev_lines);
}

static void test_selftest_imu_missing(void) {
  setup();
  g_i2c.present = 0;
  ASSERT_EQ_INT(2, app_selftest()); /* id + data (reported as skipped) */
  ASSERT_STR_CONTAINS(g_board.console, "DEV mpu6050_id status=FAIL addr=0x68 who=0x00");
  ASSERT_STR_CONTAINS(g_board.console, "DEV mpu6050_data status=FAIL skipped=mpu6050_id_failed");
  ASSERT_STR_CONTAINS(g_board.console, "SELFTEST RESULT pass=6 fail=2");
}

static void test_selftest_imu_wrong_gravity_is_flagged(void) {
  setup();
  uint8_t zero[14] = {0};
  fake_i2c_set_burst(&g_i2c, zero); /* all-zero sample: |a| = 0 g, temp = 36.53 degC */
  ASSERT_EQ_INT(1, app_selftest());
  ASSERT_STR_CONTAINS(g_board.console, "DEV mpu6050_data status=FAIL");
  ASSERT_STR_CONTAINS(g_board.console, "norm_mg=0");
}

static void test_selftest_implausible_temperature_is_flagged(void) {
  setup();
  uint8_t hot[14];
  memcpy(hot, k_burst_flat, sizeof hot);
  hot[6] = 0x7F; /* raw 0x7F5C = 32604 -> 132 degC, outside 10..40 degC */
  fake_i2c_set_burst(&g_i2c, hot);
  ASSERT_EQ_INT(1, app_selftest());
  ASSERT_STR_CONTAINS(g_board.console, "DEV mpu6050_data status=FAIL");
}

static void test_selftest_flash_missing_reports_wrong_id(void) {
  setup();
  g_spi.jedec[0] = 0xFF;
  g_spi.jedec[1] = 0xFF;
  g_spi.jedec[2] = 0xFF; /* MISO floating high: nothing connected */
  ASSERT_EQ_INT(3, app_selftest()); /* id + the two dependent checks */
  ASSERT_STR_CONTAINS(g_board.console, "DEV w25q64_id status=FAIL jedec=FFFFFF");
  ASSERT_STR_CONTAINS(g_board.console, "DEV w25q64_erase status=FAIL skipped=w25q64_id_failed");
  ASSERT_STR_CONTAINS(g_board.console, "DEV w25q64_rw status=FAIL skipped=w25q64_id_failed");
}

static void test_selftest_flash_write_protected(void) {
  setup();
  g_spi.wp_stuck = 1;
  ASSERT_EQ_INT(2, app_selftest()); /* erase + the rw check that depends on it */
  ASSERT_STR_CONTAINS(g_board.console, "DEV w25q64_id status=OK");
  ASSERT_STR_CONTAINS(g_board.console, "DEV w25q64_erase status=FAIL");
  ASSERT_STR_CONTAINS(g_board.console, "DEV w25q64_rw status=FAIL skipped=w25q64_erase_failed");
}

static void test_selftest_adc_failure(void) {
  setup();
  g_board.adc_fail = 1;
  ASSERT_EQ_INT(1, app_selftest());
  ASSERT_STR_CONTAINS(g_board.console, "DEV adc status=FAIL");
}

static void test_selftest_marker_pulses_around_adc(void) {
  setup();
  (void)app_selftest();
  ASSERT_EQ_INT(2, g_board.marker_edges); /* high before the conversion, low after */
  ASSERT_EQ_INT(0, g_board.marker_level);
}

/* ---- boot -------------------------------------------------------------- */

static void test_boot_prints_anomalies_and_pwm_demo(void) {
  setup();
  app_boot();
  const char *o = g_board.console;
  ASSERT_STR_CONTAINS(o, "hello from STM32 (lr2)");
  ASSERT_STR_CONTAINS(o, "ANOMALY i2c_probe addr=0x69 result=NACK");
  const char *an = strstr(o, "ANOMALY spi_mode=3 jedec=");
  ASSERT_TRUE(an != NULL);
  ASSERT_TRUE(strncmp(an + strlen("ANOMALY spi_mode=3 jedec="), "EF4017", 6) != 0); /* garbage, not the real ID */
  ASSERT_STR_CONTAINS(o, "PWM_DEMO ch=1 us=1000");
  ASSERT_STR_CONTAINS(o, "PWM_DEMO ch=1 us=2000");
  ASSERT_STR_CONTAINS(o, "PWM_DEMO ch=1 us=1500");
  ASSERT_STR_CONTAINS(o, "READY");
  ASSERT_EQ_INT(0, g_board.spi_mode); /* mode restored */
}

/* ---- command line ------------------------------------------------------ */

static void test_cmd_jedec_and_whoami(void) {
  setup();
  app_handle_line("jedec");
  app_handle_line("whoami");
  ASSERT_STR_CONTAINS(g_board.console, "CMD jedec status=OK id=EF4017");
  ASSERT_STR_CONTAINS(g_board.console, "CMD whoami status=OK who=0x68");
}

static void test_cmd_probe_ack_and_nack(void) {
  setup();
  app_handle_line("probe 68");
  app_handle_line("probe 69");
  ASSERT_STR_CONTAINS(g_board.console, "CMD probe addr=0x68 result=ACK");
  ASSERT_STR_CONTAINS(g_board.console, "CMD probe addr=0x69 result=NACK");
}

static void test_cmd_probe_rejects_bad_address(void) {
  setup();
  app_handle_line("probe 80");
  app_handle_line("probe zz");
  ASSERT_EQ_INT(2, count_lines_with(g_board.console, "CMD error bad address"));
}

static void test_cmd_reg_read(void) {
  setup();
  app_handle_line("reg 68 75");
  app_handle_line("reg 68 3b 14");
  ASSERT_STR_CONTAINS(g_board.console, "CMD reg addr=0x68 reg=0x75 status=OK");
  ASSERT_STR_CONTAINS(g_board.console, "data=68\r\n");
  ASSERT_STR_CONTAINS(g_board.console, "data=000000004000EF5C000000000000\r\n");
}

static void test_cmd_reg_write_and_nack(void) {
  setup();
  app_handle_line("regw 68 1a 03");
  ASSERT_EQ_INT(3, g_i2c.regs[0x1A]);
  app_handle_line("regw 69 1a 03");
  ASSERT_STR_CONTAINS(g_board.console, "CMD regw addr=0x69");
  ASSERT_STR_CONTAINS(g_board.console, "status=FAIL");
}

static void test_cmd_pwm_sets_pulse_and_validates_range(void) {
  setup();
  app_handle_line("pwm 2 1750");
  ASSERT_EQ_INT(1750, g_board.pwm[2]);
  app_handle_line("pwm 2 3000");
  app_handle_line("pwm 3 1500");
  ASSERT_EQ_INT(1750, g_board.pwm[2]);
  ASSERT_EQ_INT(2, count_lines_with(g_board.console, "CMD error usage: pwm"));
}

static void test_cmd_adc_reports_raw_and_millivolts(void) {
  setup();
  g_board.adc_value = 4095;
  app_handle_line("adc");
  ASSERT_STR_CONTAINS(g_board.console, "CMD adc status=OK raw=4095 mv=3300");
}

static void test_cmd_unknown_and_empty(void) {
  setup();
  app_handle_line("");
  app_handle_line("   ");
  app_handle_line("bogus");
  ASSERT_STR_CONTAINS(g_board.console, "CMD error unknown command");
  ASSERT_EQ_INT(1, count_lines_with(g_board.console, "CMD "));
}

/* ---- control loop (FR-05..FR-08, FR-11) ------------------------------- */

static void run_ms(unsigned ms) {
  for (unsigned i = 0; i < ms; i++) {
    g_board.now_ms++;
    app_poll();
  }
}

static void test_control_loop_tracks_tilt(void) {
  setup();
  g_board.tick_ms = 0;
  (void)app_selftest(); /* initialises the IMU */
  /* 30 deg roll: ay = 500 mg (8192 LSB), az = 866 mg (14189 LSB = 0x376D) */
  uint8_t b[14] = {0x00, 0x00, 0x20, 0x00, 0x37, 0x6D, 0xEF, 0x5C, 0, 0, 0, 0, 0, 0};
  fake_i2c_set_burst(&g_i2c, b);
  g_board.adc_value = 0; /* gain 50 % */
  fake_board_clear_console(&g_board);
  run_ms(60);
  /* roll 30 deg at 50 % gain: 3000 * 50 / 1800 = +83 us */
  ASSERT_NEAR(1583, g_board.pwm[2], 3);
  ASSERT_NEAR(1500, g_board.pwm[1], 1); /* pitch stays level */
}

static void test_control_loop_runs_at_50_hz(void) {
  setup();
  g_board.tick_ms = 0;
  (void)app_selftest();
  int calls_before = g_i2c.calls;
  run_ms(1000);
  /* one 14-byte burst every 20 ms: 50 +/- 1 per second */
  ASSERT_NEAR(50, g_i2c.calls - calls_before, 1);
}

static void test_control_loop_telemetry_once_per_second(void) {
  setup();
  g_board.tick_ms = 0;
  (void)app_selftest();
  fake_board_clear_console(&g_board);
  run_ms(3100);
  ASSERT_EQ_INT(3, count_lines_with(g_board.console, "TLM t_ms="));
}

static void test_sensor_loss_centres_servos(void) {
  setup();
  g_board.tick_ms = 0;
  (void)app_selftest();
  g_board.pwm[1] = 1900;
  g_board.pwm[2] = 1100;
  g_i2c.inject = BUS_ERR_NACK; /* cable pulled */
  fake_board_clear_console(&g_board);
  run_ms(60);
  ASSERT_EQ_INT(1500, g_board.pwm[1]);
  ASSERT_EQ_INT(1500, g_board.pwm[2]);
  ASSERT_EQ_INT(1, count_lines_with(g_board.console, "ERROR mpu6050 lost, servos centred"));
}

static void test_no_imu_no_bus_traffic_in_control_loop(void) {
  setup();
  g_board.tick_ms = 0;
  g_i2c.present = 0;
  (void)app_selftest();
  const int c = g_i2c.calls;
  run_ms(200);
  ASSERT_EQ_INT(c, g_i2c.calls); /* a device that failed the self-test is not polled */
}

static void test_uart_line_editing(void) {
  setup();
  g_board.tick_ms = 0;
  g_board.rx_script = "adc\r\n";
  run_ms(5);
  ASSERT_STR_CONTAINS(g_board.console, "CMD adc status=OK");
}

static void test_uart_line_too_long_is_not_executed(void) {
  setup();
  g_board.tick_ms = 0;
  g_board.rx_script = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n";
  run_ms(100);
  ASSERT_TRUE(strstr(g_board.console, "CMD adc") == NULL);
  ASSERT_STR_CONTAINS(g_board.console, "CMD error unknown command");
}

static const test_case_t k_tests[] = {
    {"selftest: all devices OK", test_selftest_all_devices_ok},
    {"selftest: lines are machine-parseable", test_selftest_lines_are_machine_parseable},
    {"selftest: IMU missing", test_selftest_imu_missing},
    {"selftest: IMU gravity out of range", test_selftest_imu_wrong_gravity_is_flagged},
    {"selftest: IMU temperature implausible", test_selftest_implausible_temperature_is_flagged},
    {"selftest: flash missing (FFFFFF)", test_selftest_flash_missing_reports_wrong_id},
    {"selftest: flash write-protected", test_selftest_flash_write_protected},
    {"selftest: ADC failure", test_selftest_adc_failure},
    {"selftest: marker brackets the ADC read", test_selftest_marker_pulses_around_adc},
    {"boot: anomalies and PWM demo", test_boot_prints_anomalies_and_pwm_demo},
    {"cmd: jedec, whoami", test_cmd_jedec_and_whoami},
    {"cmd: probe ACK/NACK", test_cmd_probe_ack_and_nack},
    {"cmd: probe bad address", test_cmd_probe_rejects_bad_address},
    {"cmd: reg read", test_cmd_reg_read},
    {"cmd: regw write / NACK", test_cmd_reg_write_and_nack},
    {"cmd: pwm range check", test_cmd_pwm_sets_pulse_and_validates_range},
    {"cmd: adc", test_cmd_adc_reports_raw_and_millivolts},
    {"cmd: unknown / empty", test_cmd_unknown_and_empty},
    {"control: servo follows tilt", test_control_loop_tracks_tilt},
    {"control: 50 Hz IMU rate", test_control_loop_runs_at_50_hz},
    {"control: telemetry 1 Hz", test_control_loop_telemetry_once_per_second},
    {"control: sensor loss centres servos (FR-11)", test_sensor_loss_centres_servos},
    {"control: failed IMU is not polled", test_no_imu_no_bus_traffic_in_control_loop},
    {"uart: line is executed on CR/LF", test_uart_line_editing},
    {"uart: overlong line is truncated, not executed as adc", test_uart_line_too_long_is_not_executed},
};

int suite_app(void) {
  return RUN_SUITE("app (fake board ports)", k_tests);
}
