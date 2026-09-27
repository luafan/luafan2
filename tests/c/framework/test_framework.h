/*
 * test_framework.h — LuaFan v2 minimal C unit test framework.
 * Assertion macros + a tiny suite runner. Designed to run clean under ASan.
 */
#ifndef FAN2_TEST_FRAMEWORK_H
#define FAN2_TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    void (*fn)(void);
} test_case_t;

typedef struct {
    const char *name;
    const test_case_t *cases;
    int count;
} test_suite_t;

/* Global counters (defined in test_main.c). */
extern int g_tf_assert_fail;   /* per-test failure flag */
extern int g_tf_total;
extern int g_tf_passed;
extern int g_tf_failed;

#define TEST_CASE(name) static void name(void)

#define TEST_FAIL(msg, ...) do { \
    g_tf_assert_fail = 1; \
    fprintf(stderr, "    ASSERT FAIL %s:%d: " msg "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
} while (0)

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { TEST_FAIL("expected true: %s", #cond); return; } \
} while (0)

#define TEST_ASSERT_TRUE(cond)  TEST_ASSERT(cond)
#define TEST_ASSERT_FALSE(cond) do { \
    if (cond) { TEST_FAIL("expected false: %s", #cond); return; } \
} while (0)

#define TEST_ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { TEST_FAIL("expected %lld == %lld (%s == %s)", _a, _b, #a, #b); return; } \
} while (0)

#define TEST_ASSERT_NOT_NULL(p) do { \
    if ((p) == NULL) { TEST_FAIL("expected non-NULL: %s", #p); return; } \
} while (0)

#define TEST_ASSERT_MEM_EQ(a, b, n) do { \
    if (memcmp((a), (b), (n)) != 0) { TEST_FAIL("memory mismatch: %s vs %s (%zu bytes)", #a, #b, (size_t)(n)); return; } \
} while (0)

int run_all_suites(const test_suite_t *suites, int nsuites);

#endif /* FAN2_TEST_FRAMEWORK_H */
