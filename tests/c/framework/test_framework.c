/*
 * test_framework.c — suite runner for the LuaFan v2 C tests.
 */
#include "test_framework.h"

int g_tf_assert_fail = 0;
int g_tf_total = 0;
int g_tf_passed = 0;
int g_tf_failed = 0;

int run_all_suites(const test_suite_t *suites, int nsuites) {
    for (int s = 0; s < nsuites; s++) {
        const test_suite_t *su = &suites[s];
        printf("Suite: %s\n", su->name);
        for (int i = 0; i < su->count; i++) {
            const test_case_t *tc = &su->cases[i];
            g_tf_assert_fail = 0;
            g_tf_total++;
            tc->fn();
            if (g_tf_assert_fail) {
                g_tf_failed++;
                printf("  [FAIL] %s\n", tc->name);
            } else {
                g_tf_passed++;
                printf("  [ OK ] %s\n", tc->name);
            }
        }
    }
    printf("\n============================================\n");
    printf("TOTAL: %d  PASSED: %d  FAILED: %d\n", g_tf_total, g_tf_passed, g_tf_failed);
    printf("RESULT: %s\n", g_tf_failed == 0 ? "PASSED" : "FAILED");
    printf("============================================\n");
    return g_tf_failed == 0 ? 0 : 1;
}
