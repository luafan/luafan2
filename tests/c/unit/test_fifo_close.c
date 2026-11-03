/* FIFO close must wake a pending receive and release its coroutine pin. */
#include "test_framework.h"
#include "../../../src/luafan.h"
#include "../../../src/runtime/loop.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>

TEST_CASE(t_fifo_close_wakes_pending_receive)
{
    char path[128];
    snprintf(path, sizeof(path), "/tmp/lf2_fifo_close_%ld", (long)getpid());
    unlink(path);
    TEST_ASSERT(mkfifo(path, 0600) == 0);

    lua_State *L = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    luaL_requiref(L, "fan", luaopen_fan, 1);
    lua_pop(L, 1);

    char script[768];
    snprintf(script, sizeof(script),
        "local f=assert(fan.fifo.open{ name='%s', mode='r' }) "
        "local co=coroutine.create(function() "
        "  local a,b=f:receive() assert(a==nil and b=='closed') "
        "end) "
        "local ok,e=coroutine.resume(co) assert(ok,e) "
        "assert(coroutine.status(co)=='suspended') "
        "f:close() "
        "assert(coroutine.status(co)=='dead')",
        path);
    TEST_ASSERT_EQ(luaL_dostring(L, script), LUA_OK);

    lua_close(L);
    fan_loop_cleanup();
    unlink(path);
}

static const test_case_t cases[] = {
    {"fifo_close_wakes_pending_receive", t_fifo_close_wakes_pending_receive},
};

const test_suite_t fifo_close_suite = {
    .name = "fifo_close",
    .cases = cases,
    .count = (int)(sizeof(cases) / sizeof(cases[0])),
};
