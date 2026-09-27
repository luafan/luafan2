/*
 * tcp.h — LuaFan v2 asynchronous TCP (client + server) over libevent bufferevent.
 *
 * Lua API (registered onto the `fan` table under `fan.tcp`):
 *   fan.tcp.connect(host, port[, opts]) -> conn | nil, err   (yields until connected)
 *   conn:send(data)                     -> true | nil, err   (queues; yields if backpressured is future work)
 *   conn:receive([n])                   -> data | nil, err   (yields until >=1 byte or n bytes or EOF/err)
 *   conn:close()
 *   fan.tcp.bind(host, port, on_accept) -> server | nil, err
 *     on_accept(conn) is spawned as a coroutine per accepted connection
 *   server:close()
 */
#ifndef FAN2_NET_TCP_H
#define FAN2_NET_TCP_H

#include <lua.h>

/* Registers fan.tcp onto the module table at stack top (-1). */
void fan_tcp_register(lua_State *L);

#endif /* FAN2_NET_TCP_H */
