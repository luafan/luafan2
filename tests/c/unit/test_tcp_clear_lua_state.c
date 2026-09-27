/*
 * test_tcp_clear_lua_state.c — contract tests for fan_tcp_clear_lua_state().
 *
 * Background
 * ----------
 * fan.tcp caches the owning main lua_State in a file-static pointer
 * (src/net/tcp.c: `static lua_State *g_main_L`). libevent callbacks
 * (server_accept_cb, conn_wake via fan_coro_wake, conn_drain_writecb)
 * dereference this pointer without a per-callback state argument. If
 * lua_close(L) runs before those callbacks are torn down, the pointer
 * becomes dangling and any fired callback is a use-after-free. This
 * is the exact regression class covered by v1's problem 12 / 13 tests
 * (see luafan/tests/lua/test_tcpd_cleanup_mainthread.lua and
 * test_http_client_timer_linger.lua in the v1 repo).
 *
 * fan_tcp_clear_lua_state() is the teardown hook: callers invoke it
 * immediately before lua_close(L) so the pointer is NULL'd out. The
 * paired defence lives in server_accept_cb (tcp.c:472-476) which
 * exits early when `g_main_L == NULL`, closing the accepted fd
 * without touching Lua.
 *
 * Contract exercised here
 * -----------------------
 * 1. The symbol exists and is safely callable before any register().
 * 2. It is idempotent: calling it twice in a row does not fault and
 *    does not free anything the second call cannot own.
 * 3. Register → clear → register is a valid lifecycle (module can be
 *    reloaded after a state teardown; the next register() re-installs
 *    the pointer). This mirrors the "restart" scenario described in
 *    the tcp.c register comment.
 * 4. Clearing while no callback is pending is a no-op — the write is
 *    plain, no libevent state, no allocation freed.
 *
 * What this file does NOT test (out of scope, needs an integration
 * driver): actually firing server_accept_cb / conn_drain_writecb
 * after clear() to prove the !L branch is taken. Those callbacks are
 * static in tcp.c and require a real listening socket + accept event
 * to trigger. A Lua-level regression test covering the full teardown
 * sequence would live in tests/lua/, but the current 3 call sites
 * (all immediately before lua_close, after fan_loop_run returned)
 * make it structurally impossible to fire a stale callback in-tree.
 * See the tcp.c comment for the future extension points.
 */
#include "test_framework.h"
#include "../../../src/net/tcp.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

TEST_CASE(t_clear_without_register_is_safe) {
    /* No prior fan_tcp_register: g_main_L is still NULL from BSS.
     * Clear must be a plain assignment, not a free/deref. */
    fan_tcp_clear_lua_state();
    /* If we reach here without crashing, the contract holds. */
    TEST_ASSERT(1);
}

TEST_CASE(t_clear_is_idempotent) {
    /* Two clears in a row must not fault. This guards against a future
     * refactor that turns the clear into a free / decrement operation. */
    fan_tcp_clear_lua_state();
    fan_tcp_clear_lua_state();
    TEST_ASSERT(1);
}

TEST_CASE(t_register_then_clear_then_register_lifecycle) {
    /* The realistic reload path: main starts a state, fan.tcp registers,
     * teardown clears, a fresh state registers again. Both register()
     * calls must succeed and neither may leak the module table.
     *
     * We build the "fan" module table on the stack (fan_tcp_register
     * expects it at -1) and check the fan.tcp field appears both times. */
    lua_State *L1 = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L1);
    luaL_openlibs(L1);
    lua_newtable(L1);           /* the fake `fan` table at -1 */
    fan_tcp_register(L1);
    /* fan.tcp should now be set on the table at -1 */
    lua_getfield(L1, -1, "tcp");
    TEST_ASSERT_EQ(lua_type(L1, -1), LUA_TTABLE);
    lua_pop(L1, 1);             /* pop fan.tcp */

    /* Simulate the pre-lua_close teardown hook. */
    fan_tcp_clear_lua_state();

    /* State goes away — after this point, any surviving libevent
     * callback that touched g_main_L would UAF without the clear. */
    lua_close(L1);

    /* Fresh state: register must reinstall the pointer cleanly. */
    lua_State *L2 = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L2);
    luaL_openlibs(L2);
    lua_newtable(L2);
    fan_tcp_register(L2);
    lua_getfield(L2, -1, "tcp");
    TEST_ASSERT_EQ(lua_type(L2, -1), LUA_TTABLE);
    lua_pop(L2, 1);

    fan_tcp_clear_lua_state();  /* good hygiene for the next test */
    lua_close(L2);
    TEST_ASSERT(1);
}

TEST_CASE(t_clear_after_register_does_not_free_module_table) {
    /* fan_tcp_clear_lua_state() must NOT drop luaL_ref'd resources —
     * only clear the cached main-thread pointer. The module table lives
     * on the Lua stack / registry, and the metatables live in the
     * registry keyed by their tname. If the clear ever started free()ing
     * anything, this test would surface it (leaks show up under ASan;
     * a wrong free would trip the ASan quarantine). */
    lua_State *L = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    lua_newtable(L);
    fan_tcp_register(L);

    /* Snapshot the fan.tcp table reference and re-look-up after clear.
     * If clear() accidentally invalidated Lua state, this second lookup
     * would return a bogus value or trip a Lua assert. */
    lua_getfield(L, -1, "tcp");
    int type_before = lua_type(L, -1);
    lua_pop(L, 1);

    fan_tcp_clear_lua_state();

    lua_getfield(L, -1, "tcp");
    int type_after = lua_type(L, -1);
    lua_pop(L, 1);

    TEST_ASSERT_EQ(type_before, LUA_TTABLE);
    TEST_ASSERT_EQ(type_after,  LUA_TTABLE);

    fan_tcp_clear_lua_state();
    lua_close(L);
}

static const test_case_t cases[] = {
    {"clear_without_register_is_safe",           t_clear_without_register_is_safe},
    {"clear_is_idempotent",                      t_clear_is_idempotent},
    {"register_then_clear_then_register_lifecycle",
                                                 t_register_then_clear_then_register_lifecycle},
    {"clear_after_register_does_not_free_module_table",
                                                 t_clear_after_register_does_not_free_module_table},
};

const test_suite_t tcp_clear_lua_state_suite = {
    .name  = "tcp_clear_lua_state",
    .cases = cases,
    .count = (int)(sizeof(cases) / sizeof(cases[0])),
};
