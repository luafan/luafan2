/*
 * udp.c — LuaFan v2 asynchronous UDP.
 *
 * v2 fixes over v1:
 *   - inet_pton (not deprecated inet_aton) for all address parsing.
 *   - send is inline; no raw fd is held across any yield (no TOCTOU).
 *   - bind_port / port validated to 0..65535.
 *   - IPv4 + IPv6 sockets, sendto, and multicast join/leave.
 *   - Multicast leave is supported (v1 only joined, never dropped).
 */
#include "udp.h"
#include "../platform.h"
#include "../runtime/loop.h"
#include "../runtime/coro.h"

#include <lauxlib.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <event2/event.h>

#define UDP_MT "fan.udp.sock"
#define UDP_RECV_MAX 65536

static lua_State *g_udp_L = NULL;

typedef struct {
    int fd;                /* -1 sentinel */
    int family;            /* AF_INET or AF_INET6 */
    struct event *ev;      /* persistent read event */
    lua_State *co;         /* coroutine parked in recv(), or NULL */
    int co_ref;
    int closed;
} udp_sock_t;

/* Parse a numeric host into ss for the socket's family.
 * Returns the sockaddr length on success, or 0 on failure. */
static socklen_t parse_dest(int family, const char *host, int port,
                            struct sockaddr_storage *ss) {
    memset(ss, 0, sizeof(*ss));
    if (family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &s4->sin_addr) != 1) return 0;
        return sizeof(*s4);
    } else if (family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons((uint16_t)port);
        if (inet_pton(AF_INET6, host, &s6->sin6_addr) != 1) return 0;
        return sizeof(*s6);
    }
    return 0;
}

static void udp_wake(udp_sock_t *u, int nargs) {
    lua_State *co = u->co; int ref = u->co_ref;
    u->co = NULL; u->co_ref = LUA_NOREF;
    fan_coro_wake(g_udp_L, co, ref, nargs);
}

/* read callback: a datagram is ready; if a coroutine is parked in recv(),
 * read one datagram and resume it with (data, host, port). */
static void udp_readcb(evutil_socket_t fd, short what, void *arg) {
    (void)what;
    udp_sock_t *u = (udp_sock_t *)arg;
    if (!u->co) return;  /* nobody waiting; leave it for the next recv() */

    char buf[UDP_RECV_MAX];
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&ss, &slen);
    lua_State *co = u->co;
    if (n < 0) {
        lua_pushnil(co);
        lua_pushstring(co, strerror(errno));
        udp_wake(u, 2);
        return;
    }
    char host[INET6_ADDRSTRLEN] = {0};
    int port = 0;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &s4->sin_addr, host, sizeof(host));
        port = ntohs(s4->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &s6->sin6_addr, host, sizeof(host));
        port = ntohs(s6->sin6_port);
    }
    lua_pushlstring(co, buf, (size_t)n);
    lua_pushstring(co, host);
    lua_pushinteger(co, port);
    udp_wake(u, 3);
}

static int make_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Determine the address family the caller wants.
 * Priority: explicit family arg > bind_host form > default AF_INET. */
static int pick_family(const char *family_str, const char *bind_host) {
    if (family_str) {
        if (strcmp(family_str, "inet6") == 0) return AF_INET6;
        if (strcmp(family_str, "inet") == 0) return AF_INET;
        return -1;
    }
    if (bind_host && bind_host[0]) {
        struct in6_addr a6;
        if (inet_pton(AF_INET6, bind_host, &a6) == 1) return AF_INET6;
        return AF_INET;  /* assume IPv4 (validated later at bind) */
    }
    return AF_INET;
}

/* fan.udp.new([bind_host, bind_port, family]) */
static int l_new(lua_State *L) {
    const char *bind_host = luaL_optstring(L, 1, NULL);
    int bind_port = (int)luaL_optinteger(L, 2, -1);
    const char *family_str = luaL_optstring(L, 3, NULL);
    if (bind_port != -1 && (bind_port < 0 || bind_port > 65535)) {
        return luaL_error(L, "bind_port out of range");
    }
    int family = pick_family(family_str, bind_host);
    if (family < 0) return luaL_error(L, "family must be 'inet' or 'inet6'");

    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) { lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2; }
    make_nonblocking(fd);
    /* Allow quick rebinding for tests / multicast receivers. */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind_port != -1) {
        struct sockaddr_storage ss;
        socklen_t slen = 0;
        memset(&ss, 0, sizeof(ss));
        if (family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
            sin->sin_family = AF_INET;
            sin->sin_port = htons((uint16_t)bind_port);
            if (!bind_host || !bind_host[0]) {
                sin->sin_addr.s_addr = htonl(INADDR_ANY);
            } else if (inet_pton(AF_INET, bind_host, &sin->sin_addr) != 1) {
                close(fd);
                lua_pushnil(L); lua_pushstring(L, "bind_host must be numeric IPv4");
                return 2;
            }
            slen = sizeof(*sin);
        } else {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
            sin6->sin6_family = AF_INET6;
            sin6->sin6_port = htons((uint16_t)bind_port);
            if (!bind_host || !bind_host[0]) {
                sin6->sin6_addr = in6addr_any;
            } else if (inet_pton(AF_INET6, bind_host, &sin6->sin6_addr) != 1) {
                close(fd);
                lua_pushnil(L); lua_pushstring(L, "bind_host must be numeric IPv6");
                return 2;
            }
            slen = sizeof(*sin6);
        }
        if (bind(fd, (struct sockaddr *)&ss, slen) != 0) {
            close(fd);
            lua_pushnil(L); lua_pushfstring(L, "bind failed: %s", strerror(errno));
            return 2;
        }
    }

    struct event_base *base = fan_loop_current_base();
    udp_sock_t *u = (udp_sock_t *)lua_newuserdata(L, sizeof(*u));
    memset(u, 0, sizeof(*u));
    u->fd = fd; u->family = family; u->co_ref = LUA_NOREF;
    luaL_getmetatable(L, UDP_MT);
    lua_setmetatable(L, -2);

    u->ev = event_new(base, fd, EV_READ | EV_PERSIST, udp_readcb, u);
    if (!u->ev) { close(fd); u->fd = -1; lua_pushnil(L); lua_pushstring(L, "event_new failed"); return 2; }
    event_add(u->ev, NULL);
    return 1;
}

/* sock:sendto(data, host, port) */
static int l_sendto(lua_State *L) {
    udp_sock_t *u = (udp_sock_t *)luaL_checkudata(L, 1, UDP_MT);
    size_t len; const char *data = luaL_checklstring(L, 2, &len);
    const char *host = luaL_checkstring(L, 3);
    int port = (int)luaL_checkinteger(L, 4);
    if (u->closed || u->fd < 0) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    if (port < 0 || port > 65535) return luaL_error(L, "port out of range");

    struct sockaddr_storage dst;
    socklen_t dlen = parse_dest(u->family, host, port, &dst);
    if (dlen == 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "host must be numeric %s",
                        u->family == AF_INET6 ? "IPv6" : "IPv4");
        return 2;
    }
    /* send inline: no fd is held across any yield, so no TOCTOU (v1 fix) */
    ssize_t n = sendto(u->fd, data, len, 0, (struct sockaddr *)&dst, dlen);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2; }
    lua_pushboolean(L, 1);
    return 1;
}

/* internal join/leave; add=1 join, add=0 leave. group is numeric. */
static int membership(lua_State *L, int add) {
    udp_sock_t *u = (udp_sock_t *)luaL_checkudata(L, 1, UDP_MT);
    const char *group = luaL_checkstring(L, 2);
    if (u->closed || u->fd < 0) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }

    if (u->family == AF_INET) {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        if (inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) {
            lua_pushnil(L); lua_pushstring(L, "group must be numeric IPv4"); return 2;
        }
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        int opt = add ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP;
        if (setsockopt(u->fd, IPPROTO_IP, opt, &mreq, sizeof(mreq)) < 0) {
            lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2;
        }
    } else {
        struct ipv6_mreq mreq6;
        memset(&mreq6, 0, sizeof(mreq6));
        if (inet_pton(AF_INET6, group, &mreq6.ipv6mr_multiaddr) != 1) {
            lua_pushnil(L); lua_pushstring(L, "group must be numeric IPv6"); return 2;
        }
        mreq6.ipv6mr_interface = 0;  /* default interface */
        int opt = add ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP;
        if (setsockopt(u->fd, IPPROTO_IPV6, opt, &mreq6, sizeof(mreq6)) < 0) {
            lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* sock:join(group) -> true | nil, err */
static int l_join(lua_State *L)  { return membership(L, 1); }
/* sock:leave(group) -> true | nil, err */
static int l_leave(lua_State *L) { return membership(L, 0); }

/* sock:recv() -> data, host, port | nil, err */
static int l_recv(lua_State *L) {
    udp_sock_t *u = (udp_sock_t *)luaL_checkudata(L, 1, UDP_MT);
    if (u->closed || u->fd < 0) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    if (u->co) return luaL_error(L, "recv already in progress");

    int ref = fan_coro_park(L);
    if (ref == LUA_NOREF) return luaL_error(L, "recv must be called from a coroutine");
    u->co = L; u->co_ref = ref;
    return lua_yield(L, 0);
}

/* sock:getport() -> local bound port | nil */
static int l_getport(lua_State *L) {
    udp_sock_t *u = (udp_sock_t *)luaL_checkudata(L, 1, UDP_MT);
    if (u->closed || u->fd < 0) {
        lua_pushnil(L);
        return 1;
    }

    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    if (getsockname(u->fd, (struct sockaddr *)&ss, &slen) != 0) {
        lua_pushnil(L);
        return 1;
    }
    if (ss.ss_family == AF_INET) {
        lua_pushinteger(L, ntohs(((struct sockaddr_in *)&ss)->sin_port));
    } else if (ss.ss_family == AF_INET6) {
        lua_pushinteger(L, ntohs(((struct sockaddr_in6 *)&ss)->sin6_port));
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* sock:close() */
static int l_close(lua_State *L) {
    udp_sock_t *u = (udp_sock_t *)luaL_checkudata(L, 1, UDP_MT);
    if (!u->closed) {
        u->closed = 1;
        if (u->ev) { event_free(u->ev); u->ev = NULL; }
        if (u->fd >= 0) { close(u->fd); u->fd = -1; }
        /* If a coroutine is parked in recv(), wake it with (nil, "closed") so it
         * does not hang forever on a socket that will never deliver again.
         * Do this AFTER marking closed and freeing the event/fd. */
        if (u->co) {
            lua_State *co = u->co;
            lua_pushnil(co);
            lua_pushstring(co, "closed");
            udp_wake(u, 2);
        }
    }
    return 0;
}

static int udp_gc(lua_State *L) {
    udp_sock_t *u = (udp_sock_t *)luaL_checkudata(L, 1, UDP_MT);
    if (u->ev) { event_free(u->ev); u->ev = NULL; }
    if (u->fd >= 0) { close(u->fd); u->fd = -1; }
    return 0;
}

static const luaL_Reg udp_methods[] = {
    {"sendto", l_sendto},
    {"recv",   l_recv},
    {"getport", l_getport},
    {"join",   l_join},
    {"leave",  l_leave},
    {"close",  l_close},
    {NULL, NULL},
};

static const luaL_Reg udp_funcs[] = {
    {"new", l_new},
    {NULL, NULL},
};

void fan_udp_register(lua_State *L) {
    g_udp_L = fan_coro_main(L);  /* stable main thread, not the require() coroutine */
    luaL_newmetatable(L, UDP_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, udp_methods, 0);
#else
    luaL_register(L, NULL, udp_methods);
#endif
    lua_pushcfunction(L, udp_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, udp_funcs, 0);
#else
    luaL_register(L, NULL, udp_funcs);
#endif
    lua_setfield(L, -2, "udp");
}

void fan_udp_clear_lua_state(void) {
    /* Companion to fan_udp_register — see runtime/coro.h teardown contract.
     * Prevents libevent recv callbacks from resuming a coroutine on a freed
     * lua_State after lua_close. */
    g_udp_L = NULL;
}
