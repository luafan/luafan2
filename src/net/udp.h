/*
 * udp.h — LuaFan v2 asynchronous UDP over libevent.
 *
 * v2 fixes over v1: inet_pton (not deprecated inet_aton); send holds no raw fd
 * across a yield (no TOCTOU); bind_port validated to 0..65535; IPv4 + IPv6;
 * multicast join AND leave (v1 could only join).
 *
 * Lua API (fan.udp):
 *   fan.udp.new([bind_host, bind_port, family]) -> sock | nil, err
 *     bind optional (a client socket omits it). family is "inet"|"inet6";
 *     when omitted it is inferred from bind_host, defaulting to "inet".
 *   sock:sendto(data, host, port)  -> true | nil, err  (host numeric, matches family)
 *   sock:recv()                     -> data, host, port | nil, err  (yields)
 *   sock:getport()                  -> local bound port number | nil
 *   sock:join(group)                -> true | nil, err  (IPv4 or IPv6 multicast)
 *   sock:leave(group)               -> true | nil, err
 *   sock:close()
 */
#ifndef FAN2_NET_UDP_H
#define FAN2_NET_UDP_H

#include <lua.h>

void fan_udp_register(lua_State *L);

#endif /* FAN2_NET_UDP_H */
