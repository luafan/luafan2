/*
 * dns.c — LuaFan v2 asynchronous DNS resolution over libevent's evdns.
 *
 * Lua API (fan.dns):
 *   fan.dns.resolve(host [, port]) -> { ip1, ip2, ... } | nil, err   (yields)
 *     Resolves A/AAAA records for `host` and returns the numeric IP strings.
 *     Must be called from a coroutine (fan.spawn / fan.loop context).
 *
 * Lifetime notes: evdns_getaddrinfo may invoke its callback *synchronously*
 * (e.g. for a numeric IP literal or a cached hit). We cannot resume a coroutine
 * that has not yielded yet, so we track that case and return results directly
 * without yielding. The coroutine registry pin follows the R17/R20 invariant:
 * unref only AFTER the resume completes (done inside fan_coro_wake).
 */
#include "dns.h"
#include "evdns.h"
#include "../platform.h"
#include "../runtime/loop.h"
#include "../runtime/coro.h"

#include <lauxlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <event2/dns.h>
#include <event2/util.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static lua_State *g_dns_L = NULL;

typedef struct {
    lua_State *co;      /* the parked coroutine */
    int co_ref;         /* registry pin */
    int yielded;        /* 1 once the coroutine has actually yielded */
    int done;           /* 1 once the callback has produced a result */
    int nresults;       /* results pushed on co when done synchronously */
} dns_req_t;

/* Push the resolved addresses (or nil,err) onto `co`; returns pushed count. */
static int push_result(lua_State *co, int errcode, struct evutil_addrinfo *ai) {
    if (errcode) {
        lua_pushnil(co);
        lua_pushstring(co, evutil_gai_strerror(errcode));
        return 2;
    }
    lua_newtable(co);
    int idx = 1;
    for (struct evutil_addrinfo *p = ai; p; p = p->ai_next) {
        char host[INET6_ADDRSTRLEN] = {0};
        const char *r = NULL;
        if (p->ai_family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)p->ai_addr;
            r = inet_ntop(AF_INET, &s4->sin_addr, host, sizeof(host));
        } else if (p->ai_family == AF_INET6) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)p->ai_addr;
            r = inet_ntop(AF_INET6, &s6->sin6_addr, host, sizeof(host));
        }
        if (r) {
            lua_pushstring(co, host);
            lua_rawseti(co, -2, idx++);
        }
    }
    return 1;
}

static void dns_cb(int errcode, struct evutil_addrinfo *ai, void *arg) {
    dns_req_t *req = (dns_req_t *)arg;
    lua_State *co = req->co;

    int n = push_result(co, errcode, ai);
    if (ai) evutil_freeaddrinfo(ai);

    if (req->yielded) {
        /* Async path: resume the parked coroutine, then release its pin. */
        int ref = req->co_ref;
        fan_coro_wake(g_dns_L, co, ref, n);
        free(req);
    } else {
        /* Synchronous path: results are already on co's stack; the caller
         * (l_resolve, still executing) will detect done and return them. */
        req->done = 1;
        req->nresults = n;
    }
}

/* fan.dns.resolve(host [, port [, evdns_ud]])
 *
 * The optional third argument accepts a `fan.evdns.create(...)` userdata
 * (M11): when supplied we resolve through that base instead of the shared
 * default, so callers can point specific queries at custom nameservers. */
static int l_resolve(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_optinteger(L, 2, 0);
    if (port < 0 || port > 65535) return luaL_error(L, "port out of range");

    /* Must run in a coroutine: resolution may be async and needs to yield.
     * Check up front so we never start a native request we cannot await. */
    if (!lua_isyieldable(L)) {
        return luaL_error(L, "fan.dns.resolve must be called from a coroutine");
    }

    struct evdns_base *dnsbase = NULL;
    if (!lua_isnoneornil(L, 3)) {
        dnsbase = fan_evdns_get_base(L, 3);
        if (!dnsbase) {
            return luaL_error(L, "fan.dns.resolve: third arg must be a "
                                 "fan.evdns userdata (from fan.evdns.create)");
        }
    } else {
        dnsbase = fan_loop_dnsbase();
    }
    if (!dnsbase) { lua_pushnil(L); lua_pushstring(L, "no dns base"); return 2; }

    dns_req_t *req = (dns_req_t *)calloc(1, sizeof(*req));
    if (!req) return luaL_error(L, "out of memory");
    req->co = L;
    req->co_ref = LUA_NOREF;
    req->yielded = 0;
    req->done = 0;

    char portbuf[8];
    evutil_snprintf(portbuf, sizeof(portbuf), "%d", port);

    struct evutil_addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;       /* both IPv4 and IPv6 */
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    /* No EVUTIL_AI_ADDRCONFIG: on hosts without a globally routable IPv6
     * address it would reject an IPv6 literal like "::1" (EAI_NONAME). We want
     * numeric literals to always resolve to themselves regardless of the host's
     * outbound address configuration. */
    hints.ai_flags = 0;

    /* evdns_getaddrinfo may invoke dns_cb synchronously (numeric literal or
     * cache hit). In that window req->yielded is still 0, so dns_cb only marks
     * req->done and leaves the results on L's stack — it never resumes/frees. */
    struct evdns_getaddrinfo_request *gr =
        evdns_getaddrinfo(dnsbase, host, port > 0 ? portbuf : NULL,
                          &hints, dns_cb, req);

    if (req->done) {
        /* Completed synchronously: results already on L's stack. */
        int n = req->nresults;
        free(req);
        return n;
    }

    if (!gr) {
        /* Failed to start and no callback ran. */
        free(req);
        lua_pushnil(L);
        lua_pushstring(L, "evdns_getaddrinfo failed to start");
        return 2;
    }

    /* Async: park this coroutine and yield; dns_cb will resume it. */
    int ref = fan_coro_park(L);
    req->co_ref = ref;
    req->yielded = 1;
    return lua_yield(L, 0);
}

static const luaL_Reg dns_funcs[] = {
    {"resolve", l_resolve},
    {NULL, NULL},
};

void fan_dns_register(lua_State *L) {
    g_dns_L = fan_coro_main(L);  /* stable main thread, not the require() coroutine */
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, dns_funcs, 0);
#else
    luaL_register(L, NULL, dns_funcs);
#endif
    lua_setfield(L, -2, "dns");
}

void fan_dns_clear_lua_state(void) {
    /* Companion to fan_dns_register — see runtime/coro.h teardown contract.
     * Prevents evdns resolve callbacks from touching a dangling main state
     * after lua_close. */
    g_dns_L = NULL;
}
