#include <string.h>

#include "fakes.h"
#include "test.h"
#include "w25q64.h"

/* millis() for the driver under test: virtual time that advances on every call */
static uint32_t g_ms;
static uint32_t fake_millis(void) {
  return g_ms++;
}

static void setup(fake_spi_t *f, w25q_t *dev) {
  g_ms = 0;
  fake_spi_init(f);
  w25q_bind(dev, &f->bus, fake_millis);
}

/* ---- identification --------------------------------------------------- */

static void test_jedec_bytes_from_capture(void) {
  /* docs/captures/spi_jedec.txt: 9Fh -> EF 40 17 */
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  uint8_t id[3] = {0, 0, 0};
  ASSERT_EQ_INT(W25Q_OK, w25q_check_id(&dev, id));
  ASSERT_EQ_INT(0xEF, id[0]);
  ASSERT_EQ_INT(0x40, id[1]);
  ASSERT_EQ_INT(0x17, id[2]);
  ASSERT_EQ_INT(1, f.log_n);
  ASSERT_EQ_INT(0x9F, f.log[0].op);
  ASSERT_EQ_INT(2, f.cs_edges); /* CS# low once, high once: command is one transaction */
}

static void test_wrong_id_is_rejected(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  f.jedec[2] = 0x16; /* W25Q32 */
  uint8_t id[3];
  ASSERT_EQ_INT(W25Q_ERR_WRONG_ID, w25q_check_id(&dev, id));
  ASSERT_EQ_INT(0x16, id[2]);
}

static void test_wrong_spi_mode_reads_garbage_id(void) {
  /* docs/captures/spi_anomaly_mode3.txt: FF CF FF ... instead of EF 40 17 */
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  f.wrong_mode = 1;
  uint8_t id[3] = {0, 0, 0};
  ASSERT_EQ_INT(W25Q_ERR_WRONG_ID, w25q_check_id(&dev, id));
  ASSERT_TRUE(!(id[0] == 0xEF && id[1] == 0x40 && id[2] == 0x17));
}

static void test_bus_error_is_reported(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  uint8_t id[3];
  f.inject = BUS_ERR_IO;
  ASSERT_EQ_INT(W25Q_ERR_BUS, w25q_check_id(&dev, id));
  f.inject = BUS_ERR_TIMEOUT;
  ASSERT_EQ_INT(W25Q_ERR_TIMEOUT, w25q_check_id(&dev, id));
}

static void test_cs_is_released_after_a_failed_transfer(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  f.inject = BUS_ERR_IO;
  uint8_t id[3];
  (void)w25q_read_jedec(&dev, id);
  ASSERT_EQ_INT(0, f.cs_low);
}

/* ---- write sequence --------------------------------------------------- */

static void test_page_program_sends_write_enable_first(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  const uint8_t data[4] = {1, 2, 3, 4};
  ASSERT_EQ_INT(W25Q_OK, w25q_page_program(&dev, 0x0010F8, data, sizeof data));
  /* sequence: WREN, RDSR (WEL check), PP, RDSR ... */
  ASSERT_EQ_INT(0x06, f.log[0].op);
  ASSERT_EQ_INT(0x05, f.log[1].op);
  ASSERT_EQ_INT(0x02, f.log[2].op);
  ASSERT_EQ_INT(0, f.log[0].wel_at_start);
  ASSERT_EQ_INT(1, f.log[2].wel_at_start); /* WEL was set when PP started: write was accepted */
  ASSERT_EQ_INT(0x0010F8, f.log[2].addr);
  ASSERT_EQ_INT(4, f.log[2].len);
  ASSERT_MEM_EQ(data, &f.mem[0x10F8], 4);
}

static void test_program_without_write_enable_changes_nothing(void) {
  /* Model sanity: a PP that is not preceded by WREN must be ignored by the part. */
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  const uint8_t hdr[4] = {0x02, 0x00, 0x10, 0x00};
  f.bus.cs(f.bus.ctx, 0);
  (void)f.bus.transfer(f.bus.ctx, hdr, NULL, 4);
  const uint8_t d = 0x00;
  (void)f.bus.transfer(f.bus.ctx, &d, NULL, 1);
  f.bus.cs(f.bus.ctx, 1);
  ASSERT_EQ_INT(0xFF, f.mem[0x1000]);
}

static void test_write_enable_not_latched_is_reported(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  f.wp_stuck = 1;
  const uint8_t data[2] = {0xAA, 0x55};
  ASSERT_EQ_INT(W25Q_ERR_NOT_WRITE_ENABLED, w25q_page_program(&dev, 0, data, sizeof data));
  ASSERT_EQ_INT(W25Q_ERR_NOT_WRITE_ENABLED, w25q_sector_erase(&dev, 0));
  /* no PP / SE opcode may have been sent */
  for (int i = 0; i < f.log_n; i++) {
    ASSERT_TRUE(f.log[i].op != 0x02 && f.log[i].op != 0x20);
  }
}

static void test_sector_erase_sequence_and_effect(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  f.mem[0x1000] = 0x00;
  f.mem[0x1FFF] = 0x12;
  f.mem[0x2000] = 0x34; /* next sector must survive */
  ASSERT_EQ_INT(W25Q_OK, w25q_sector_erase(&dev, 0x1234));
  ASSERT_EQ_INT(0x06, f.log[0].op);
  ASSERT_EQ_INT(0x05, f.log[1].op);
  ASSERT_EQ_INT(0x20, f.log[2].op);
  ASSERT_EQ_INT(1, f.log[2].wel_at_start);
  ASSERT_EQ_INT(0x001234, f.log[2].addr);
  ASSERT_EQ_INT(0xFF, f.mem[0x1000]);
  ASSERT_EQ_INT(0xFF, f.mem[0x1FFF]);
  ASSERT_EQ_INT(0x34, f.mem[0x2000]);
  /* afterwards the driver polled RDSR until BUSY cleared (5 busy polls + 1 free) */
  int polls = 0;
  for (int i = 3; i < f.log_n; i++) {
    polls += (f.log[i].op == 0x05);
  }
  ASSERT_EQ_INT(6, polls);
}

static void test_program_can_only_clear_bits(void) {
  /* NOR flash: programming ANDs the new data into the old content */
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  const uint8_t a = 0xF0;
  const uint8_t b = 0x3C;
  ASSERT_EQ_INT(W25Q_OK, w25q_page_program(&dev, 0x200, &a, 1));
  ASSERT_EQ_INT(W25Q_OK, w25q_page_program(&dev, 0x200, &b, 1));
  ASSERT_EQ_INT(0x30, f.mem[0x200]);
}

static void test_write_splits_at_page_boundary(void) {
  /* the self-test pattern: 16 bytes at 0x0010F8 crosses the page boundary at 0x1100 */
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  uint8_t wr[16];
  for (int i = 0; i < 16; i++) {
    wr[i] = (uint8_t)(0xA0 + i);
  }
  ASSERT_EQ_INT(W25Q_OK, w25q_write(&dev, 0x0010F8, wr, sizeof wr));
  int pp = 0;
  uint32_t addrs[4] = {0};
  size_t lens[4] = {0};
  for (int i = 0; i < f.log_n; i++) {
    if (f.log[i].op == 0x02 && pp < 4) {
      addrs[pp] = f.log[i].addr;
      lens[pp] = f.log[i].len;
      pp++;
    }
  }
  ASSERT_EQ_INT(2, pp);
  ASSERT_EQ_INT(0x0010F8, addrs[0]);
  ASSERT_EQ_INT(8, lens[0]);
  ASSERT_EQ_INT(0x001100, addrs[1]);
  ASSERT_EQ_INT(8, lens[1]);
  ASSERT_MEM_EQ(wr, &f.mem[0x10F8], 16);
}

static void test_every_program_is_preceded_by_its_own_write_enable(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  uint8_t wr[300];
  memset(wr, 0x5A, sizeof wr);
  ASSERT_EQ_INT(W25Q_OK, w25q_write(&dev, 0x0000, wr, sizeof wr)); /* 256 + 44 */
  int pending_wren = 0;
  int programs = 0;
  for (int i = 0; i < f.log_n; i++) {
    if (f.log[i].op == 0x06) {
      pending_wren = 1;
    }
    if (f.log[i].op == 0x02) {
      ASSERT_TRUE(pending_wren); /* a WREN was seen since the previous PP */
      pending_wren = 0;
      programs++;
    }
  }
  ASSERT_EQ_INT(2, programs);
}

static void test_page_program_argument_checks(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  uint8_t buf[300] = {0};
  ASSERT_EQ_INT(W25Q_ERR_ARG, w25q_page_program(&dev, 0, NULL, 4));
  ASSERT_EQ_INT(W25Q_ERR_ARG, w25q_page_program(&dev, 0, buf, 0));
  ASSERT_EQ_INT(W25Q_ERR_ARG, w25q_page_program(&dev, 0, buf, 257));
  ASSERT_EQ_INT(W25Q_ERR_ARG, w25q_page_program(&dev, 0xFC, buf, 8)); /* crosses 0x100 */
  ASSERT_EQ_INT(W25Q_ERR_ARG, w25q_write(&dev, 0, NULL, 4));
  ASSERT_EQ_INT(W25Q_ERR_ARG, w25q_read(&dev, 0, NULL, 4));
  ASSERT_EQ_INT(W25Q_ERR_ARG, w25q_read(&dev, 0, buf, 0));
  ASSERT_EQ_INT(0, f.log_n); /* nothing reached the bus */
}

/* ---- busy handling ---------------------------------------------------- */

static void test_wait_ready_times_out_when_busy_sticks(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  f.stuck_busy = 1;
  ASSERT_EQ_INT(W25Q_ERR_TIMEOUT, w25q_wait_ready(&dev, 10));
}

static void test_erase_reports_timeout_if_flash_never_finishes(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  f.busy_polls_after_erase = 1000000;
  ASSERT_EQ_INT(W25Q_ERR_TIMEOUT, w25q_sector_erase(&dev, 0x1000));
}

static void test_wait_ready_returns_on_bus_error(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  f.inject = BUS_ERR_IO;
  ASSERT_EQ_INT(W25Q_ERR_BUS, w25q_wait_ready(&dev, 10));
}

/* ---- read ------------------------------------------------------------- */

static void test_read_uses_03_with_24_bit_address(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  for (int i = 0; i < 16; i++) {
    f.mem[0x10F8 + i] = (uint8_t)(0xA0 + i); /* bytes seen on the wire in spi_program_read.txt */
  }
  uint8_t buf[16];
  ASSERT_EQ_INT(W25Q_OK, w25q_read(&dev, 0x0010F8, buf, sizeof buf));
  ASSERT_EQ_INT(0x03, f.log[0].op);
  ASSERT_EQ_INT(0x0010F8, f.log[0].addr);
  ASSERT_EQ_INT(16, f.log[0].len);
  ASSERT_EQ_INT(0xA0, buf[0]);
  ASSERT_EQ_INT(0xAF, buf[15]);
}

static void test_status_register_bits(void) {
  fake_spi_t f;
  w25q_t dev;
  setup(&f, &dev);
  uint8_t sr = 0xEE;
  ASSERT_EQ_INT(W25Q_OK, w25q_read_status(&dev, &sr));
  ASSERT_EQ_INT(0x00, sr);
  f.wel = 1;
  f.busy_polls = 1;
  ASSERT_EQ_INT(W25Q_OK, w25q_read_status(&dev, &sr));
  ASSERT_EQ_INT(W25Q_SR1_BUSY | W25Q_SR1_WEL, sr);
}

static const test_case_t k_tests[] = {
    {"JEDEC ID bytes from capture (EF 40 17)", test_jedec_bytes_from_capture},
    {"wrong JEDEC ID is rejected", test_wrong_id_is_rejected},
    {"wrong SPI mode reads garbage ID", test_wrong_spi_mode_reads_garbage_id},
    {"bus error / timeout is reported", test_bus_error_is_reported},
    {"CS# released after failed transfer", test_cs_is_released_after_a_failed_transfer},
    {"page program: WREN before PP", test_page_program_sends_write_enable_first},
    {"model: PP without WREN is ignored", test_program_without_write_enable_changes_nothing},
    {"WEL not latched -> error, no PP/SE sent", test_write_enable_not_latched_is_reported},
    {"sector erase: sequence and effect", test_sector_erase_sequence_and_effect},
    {"program can only clear bits", test_program_can_only_clear_bits},
    {"write splits at page boundary (0x10F8+16)", test_write_splits_at_page_boundary},
    {"every PP has its own WREN", test_every_program_is_preceded_by_its_own_write_enable},
    {"argument checks reach no bus", test_page_program_argument_checks},
    {"wait_ready: timeout when BUSY sticks", test_wait_ready_times_out_when_busy_sticks},
    {"erase: timeout if flash never finishes", test_erase_reports_timeout_if_flash_never_finishes},
    {"wait_ready: bus error", test_wait_ready_returns_on_bus_error},
    {"read: 03h + 24-bit address", test_read_uses_03_with_24_bit_address},
    {"status register BUSY/WEL bits", test_status_register_bits},
};

int suite_w25q64(void) {
  return RUN_SUITE("w25q64 driver (fake SPI flash)", k_tests);
}
