/* MariaDB async callback must not touch a closed Lua state. */
#include "test_framework.h"
#include "../../../src/luafan.h"
#include "../../../src/runtime/loop.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <event2/event.h>
#include <sys/wait.h>
#include <unistd.h>

static void stop_probe(evutil_socket_t fd, short what, void *arg)
{
    (void)fd; (void)what; (void)arg;
    fan_loop_break();
}

TEST_CASE(t_mariadb_pending_teardown)
{
    lua_State *L = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    luaL_requiref(L, "fan", luaopen_fan, 1);
    lua_pop(L, 1);

    const char *script =
        "local fan=require('fan') "
        "local db,err=fan.mariadb.connect{unix_socket='/run/mysqld/mysqld.sock',user='root',password=''} "
        "assert(db,err) "
        "local co=coroutine.create(function() db:query_async('SELECT SLEEP(1)') end) "
        "local ok,e=coroutine.resume(co) assert(ok,e) "
        "assert(coroutine.status(co)=='suspended')";
    TEST_ASSERT_EQ(luaL_dostring(L, script), LUA_OK);

    fan_clear_lua_states();
    lua_close(L);

    struct event *stopper = evtimer_new(fan_loop_base(), stop_probe, NULL);
    TEST_ASSERT_NOT_NULL(stopper);
    struct timeval tv = {2, 0};
    evtimer_add(stopper, &tv);
    fan_loop_run();
    event_free(stopper);
    fan_loop_cleanup();
}

static const test_case_t cases[] = {
    {"mariadb_pending_teardown", t_mariadb_pending_teardown},
};

const test_suite_t mariadb_pending_teardown_suite = {
    .name = "mariadb_pending_teardown",
    .cases = cases,
    .count = (int)(sizeof(cases) / sizeof(cases[0])),
};
