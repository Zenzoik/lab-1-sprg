/**
 * @file test.h
 * @brief Minimal unit-test framework (no external dependency, runs on the host).
 *
 * Every test is a function registered with TEST(); ASSERT_* record a failure
 * and return from the test. The runner prints one line per test and exits
 * non-zero if anything failed, so `make test` fails the CI job.
 */
#ifndef TESTS_TEST_H
#define TESTS_TEST_H

#include <stdio.h>
#include <string.h>

typedef void (*test_fn)(void);

typedef struct {
  const char *name;
  test_fn fn;
} test_case_t;

extern int g_test_failed; /* set by the ASSERT_* macros for the running test */
extern const char *g_test_current;

#define ASSERT_TRUE(cond)                                                              \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      printf("    %s:%d: ASSERT_TRUE(%s) failed\n", __FILE__, __LINE__, #cond);          \
      g_test_failed = 1;                                                               \
      return;                                                                          \
    }                                                                                  \
  } while (0)

#define ASSERT_EQ_INT(expected, actual)                                                \
  do {                                                                                 \
    long long e_ = (long long)(expected);                                              \
    long long a_ = (long long)(actual);                                                \
    if (e_ != a_) {                                                                    \
      printf("    %s:%d: expected %lld (0x%llX) but got %lld (0x%llX) in %s\n", __FILE__, \
             __LINE__, e_, (unsigned long long)e_, a_, (unsigned long long)a_, #actual);   \
      g_test_failed = 1;                                                               \
      return;                                                                          \
    }                                                                                  \
  } while (0)

#define ASSERT_NEAR(expected, actual, tol)                                             \
  do {                                                                                 \
    long long e_ = (long long)(expected);                                              \
    long long a_ = (long long)(actual);                                                \
    long long d_ = (e_ > a_) ? (e_ - a_) : (a_ - e_);                                  \
    if (d_ > (long long)(tol)) {                                                       \
      printf("    %s:%d: expected %lld +/- %lld but got %lld in %s\n", __FILE__, __LINE__, \
             e_, (long long)(tol), a_, #actual);                                       \
      g_test_failed = 1;                                                               \
      return;                                                                          \
    }                                                                                  \
  } while (0)

#define ASSERT_MEM_EQ(expected, actual, n)                                             \
  do {                                                                                 \
    if (memcmp((expected), (actual), (n)) != 0) {                                      \
      printf("    %s:%d: memory differs: %s vs %s (%d bytes)\n", __FILE__, __LINE__,    \
             #expected, #actual, (int)(n));                                            \
      g_test_failed = 1;                                                               \
      return;                                                                          \
    }                                                                                  \
  } while (0)

#define ASSERT_STR_CONTAINS(haystack, needle)                                          \
  do {                                                                                 \
    if (strstr((haystack), (needle)) == NULL) {                                        \
      printf("    %s:%d: \"%s\" not found in output:\n%s\n", __FILE__, __LINE__, (needle), \
             (haystack));                                                              \
      g_test_failed = 1;                                                               \
      return;                                                                          \
    }                                                                                  \
  } while (0)

/** Run an array of tests, print a summary, return the number of failures. */
int test_run_all(const char *suite, const test_case_t *tests, int count);

#define RUN_SUITE(name, array) test_run_all((name), (array), (int)(sizeof(array) / sizeof((array)[0])))

#endif /* TESTS_TEST_H */
