/*
 * evdns.c — LuaFan v2 user-facing DNS base module (fan.evdns).
 *
 * Rewritten from scratch. The public surface (create() plus a userdata
 * with __gc/__tostring) matches v1 `fan.evdns` byte-for-byte so code that
 * migrates from v1 keeps working, but internals share the v2 loop's
 * evdns instead of v1's event_mgr.
 *
 * Failure modes preserved from v1:
 *   * If the caller passes a nameservers arg that we cannot register (all
 *     entries invalid or evdns_base_new fails), we silently fall back to
 *     wrapping the default loop dnsbase. This lets `create()` never fail
 *     at runtime for the common "cloud host with locked-down /etc/resolv.conf"
 *     case — callers get a resolver that at least tries the system's own
 *     configuration.
 *   * A type error on the argument (number, boolean, function, etc.) is
 *     the only path that raises: silently ignoring garbage would hide bugs.
 */
#include "evdns.h"
#include "../platform.h"
#include "../runtime/loop.h"

#include <lauxlib.h>
#include <event2/dns.h>
#include <string.h>
#include <stdlib.h>

#define FAN_EVDNS_MT "fan.evdns"

typedef struct {
    struct evdns_base *base;
    int is_default;   /* 1 => shared with fan_loop_dnsbase(); do NOT free */
} fan_evdns_ud;

/* Add `ns` (a printable IP-with-optional-port) to `base`. Returns 1 on
 * success, 0 on any evdns error. Empty/NULL is skipped. */
static int try_add_nameserver(struct evdns_base *base, const char *ns) {
    if (!ns || *ns == '\0') return 0;
    return evdns_base_nameserver_ip_add(base, ns) == 0 ? 1 : 0;
}

/* Push a fan.evdns userdata wrapping `base`. Ownership: if is_default is
 * 1, __gc will not touch the base (loop owns it). */
static void push_ud(lua_State *L, struct evdns_base *base, int is_default) {
    fan_evdns_ud *u = (fan_evdns_ud *)lua_newuserdata(L, sizeof(*u));
    u->base = base;
    u->is_default = is_default;
    luaL_getmetatable(L, FAN_EVDNS_MT);
    lua_setmetatable(L, -2);
}

/* Push the shared default base as a userdata. Never raises. */
static int push_default(lua_State *L) {
    struct evdns_base *def = fan_loop_dnsbase();
    if (!def) {                                     /* LCOV_EXCL_START */
        /* Only reachable if event_base allocation itself failed, in which
         * case the runtime is unusable; we cannot mock this from a test. */
        lua_pushnil(L);
        lua_pushstring(L, "no default dns base");
        return 2;
    }                                               /* LCOV_EXCL_STOP */
    push_ud(L, def, 1);
    return 1;
}

/* fan.evdns.create([nameservers])
 * nameservers: nil | string | array-of-strings.
 * Returns a fan.evdns userdata; falls back to the default base on any
 * runtime failure to register the requested servers. */
static int l_create(lua_State *L) {
    int top = lua_gettop(L);

    if (top == 0 || lua_isnil(L, 1)) {
        return push_default(L);
    }

    int t = lua_type(L, 1);
    if (t != LUA_TSTRING && t != LUA_TTABLE) {
        return luaL_error(L, "fan.evdns.create: nameservers must be nil, "
                             "a string, or a table of strings");
    }

    struct event_base *ev_base = fan_loop_current_base();
    if (!ev_base) {                                 /* LCOV_EXCL_START */
        /* Same "runtime unusable" story as push_default's fallback. */
        lua_pushnil(L);
        lua_pushstring(L, "no event base");
        return 2;
    }                                               /* LCOV_EXCL_STOP */

    /* NB: EVDNS_BASE_INITIALIZE_NAMESERVERS reads /etc/resolv.conf up front.
     * We *want* that behaviour: if the caller's custom list all fail to
     * register we still want a functional resolver. We then clear+resume to
     * replace with the caller's list. */
    struct evdns_base *base = evdns_base_new(ev_base, EVDNS_BASE_INITIALIZE_NAMESERVERS);
    if (!base) {                                    /* LCOV_EXCL_LINE */
        /* Fresh base creation failed entirely; fall back to default.
         * evdns_base_new only fails on OOM, so this is untestable. */
        return push_default(L);                     /* LCOV_EXCL_LINE */
    }

    /* Match the loop-default tuning so a wedged custom NS fails fast. */
    evdns_base_set_option(base, "timeout:", "2");
    evdns_base_set_option(base, "attempts:", "2");
    evdns_base_set_option(base, "max-timeouts:", "2");

    /* Wipe system-derived servers so only the caller's list is consulted.
     * suspend/resume brackets the mutation as evdns docs recommend. */
    evdns_base_clear_nameservers_and_suspend(base);

    int added = 0;
    if (t == LUA_TSTRING) {
        added = try_add_nameserver(base, lua_tostring(L, 1));
    } else {
        /* table: iterate 1..#t */
#if LUA_VERSION_NUM >= 502
        lua_Integer n = luaL_len(L, 1);
#else
        lua_Integer n = (lua_Integer)lua_objlen(L, 1);
#endif
        for (lua_Integer i = 1; i <= n; i++) {
            lua_rawgeti(L, 1, (int)i);
            if (lua_type(L, -1) == LUA_TSTRING) {
                added += try_add_nameserver(base, lua_tostring(L, -1));
            }
            lua_pop(L, 1);
        }
    }

    if (added == 0) {
        /* No valid nameservers registered; free the empty base and hand
         * the caller the default one so their code keeps running. */
        evdns_base_free(base, 0);
        return push_default(L);
    }

    evdns_base_resume(base);
    push_ud(L, base, 0);
    return 1;
}

static int l_gc(lua_State *L) {
    fan_evdns_ud *u = (fan_evdns_ud *)luaL_checkudata(L, 1, FAN_EVDNS_MT);
    if (u->base && !u->is_default) {
        /* fail_requests=0: outstanding lookups get to finish. In practice
         * a live evdns userdata is pinned by its owner as long as it
         * has pending work, so this GC path only fires after quiescence. */
        evdns_base_free(u->base, 0);
    }
    u->base = NULL;
    return 0;
}

static int l_tostring(lua_State *L) {
    fan_evdns_ud *u = (fan_evdns_ud *)luaL_checkudata(L, 1, FAN_EVDNS_MT);
    lua_pushfstring(L, "fan.evdns<%s>", u->is_default ? "default" : "custom");
    return 1;
}

/* Public C accessors (used by tcp/udp/http when they wire the `evdns`
 * option). Return NULL when the value at `idx` is not a fan.evdns. */
struct evdns_base *fan_evdns_get_base(lua_State *L, int idx) {
    if (!lua_getmetatable(L, idx)) return NULL;
    luaL_getmetatable(L, FAN_EVDNS_MT);
    int match = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    if (!match) return NULL;
    fan_evdns_ud *u = (fan_evdns_ud *)lua_touserdata(L, idx);
    return u ? u->base : NULL;
}

/* Not called from Lua; consumed by tcp/udp/http when they wire the
 * `evdns` opt. Exercised end-to-end by those modules' tests once the
 * wire-up lands in later milestones. */
int fan_evdns_is_custom(lua_State *L, int idx) {         /* LCOV_EXCL_START */
    if (!lua_getmetatable(L, idx)) return 0;
    luaL_getmetatable(L, FAN_EVDNS_MT);
    int match = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    if (!match) return 0;
    fan_evdns_ud *u = (fan_evdns_ud *)lua_touserdata(L, idx);
    return (u && u->base && !u->is_default) ? 1 : 0;
}                                                        /* LCOV_EXCL_STOP */

static const luaL_Reg evdns_funcs[] = {
    {"create", l_create},
    {NULL, NULL},
};

void fan_evdns_register(lua_State *L) {
    /* Metatable for fan.evdns userdata. */
    luaL_newmetatable(L, FAN_EVDNS_MT);
    lua_pushcfunction(L, l_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_tostring);
    lua_setfield(L, -2, "__tostring");
    /* Deliberately no __index: the userdata carries no methods; callers
     * pass it as an opaque handle to fan.tcp/udp/http/dns. */
    lua_pop(L, 1);

    /* Module table on top of fan (which the caller left at -1). */
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, evdns_funcs, 0);
#else
    luaL_register(L, NULL, evdns_funcs);
#endif
    lua_setfield(L, -2, "evdns");
}
