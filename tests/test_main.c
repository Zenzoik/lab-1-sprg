#include <stdio.h>

#include "test.h"

int g_test_failed;
const char *g_test_current;

int test_run_all(const char *suite, const test_case_t *tests, int count) {
  int failed = 0;
  printf("== %s (%d tests)\n", suite, count);
  for (int i = 0; i < count; i++) {
    g_test_failed = 0;
    g_test_current = tests[i].name;
    tests[i].fn();
    printf("  [%s] %s\n", g_test_failed ? "FAIL" : " ok ", tests[i].name);
    failed += g_test_failed ? 1 : 0;
  }
  return failed;
}

/* Suites live in their own files and are called from main(). */
int suite_motion(void);
int suite_mpu6050(void);
int suite_w25q64(void);
int suite_app(void);

int main(void) {
  int failed = 0;
  failed += suite_motion();
  failed += suite_mpu6050();
  failed += suite_w25q64();
  failed += suite_app();
  printf("\n%s: %d test(s) failed\n", failed ? "FAILED" : "PASSED", failed);
  return failed ? 1 : 0;
}
