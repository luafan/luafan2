/* fan.tcp pending connect/receive teardown regression probe. */
#include "test_framework.h"
#include "../../../src/luafan.h"
#include "../../../src/runtime/loop.h"
#include "../../../src/net/tcp.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <event2/event.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static void stop_tcp_probe(evutil_socket_t fd, short what, void *arg)
{
    (void)fd; (void)what; (void)arg;
    fan_loop_break();
}

TEST_CASE(t_tcp_pending_receive_teardown)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(listener >= 0);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    TEST_ASSERT(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    TEST_ASSERT(listen(listener, 1) == 0);
    socklen_t alen = sizeof(addr);
    TEST_ASSERT(getsockname(listener, (struct sockaddr *)&addr, &alen) == 0);
    int port = ntohs(addr.sin_port);

    pid_t child = fork();
    TEST_ASSERT(child >= 0);
    if (child == 0) {
        int client = accept(listener, NULL, NULL);
        if (client >= 0) {
            usleep(200000);
            (void)write(client, "x", 1);
            close(client);
        }
        close(listener);
        _exit(0);
    }

    lua_State *L = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    luaL_requiref(L, "fan", luaopen_fan, 1);
    lua_pop(L, 1);
    char script[768];
    snprintf(script, sizeof(script),
        "local co=coroutine.create(function() "
        "local c=assert(fan.tcp.connect('127.0.0.1',%d)) c:receive() end) "
        "local ok,e=coroutine.resume(co) assert(ok,e) "
        "assert(coroutine.status(co)=='suspended')", port);
    TEST_ASSERT_EQ(luaL_dostring(L, script), LUA_OK);

    fan_tcp_clear_lua_state();
    lua_close(L);
    struct event *stopper = evtimer_new(fan_loop_base(), stop_tcp_probe, NULL);
    TEST_ASSERT_NOT_NULL(stopper);
    struct timeval tv = {1, 0};
    evtimer_add(stopper, &tv);
    fan_loop_run();
    event_free(stopper);
    fan_loop_cleanup();
    close(listener);
    int status = 0;
    waitpid(child, &status, 0);
    TEST_ASSERT(WIFEXITED(status));
}

static const test_case_t cases[] = {
    {"tcp_pending_receive_teardown", t_tcp_pending_receive_teardown},
};

const test_suite_t tcp_pending_teardown_suite = {
    .name = "tcp_pending_teardown",
    .cases = cases,
    .count = (int)(sizeof(cases) / sizeof(cases[0])),
};
