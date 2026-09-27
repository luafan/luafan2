/*
 * dns.h — LuaFan v2 asynchronous DNS resolution over libevent evdns.
 *
 * Lua API (fan.dns):
 *   fan.dns.resolve(host [, port]) -> { ip1, ip2, ... } | nil, err  (yields)
 *     Must be called from a coroutine. Returns numeric IPv4/IPv6 strings.
 */
#ifndef FAN2_NET_DNS_H
#define FAN2_NET_DNS_H

#include <lua.h>

void fan_dns_register(lua_State *L);

#endif /* FAN2_NET_DNS_H */
