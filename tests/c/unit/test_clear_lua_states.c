/*
 * test_clear_lua_states.c — contract tests for the whole fan_*_clear_lua_state
 * family + the aggregate fan_clear_lua_states() teardown hook.
 *
 * This is the "A-track" completion of the work started in
 * tests/c/unit/test_tcp_clear_lua_state.c (that suite focuses on tcp in
 * isolation). Here we sweep every module that caches a main-thread pointer
 * — tcp, httpd, http, udp, dns, fifo, websocket, popen — plus the
 * aggregate entry point and the fan_coro_wake / fan_unref_safe guards
 * that catch late libevent callbacks.
 *
 * Contract exercised
 * ------------------
 * 1. Each per-module clear is safe before any register() (BSS state).
 * 2. Each per-module clear is idempotent.
 * 3. Aggregate fan_clear_lua_states() runs every per-module clear and is
 *    itself safe / idempotent.
 * 4. register -> clear -> register lifecycle works on each module via
 *    the aggregate hook.
 * 5. fan_coro_wake(NULL, co, ref, n) drops the wake silently without
 *    dereferencing co or main_L.
 * 6. fan_unref_safe(NULL, ref) is a no-op; fan_unref_safe(L, LUA_NOREF)
 *    is a no-op.
 *
 * These properties collectively guarantee that a callback firing between
 * fan_clear_lua_states() and fan_loop_cleanup() cannot UAF — the same
 * regression class v1's problem-12/13 addressed. See runtime/coro.h and
 * src/net/tcp.c for the design contract this file pins down.
 */
#include "test_framework.h"
#include "../../../src/net/tcp.h"
#include "../../../src/net/httpd.h"
#include "../../../src/net/http.h"
#include "../../../src/net/udp.h"
#include "../../../src/net/dns.h"
#include "../../../src/net/fifo.h"
#include "../../../src/net/websocket.h"
#include "../../../src/sys/popen.h"
#include "../../../src/runtime/coro.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/* Forward-declared in main.c; also lives in luafan.c. */
extern void fan_clear_lua_states(void);

/* ---- per-module: safe before register --------------------------------- */

TEST_CASE(t_all_clears_safe_before_register) {
    /* From BSS, every g_*_L is NULL. Clearing NULL must be a no-op. */
    fan_tcp_clear_lua_state();
    fan_httpd_clear_lua_state();
    fan_http_clear_lua_state();
    fan_udp_clear_lua_state();
    fan_dns_clear_lua_state();
    fan_fifo_clear_lua_state();
    fan_ws_clear_lua_state();
    fan_popen_clear_lua_state();
    TEST_ASSERT(1);
}

TEST_CASE(t_all_clears_idempotent) {
    for (int i = 0; i < 3; i++) {
        fan_tcp_clear_lua_state();
        fan_httpd_clear_lua_state();
        fan_http_clear_lua_state();
        fan_udp_clear_lua_state();
        fan_dns_clear_lua_state();
        fan_fifo_clear_lua_state();
        fan_ws_clear_lua_state();
        fan_popen_clear_lua_state();
    }
    TEST_ASSERT(1);
}

/* ---- aggregate ---------------------------------------------------------- */

TEST_CASE(t_aggregate_safe_before_register) {
    fan_clear_lua_states();
    TEST_ASSERT(1);
}

TEST_CASE(t_aggregate_idempotent) {
    fan_clear_lua_states();
    fan_clear_lua_states();
    fan_clear_lua_states();
    TEST_ASSERT(1);
}

/* ---- full lifecycle via aggregate -------------------------------------- */

/* Helper: build a minimal `fan` module table on the stack and run every
 * register(). We do NOT go through luaopen_fan because that would pull in
 * fan.evdns / tls / zlib registration which need the event loop; the
 * per-module register calls only touch the Lua stack + their static
 * pointer, which is what we're testing. */
static int register_all_modules(lua_State *L) {
    lua_newtable(L);            /* fake fan table at -1 */
    fan_tcp_register(L);
    fan_fifo_register(L);
    fan_udp_register(L);
    fan_dns_register(L);
    fan_httpd_register(L);
    fan_ws_register(L);
    fan_http_register(L);
    fan_popen_register(L);
    /* fan.tcp, .fifo, .udp, .dns, .httpd_c, .http_c, .popen should exist. */
    lua_getfield(L, -1, "tcp");     int ok = lua_type(L, -1) == LUA_TTABLE; lua_pop(L, 1);
    if (!ok) return 0;
    lua_getfield(L, -1, "fifo");    ok = lua_type(L, -1) == LUA_TTABLE; lua_pop(L, 1);
    if (!ok) return 0;
    lua_getfield(L, -1, "udp");     ok = lua_type(L, -1) == LUA_TTABLE; lua_pop(L, 1);
    if (!ok) return 0;
    lua_getfield(L, -1, "dns");     ok = lua_type(L, -1) == LUA_TTABLE; lua_pop(L, 1);
    if (!ok) return 0;
    lua_getfield(L, -1, "httpd_c"); ok = lua_type(L, -1) == LUA_TTABLE; lua_pop(L, 1);
    if (!ok) return 0;
    lua_getfield(L, -1, "http_c");  ok = lua_type(L, -1) == LUA_TTABLE; lua_pop(L, 1);
    if (!ok) return 0;
    lua_getfield(L, -1, "popen");   ok = lua_type(L, -1) == LUA_TTABLE; lua_pop(L, 1);
    /* websocket doesn't install its own subtable (register only builds a
     * metatable); tolerate its absence. */
    return ok;
}

TEST_CASE(t_full_register_clear_register_lifecycle) {
    /* Two independent lua_States with an aggregate clear in between. Both
     * cycles must succeed and neither may leak on lua_close (ASan catches
     * that separately). */
    lua_State *L1 = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L1);
    luaL_openlibs(L1);
    TEST_ASSERT(register_all_modules(L1));

    fan_clear_lua_states();     /* teardown hook */
    lua_close(L1);

    lua_State *L2 = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L2);
    luaL_openlibs(L2);
    TEST_ASSERT(register_all_modules(L2));

    fan_clear_lua_states();
    lua_close(L2);
    TEST_ASSERT(1);
}

/* ---- fan_coro_wake / fan_unref_safe NULL guards ----------------------- */

TEST_CASE(t_fan_coro_wake_null_main_L_is_noop) {
    /* If fan_coro_wake dereferenced co / main_L in the NULL case we'd
     * crash immediately. The guard turns it into a silent drop. */
    fan_coro_wake(NULL, NULL, LUA_NOREF, 0);
    fan_coro_wake(NULL, (lua_State *)0xdeadbeef, 42, 3);
    TEST_ASSERT(1);
}

TEST_CASE(t_fan_unref_safe_null_or_noref_is_noop) {
    fan_unref_safe(NULL, 42);
    fan_unref_safe(NULL, LUA_NOREF);
    /* With a real state but LUA_NOREF: must not touch the registry. */
    lua_State *L = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L);
    fan_unref_safe(L, LUA_NOREF);
    /* With a real state + real ref: must actually unref. Round-trip
     * through luaL_ref / fan_unref_safe / luaL_ref: if fan_unref_safe
     * really released the slot, the next luaL_ref should reuse it (this
     * is standard luaL_ref freelist behaviour). */
    lua_pushinteger(L, 111);
    int r1 = luaL_ref(L, LUA_REGISTRYINDEX);
    fan_unref_safe(L, r1);
    lua_pushinteger(L, 222);
    int r2 = luaL_ref(L, LUA_REGISTRYINDEX);
    TEST_ASSERT_EQ(r1, r2);  /* freelist reused the freed slot */
    fan_unref_safe(L, r2);
    lua_close(L);
}

/* ---- suite ------------------------------------------------------------- */

static const test_case_t cases[] = {
    {"all_clears_safe_before_register",         t_all_clears_safe_before_register},
    {"all_clears_idempotent",                   t_all_clears_idempotent},
    {"aggregate_safe_before_register",          t_aggregate_safe_before_register},
    {"aggregate_idempotent",                    t_aggregate_idempotent},
    {"full_register_clear_register_lifecycle",  t_full_register_clear_register_lifecycle},
    {"fan_coro_wake_null_main_L_is_noop",       t_fan_coro_wake_null_main_L_is_noop},
    {"fan_unref_safe_null_or_noref_is_noop",    t_fan_unref_safe_null_or_noref_is_noop},
};

const test_suite_t clear_lua_states_suite = {
    .name  = "clear_lua_states",
    .cases = cases,
    .count = (int)(sizeof(cases) / sizeof(cases[0])),
};
