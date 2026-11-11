#include "perf.h"

void perf_stat_init(perf_stat_t *s, const char *name) {
  s->name = name;
  perf_stat_reset(s);
}

void perf_stat_reset(perf_stat_t *s) {
  s->count = 0;
  s->min = 0;
  s->max = 0;
  s->sum = 0;
}

void perf_stat_add(perf_stat_t *s, uint32_t value) {
  if (s->count == 0u || value < s->min) {
    s->min = value;
  }
  if (value > s->max) {
    s->max = value;
  }
  s->sum += value;
  s->count++;
}

uint32_t perf_stat_avg(const perf_stat_t *s) {
  if (s->count == 0u) {
    return 0u;
  }
  return (uint32_t)((s->sum + s->count / 2u) / s->count);
}

unsigned perf_hist_bin(uint32_t abs_us) {
  static const uint32_t k_limit[PERF_HIST_BINS - 1u] = {100u, 500u, 1000u, 5000u, 20000u};
  unsigned i = 0;
  while (i < PERF_HIST_BINS - 1u && abs_us > k_limit[i]) {
    i++;
  }
  return i;
}
