/*
 * http.h — LuaFan v2 HTTP/1.1 client (libcurl multi + libevent backend).
 *
 * Milestone M13.C. The Lua-side dispatcher lua/fan/http.lua picks between
 * this C backend and the pure-Lua fallback (lua/fan/http_lua.lua) based on
 * opts.backend and _G.__FAN_HTTP_BACKEND_DEFAULT, mirroring how fan.httpd
 * dispatches between httpd.c and httpd_lua.lua.
 *
 * Only compiled when FAN_WITH_CURL=1.
 */
#ifndef LUAFAN2_NET_HTTP_H
#define LUAFAN2_NET_HTTP_H

#include <lua.h>

/* Registers the `fan.http_c` submodule on the module table at -1.
 * Safe to call even without libcurl: expands to a no-op stub table with a
 * single `available=false` flag so lua/fan/http.lua can detect it. */
void fan_http_register(lua_State *L);

#endif
