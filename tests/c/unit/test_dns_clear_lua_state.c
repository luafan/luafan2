/* fan.dns teardown regression probe. */
#include "test_framework.h"
#include "../../../src/luafan.h"
#include "../../../src/runtime/loop.h"
#include "../../../src/net/dns.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <event2/event.h>

static void stop_dns_probe(evutil_socket_t fd, short what, void *arg)
{
    (void)fd; (void)what; (void)arg;
    fan_loop_break();
}

TEST_CASE(t_dns_pending_request_teardown)
{
    lua_State *L = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    luaL_requiref(L, "fan", luaopen_fan, 1);
    lua_pop(L, 1);

    const char *script =
        "local co = coroutine.create(function() "
        "  fan.dns.resolve('lf2-teardown-nonexistent.invalid') "
        "end) "
        "local ok, err = coroutine.resume(co) "
        "assert(ok, err) "
        "assert(coroutine.status(co) == 'suspended')";
    TEST_ASSERT_EQ(luaL_dostring(L, script), LUA_OK);

    fan_dns_clear_lua_state();
    lua_close(L);
    struct event *stopper = evtimer_new(fan_loop_base(), stop_dns_probe, NULL);
    TEST_ASSERT_NOT_NULL(stopper);
    struct timeval tv = {3, 0};
    evtimer_add(stopper, &tv);
    fan_loop_run();
    event_free(stopper);
    fan_loop_cleanup();
}

static const test_case_t cases[] = {
    {"dns_pending_request_teardown", t_dns_pending_request_teardown},
};

const test_suite_t dns_clear_lua_state_suite = {
    .name = "dns_clear_lua_state",
    .cases = cases,
    .count = (int)(sizeof(cases) / sizeof(cases[0])),
};
