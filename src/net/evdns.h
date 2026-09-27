/*
 * evdns.h — LuaFan v2 user-facing DNS base module.
 *
 * Wraps a libevent `struct evdns_base *` in a Lua userdata so callers can
 * spin up a resolver with custom nameservers and later pass it to
 * `fan.tcp.connect`, `fan.udp.new`, or `fan.dns.resolve` as the `evdns`
 * option (wired site-by-site as those modules land).
 *
 * Lua API (fan.evdns):
 *   fan.evdns.create()                 -> userdata (wraps the default base)
 *   fan.evdns.create(nil)              -> same as above
 *   fan.evdns.create("1.1.1.1")        -> userdata bound to a single NS
 *   fan.evdns.create({"1.1.1.1","8.8.8.8"}) -> multi-NS resolver
 *
 * Fallback rule (kept identical to v1 fan.evdns): if every candidate
 * nameserver fails to register, the returned userdata silently falls back
 * to wrapping the loop's default base so the caller keeps a usable
 * resolver rather than a broken one.
 *
 * The metatable's __gc frees the base only when it was created by
 * `evdns.create` (`is_default == 0`). The default base is shared with
 * the rest of the runtime and is freed by fan_loop_cleanup().
 */
#ifndef FAN2_NET_EVDNS_H
#define FAN2_NET_EVDNS_H

#include <lua.h>

struct evdns_base;

/* Register `fan.evdns` on the fan table currently at the top of L's stack. */
void fan_evdns_register(lua_State *L);

/* If the value at `idx` is a fan.evdns userdata, return its dnsbase;
 * otherwise return NULL. Does not raise. */
struct evdns_base *fan_evdns_get_base(lua_State *L, int idx);

/* Return 1 if the userdata at `idx` wraps a non-default (custom) base. */
int fan_evdns_is_custom(lua_State *L, int idx);

#endif /* FAN2_NET_EVDNS_H */
