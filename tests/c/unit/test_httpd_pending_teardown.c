/* HTTPD yielded handler must not resume after Lua teardown. */
#include "test_framework.h"
#include "../../../src/luafan.h"
#include "../../../src/runtime/loop.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <event2/event.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static void stop_probe(evutil_socket_t fd, short what, void *arg)
{
    (void)fd; (void)what; (void)arg;
    fan_loop_break();
}

TEST_CASE(t_httpd_pending_teardown)
{
    lua_State *L = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    luaL_requiref(L, "fan", luaopen_fan, 1);
    lua_pop(L, 1);

    const char *script =
        "local fan=require('fan') "
        "assert(fan.httpd_c.bind{host='127.0.0.1',port=24672,handler=function(req) fan.sleep(1) end})";
    TEST_ASSERT_EQ(luaL_dostring(L, script), LUA_OK);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(client >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(24672);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT(connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    const char request[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    (void)send(client, request, sizeof(request) - 1, 0);

    fan_clear_lua_states();
    lua_close(L);
    struct event *stopper = evtimer_new(fan_loop_base(), stop_probe, NULL);
    TEST_ASSERT_NOT_NULL(stopper);
    struct timeval tv = {2, 0};
    evtimer_add(stopper, &tv);
    fan_loop_run();
    event_free(stopper);
    close(client);
    fan_loop_cleanup();
}

static const test_case_t cases[] = {
    {"httpd_pending_teardown", t_httpd_pending_teardown},
};

const test_suite_t httpd_pending_teardown_suite = {
    .name = "httpd_pending_teardown",
    .cases = cases,
    .count = (int)(sizeof(cases) / sizeof(cases[0])),
};
