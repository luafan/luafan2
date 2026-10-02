/* fan.http_c teardown regression: pending callback request during Lua teardown. */
#include "test_framework.h"
#include "../../../src/luafan.h"
#include "../../../src/runtime/loop.h"
#include "../../../src/net/http.h"

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

static void stop_loop_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    (void)arg;
    fan_loop_break();
}

TEST_CASE(t_http_pending_request_teardown)
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
            char request[1024];
            (void)read(client, request, sizeof(request));
            usleep(200000);
            const char *reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
            (void)write(client, reply, strlen(reply));
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

    char script[1024];
    snprintf(script, sizeof(script),
        "local co = coroutine.create(function() "
        "  fan.http_c.request{url='http://127.0.0.1:%d/', timeout=5, "
        "    onreceive=function() end} "
        "end) "
        "local ok, err = coroutine.resume(co) "
        "assert(ok, err) "
        "assert(coroutine.status(co) == 'suspended')",
        port);
    int rc = luaL_dostring(L, script);
    TEST_ASSERT_EQ(rc, LUA_OK);

    /* First isolate the module's clear hook: callbacks still fire while the
     * Lua state is valid, but g_main_L has already been cleared. */
    fan_http_clear_lua_state();

    struct event *stopper = evtimer_new(fan_loop_base(), stop_loop_cb, NULL);
    TEST_ASSERT_NOT_NULL(stopper);
    struct timeval tv = {0, 500000};
    evtimer_add(stopper, &tv);
    fan_loop_run();
    event_free(stopper);

    /* Then finish the normal embedding teardown. */
    lua_close(L);
    fan_loop_cleanup();
    close(listener);
    int status = 0;
    waitpid(child, &status, 0);
    TEST_ASSERT(WIFEXITED(status));
}

static const test_case_t cases[] = {
    {"http_pending_request_teardown", t_http_pending_request_teardown},
};

const test_suite_t http_clear_lua_state_suite = {
    .name = "http_clear_lua_state",
    .cases = cases,
    .count = (int)(sizeof(cases) / sizeof(cases[0])),
};
