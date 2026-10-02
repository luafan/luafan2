/* FIFO pending receive must not touch a closed Lua state. */
#include "test_framework.h"
#include "../../../src/luafan.h"
#include "../../../src/runtime/loop.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <event2/event.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

static void stop_probe(evutil_socket_t fd, short what, void *arg)
{
    (void)fd; (void)what; (void)arg;
    fan_loop_break();
}

TEST_CASE(t_fifo_pending_teardown)
{
    char path[128];
    snprintf(path, sizeof(path), "/tmp/lf2_fifo_teardown_%ld", (long)getpid());
    unlink(path);
    TEST_ASSERT(mkfifo(path, 0600) == 0);

    pid_t child = fork();
    TEST_ASSERT(child >= 0);
    if (child == 0) {
        int fd = -1;
        for (int i = 0; i < 20 && fd < 0; i++) {
            fd = open(path, O_WRONLY | O_NONBLOCK);
            if (fd < 0) usleep(10000);
        }
        if (fd >= 0) {
            usleep(200000);
            (void)write(fd, "x", 1);
            close(fd);
        }
        _exit(0);
    }

    lua_State *L = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    luaL_requiref(L, "fan", luaopen_fan, 1);
    lua_pop(L, 1);
    char script[512];
    snprintf(script, sizeof(script),
        "local f=assert(fan.fifo.open{ name='%s', mode='r' }) "
        "local co=coroutine.create(function() f:receive() end) "
        "local ok,e=coroutine.resume(co) assert(ok,e) "
        "assert(coroutine.status(co)=='suspended')", path);
    TEST_ASSERT_EQ(luaL_dostring(L, script), LUA_OK);

    fan_clear_lua_states();
    lua_close(L);
    struct event *stopper = evtimer_new(fan_loop_base(), stop_probe, NULL);
    TEST_ASSERT_NOT_NULL(stopper);
    struct timeval tv = {1, 0};
    evtimer_add(stopper, &tv);
    fan_loop_run();
    event_free(stopper);
    fan_loop_cleanup();
    int status = 0;
    waitpid(child, &status, 0);
    unlink(path);
    TEST_ASSERT(WIFEXITED(status));
}

static const test_case_t cases[] = {
    {"fifo_pending_teardown", t_fifo_pending_teardown},
};

const test_suite_t fifo_pending_teardown_suite = {
    .name = "fifo_pending_teardown",
    .cases = cases,
    .count = (int)(sizeof(cases) / sizeof(cases[0])),
};
