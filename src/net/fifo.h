/*
 * fifo.h — LuaFan v2 named-pipe (FIFO) IPC over libevent.
 *
 * v2 fixes over v1: fd sentinel is -1 (fd 0 is a valid stdin fd, not "empty");
 * partial writes are handled by libevent's output buffer; no printf noise.
 *
 * Lua API (fan.fifo):
 *   fan.fifo.open{ name=path, mode="r"|"w" [, create=true] } -> f | nil, err
 *   f:send(data)     -> true | nil, err     (writer)
 *   f:receive([n])   -> data | nil, err     (reader; yields until data/eof)
 *   f:close()
 */
#ifndef FAN2_NET_FIFO_H
#define FAN2_NET_FIFO_H

#include <lua.h>

void fan_fifo_register(lua_State *L);

#endif /* FAN2_NET_FIFO_H */
