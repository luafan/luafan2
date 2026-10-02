/*
 * test_main.c — entry point aggregating all LuaFan v2 C unit suites.
 */
#include "test_framework.h"

extern const test_suite_t bytearray_suite;
extern const test_suite_t websocket_suite;
extern const test_suite_t tcp_clear_lua_state_suite;
extern const test_suite_t tcp_pending_teardown_suite;
extern const test_suite_t mariadb_pending_teardown_suite;
extern const test_suite_t clear_lua_states_suite;
extern const test_suite_t http_clear_lua_state_suite;
extern const test_suite_t dns_clear_lua_state_suite;
extern const test_suite_t udp_clear_lua_state_suite;
extern const test_suite_t crypto_suite;

int main(void) {
    const test_suite_t suites[] = {
        bytearray_suite,
        websocket_suite,
        tcp_clear_lua_state_suite,
        tcp_pending_teardown_suite,
        mariadb_pending_teardown_suite,
        clear_lua_states_suite,
        http_clear_lua_state_suite,
        dns_clear_lua_state_suite,
        udp_clear_lua_state_suite,
        crypto_suite,
    };
    return run_all_suites(suites, (int)(sizeof(suites) / sizeof(suites[0])));
}
