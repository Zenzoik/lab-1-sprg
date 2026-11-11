/**
 * @file perf.h
 * @brief Platform independent helpers of the measurement module (LR3): running statistics and
 *        a histogram of absolute jitter values.
 *
 * Time is measured by the platform's free-running CPU-cycle counter (see app_ports_t::cycles);
 * this module only accumulates numbers, it never touches hardware.
 */
#ifndef APP_PERF_H
#define APP_PERF_H

#include <stdint.h>

/** Running min / max / sum / count of one metric (microseconds, percent, ...). */
typedef struct {
  const char *name;
  uint32_t count;
  uint32_t min;
  uint32_t max;
  uint64_t sum;
} perf_stat_t;

void perf_stat_init(perf_stat_t *s, const char *name);
void perf_stat_reset(perf_stat_t *s);
void perf_stat_add(perf_stat_t *s, uint32_t value);
/** Mean rounded to nearest; 0 if nothing was recorded. */
uint32_t perf_stat_avg(const perf_stat_t *s);

/** Histogram of |jitter| in microseconds: <=100, <=500, <=1000, <=5000, <=20000, >20000. */
#define PERF_HIST_BINS 6u
unsigned perf_hist_bin(uint32_t abs_us);

#endif /* APP_PERF_H */
