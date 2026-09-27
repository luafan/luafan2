/*
 * httpd.h — LuaFan v2 HTTP/1.1 server built on libevent evhttp.
 *
 * M14.C-a scope (this file):
 *   - fan.httpd_c.bind{ host, port, handler = fn(req) } -> server userdata
 *   - request:method / :path / :query / :headers / :body / :available / :read
 *   - response methods live on the same request userdata (v1 flat surface):
 *       response:reply(status, headers, body)
 *       response:addheader(name, value)              (M14.C-b will land)
 *       response:reply_start(status, headers)        (M14.C-b will land)
 *       response:reply_chunk(data)                   (M14.C-b will land)
 *       response:reply_end()                         (M14.C-b will land)
 *   - handler runs in a fresh Lua coroutine so fan.sleep can yield across
 *     I/O; the same C-side accept pattern as fan.tcp.bind.
 *   - HTTPS / WebSocket upgrade are OUT OF SCOPE for M14.C-a; the Lua-side
 *     dispatch shim (lua/fan/httpd.lua) picks the pure-Lua backend
 *     (httpd_lua.lua) when opts.ssl is set.
 *
 * Later milestones will grow this: M14.C-b chunked + addheader,
 * M14.C-c HTTPS via evhttp_new_bufferevent_cb, M14.C-d WebSocket upgrade
 * bridge into fan.websocket.
 */
#ifndef FAN_NET_HTTPD_H
#define FAN_NET_HTTPD_H

#include <lua.h>

/* Registers the fan.httpd_c submodule on the fan table left at -1 by
 * luaopen_fan. The Lua-side dispatch shim (lua/fan/httpd.lua) requires
 * this and picks between C / pure-Lua backends per bind call. */
void fan_httpd_register(lua_State *L);

#endif /* FAN_NET_HTTPD_H */
