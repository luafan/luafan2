/* fan.udp teardown regression probe. */
#include "test_framework.h"
#include "../../../src/luafan.h"
#include "../../../src/runtime/loop.h"
#include "../../../src/net/udp.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <event2/event.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void stop_udp_probe(evutil_socket_t fd, short what, void *arg)
{
    (void)fd; (void)what; (void)arg;
    fan_loop_break();
}

TEST_CASE(t_udp_pending_recv_teardown)
{
    const int port = 28761;
    pid_t child = fork();
    TEST_ASSERT(child >= 0);
    if (child == 0) {
        usleep(150000);
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            (void)sendto(fd, "x", 1, 0, (struct sockaddr *)&a, sizeof(a));
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
        "local s=assert(fan.udp.new('127.0.0.1',%d)) "
        "local co=coroutine.create(function() s:recv() end) "
        "local ok,e=coroutine.resume(co) assert(ok,e) "
        "assert(coroutine.status(co)=='suspended')", port);
    TEST_ASSERT_EQ(luaL_dostring(L, script), LUA_OK);

    fan_udp_clear_lua_state();
    lua_close(L);
    struct event *stopper = evtimer_new(fan_loop_base(), stop_udp_probe, NULL);
    TEST_ASSERT_NOT_NULL(stopper);
    struct timeval tv = {1, 0};
    evtimer_add(stopper, &tv);
    fan_loop_run();
    event_free(stopper);
    fan_loop_cleanup();
    int status = 0;
    waitpid(child, &status, 0);
    TEST_ASSERT(WIFEXITED(status));
}

static const test_case_t cases[] = {
    {"udp_pending_recv_teardown", t_udp_pending_recv_teardown},
};

const test_suite_t udp_clear_lua_state_suite = {
    .name = "udp_clear_lua_state",
    .cases = cases,
    .count = (int)(sizeof(cases) / sizeof(cases[0])),
};
