/*
 * test_main.c — entry point aggregating all LuaFan v2 C unit suites.
 */
#include "test_framework.h"

extern const test_suite_t bytearray_suite;
extern const test_suite_t websocket_suite;

int main(void) {
    const test_suite_t suites[] = {
        bytearray_suite,
        websocket_suite,
    };
    return run_all_suites(suites, (int)(sizeof(suites) / sizeof(suites[0])));
}
