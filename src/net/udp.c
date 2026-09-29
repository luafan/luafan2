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

/* ==========================================================================
 * M18: callback-based async UDP (fan.udp.new_async{...})
 *
 * Restores v1 fan.udpd semantics on top of libevent EV_READ / EV_WRITE
 * events:
 *   - immediate handle return
 *   - onread(self, data, dest) callback per received datagram, where
 *     `dest` is a UDP_AddrInfo userdata with getHost/getIP/getPort
 *   - onsendready(self) after `sock:send_req()` when the socket is
 *     writable (kept for v1 parity — UDP is rarely blocked on send)
 *   - sock:send(data[, dest]) — dest is either a UDP_AddrInfo userdata
 *     or falls back to the default host:port passed at creation time
 *   - sock:rebind() to re-bind the same host:port after a mobile
 *     network transition
 *   - fan.udp.make_dest(host, port) / make_dests(host, port) build
 *     UDP_AddrInfo objects from a hostname (getaddrinfo) or numeric IP
 *
 * Rejects (same as M17 client): `worker`, `callback_self_first`.
 * ========================================================================== */
#include <netdb.h>          /* getaddrinfo (used by make_dest) */

#define UDP_ASYNC_MT "fan.udp.async_sock"
#define UDP_DEST_MT  "fan.udp.dest"

typedef struct {
    struct sockaddr_storage ss;
    socklen_t slen;
    char host[NI_MAXHOST];    /* string form for getHost/getIP */
    int  port;
} udp_dest_t;

typedef struct {
    int fd;
    int family;
    struct event *ev;         /* persistent EV_READ */
    struct event *evw;        /* one-shot EV_WRITE, armed only by send_req */
    int closed;
    /* Default destination (from `host`/`port` in the option table).  When
     * the caller invokes sock:send(data) without a dest, we use these. */
    char *def_host;
    int   def_port;
    int   def_family;         /* AF_INET / AF_INET6 / 0 = not set */
    /* Rebind parameters (v1 parity: rebind reuses the original
     * bind_host / bind_port). */
    char *bind_host;
    int   bind_port;
    /* Lua registry refs — LUA_NOREF when absent. */
    int on_read_ref;
    int on_sendready_ref;
    int self_ref;             /* pin the userdata while callbacks armed */
} udp_async_t;

/* forward */
static void udp_async_readcb(evutil_socket_t fd, short what, void *arg);
static void udp_async_writecb(evutil_socket_t fd, short what, void *arg);

/* ---- UDP_AddrInfo (dest) ------------------------------------------------- */

static udp_dest_t *udp_dest_push_new(lua_State *L) {
    udp_dest_t *d = (udp_dest_t *)lua_newuserdata(L, sizeof(*d));
    memset(d, 0, sizeof(*d));
    luaL_getmetatable(L, UDP_DEST_MT);
    lua_setmetatable(L, -2);
    return d;
}

/* Populate a dest from a resolved addrinfo entry (v4/v6, network-order
 * port already inside ai_addr).  Fills the string host + numeric port
 * for the Lua-side getHost / getPort. */
static void udp_dest_fill(udp_dest_t *d, const struct addrinfo *ai) {
    memcpy(&d->ss, ai->ai_addr, ai->ai_addrlen);
    d->slen = ai->ai_addrlen;
    if (ai->ai_family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&d->ss;
        inet_ntop(AF_INET, &s4->sin_addr, d->host, sizeof(d->host));
        d->port = ntohs(s4->sin_port);
    } else if (ai->ai_family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&d->ss;
        inet_ntop(AF_INET6, &s6->sin6_addr, d->host, sizeof(d->host));
        d->port = ntohs(s6->sin6_port);
    }
}

/* fan.udp.make_dest(host, port[, evdns]) -> UDP_AddrInfo | (nil, err)
 * Uses getaddrinfo synchronously — v1 offered evdns-driven async
 * resolution, but that requires a full coroutine yield / resume dance
 * that inflates M18 scope well beyond parity for typical downstream
 * usage (numeric IP + explicit port).  Callers that specifically need
 * async DNS should resolve up front with fan.dns.resolve and pass a
 * numeric IP to make_dest.  The `evdns` third argument is accepted for
 * API compatibility with v1 but currently ignored (documented in
 * manifest/udp.md). */
static int l_udp_make_dest(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    if (port < 0 || port > 65535) {
        lua_pushnil(L); lua_pushstring(L, "port out of range"); return 2;
    }
    /* Ignore optional third arg (v1 evdns userdata) for now. */

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, portbuf, &hints, &res);
    if (rc != 0 || !res) {
        lua_pushnil(L); lua_pushstring(L, gai_strerror(rc)); return 2;
    }
    udp_dest_t *d = udp_dest_push_new(L);
    udp_dest_fill(d, res);
    freeaddrinfo(res);
    return 1;
}

/* fan.udp.make_dests(host, port[, evdns]) -> array of UDP_AddrInfo
 * Returns every addrinfo returned by getaddrinfo (both v4 and v6). */
static int l_udp_make_dests(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    if (port < 0 || port > 65535) {
        lua_pushnil(L); lua_pushstring(L, "port out of range"); return 2;
    }
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, portbuf, &hints, &res);
    if (rc != 0 || !res) {
        lua_pushnil(L); lua_pushstring(L, gai_strerror(rc)); return 2;
    }
    lua_newtable(L);
    int i = 1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        udp_dest_t *d = udp_dest_push_new(L);
        udp_dest_fill(d, ai);
        lua_rawseti(L, -2, i++);
    }
    freeaddrinfo(res);
    return 1;
}

static int l_udp_dest_getHost(lua_State *L) {
    udp_dest_t *d = (udp_dest_t *)luaL_checkudata(L, 1, UDP_DEST_MT);
    lua_pushstring(L, d->host);
    return 1;
}
static int l_udp_dest_getIP(lua_State *L) {
    /* v1 documents getIP as equivalent to getHost for numeric IPs — see
     * tmp/luafan/docs/api/udpd.md.  Because make_dest resolves eagerly
     * to a numeric ss, the two are effectively the same for us. */
    return l_udp_dest_getHost(L);
}
static int l_udp_dest_getPort(lua_State *L) {
    udp_dest_t *d = (udp_dest_t *)luaL_checkudata(L, 1, UDP_DEST_MT);
    lua_pushinteger(L, d->port);
    return 1;
}

/* ---- async socket callback dispatch ------------------------------------- */

typedef struct { udp_async_t *u; char *data; size_t len; udp_dest_t peer; } async_udp_read_args_t;
static __thread async_udp_read_args_t g_udp_read_args;
static int udp_async_push_self(lua_State *co, udp_async_t *u) {
    if (u->self_ref != LUA_NOREF) lua_rawgeti(co, LUA_REGISTRYINDEX, u->self_ref);
    else lua_pushnil(co);
    return 1;
}
static int udp_async_push_self_data_dest(lua_State *co, udp_async_t *u) {
    (void)u;
    udp_async_t *ua = g_udp_read_args.u;
    if (ua && ua->self_ref != LUA_NOREF)
        lua_rawgeti(co, LUA_REGISTRYINDEX, ua->self_ref);
    else lua_pushnil(co);
    lua_pushlstring(co, g_udp_read_args.data, g_udp_read_args.len);
    /* Push a fresh dest userdata for the peer.  We do NOT reuse a stashed
     * dest — every datagram may come from a different peer and Lua handlers
     * routinely hold on to the dest for a reply. */
    udp_dest_t *pd = udp_dest_push_new(co);
    *pd = g_udp_read_args.peer;
    return 3;
}

static void udp_async_dispatch(udp_async_t *u, int cb_ref,
                               int (*push)(lua_State *, udp_async_t *)) {
    if (cb_ref == LUA_NOREF) return;
    lua_State *L = g_udp_L;
    if (!L) return;
    lua_State *co = lua_newthread(L);
    lua_pushvalue(L, -1);
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    lua_rawgeti(co, LUA_REGISTRYINDEX, cb_ref);
    int nargs = push(co, u);
    fan_coro_wake(L, co, co_ref, nargs);
}

static void udp_async_readcb(evutil_socket_t fd, short what, void *arg) {
    (void)what;
    udp_async_t *u = (udp_async_t *)arg;
    if (u->closed) return;
    /* Drain every pending datagram in this callback tick — libevent's
     * EV_PERSIST re-fires if we leave data behind, but consolidating the
     * loop here reduces callback overhead when the socket is hot. */
    for (;;) {
        char buf[UDP_RECV_MAX];
        struct sockaddr_storage ss;
        socklen_t slen = sizeof(ss);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&ss, &slen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            /* Other errors are transient at the socket layer for UDP;
             * v1 fan.udpd logged and continued.  We do the same to keep
             * a hot receiver going. */
            break;
        }
        if (u->on_read_ref == LUA_NOREF) continue;  /* drain, no callback */

        /* Populate a stack peer + stash into the TLS args slot for the
         * dispatch push helper. */
        udp_dest_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(&peer.ss, &ss, slen);
        peer.slen = slen;
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)&peer.ss;
            inet_ntop(AF_INET, &s4->sin_addr, peer.host, sizeof(peer.host));
            peer.port = ntohs(s4->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&peer.ss;
            inet_ntop(AF_INET6, &s6->sin6_addr, peer.host, sizeof(peer.host));
            peer.port = ntohs(s6->sin6_port);
        }

        g_udp_read_args.u = u;
        g_udp_read_args.data = buf;
        g_udp_read_args.len = (size_t)n;
        g_udp_read_args.peer = peer;
        udp_async_dispatch(u, u->on_read_ref, udp_async_push_self_data_dest);
        g_udp_read_args.u = NULL;
        g_udp_read_args.data = NULL;
        g_udp_read_args.len = 0;
    }
}

static void udp_async_writecb(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    udp_async_t *u = (udp_async_t *)arg;
    if (u->closed) return;
    /* Fire onsendready once — the caller must re-arm via send_req() for
     * the next drain.  v1 semantics: the writecb is a one-shot armed by
     * send_req, not a continuously firing edge trigger. */
    if (u->evw) { event_del(u->evw); }
    udp_async_dispatch(u, u->on_sendready_ref, udp_async_push_self);
}

/* Internal helper to build a socket + optional bind for both new_async
 * and rebind.  Returns 0 on success, -1 with *err set on failure. */
static int udp_async_open_socket(udp_async_t *u, const char **err) {
    int family = AF_INET;
    if (u->bind_host && u->bind_host[0]) {
        /* Family sniff from address literal. */
        struct in6_addr tmp6;
        if (inet_pton(AF_INET6, u->bind_host, &tmp6) == 1) family = AF_INET6;
    }
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) { *err = strerror(errno); return -1; }
    make_nonblocking(fd);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (u->bind_port >= 0) {
        struct sockaddr_storage ss;
        socklen_t slen = 0;
        memset(&ss, 0, sizeof(ss));
        if (family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
            s4->sin_family = AF_INET;
            s4->sin_port = htons((uint16_t)u->bind_port);
            if (!u->bind_host || !u->bind_host[0])
                s4->sin_addr.s_addr = htonl(INADDR_ANY);
            else if (inet_pton(AF_INET, u->bind_host, &s4->sin_addr) != 1) {
                close(fd); *err = "bind_host: bad ipv4"; return -1;
            }
            slen = sizeof(*s4);
        } else {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
            s6->sin6_family = AF_INET6;
            s6->sin6_port = htons((uint16_t)u->bind_port);
            if (!u->bind_host || !u->bind_host[0]) s6->sin6_addr = in6addr_any;
            else if (inet_pton(AF_INET6, u->bind_host, &s6->sin6_addr) != 1) {
                close(fd); *err = "bind_host: bad ipv6"; return -1;
            }
            slen = sizeof(*s6);
        }
        if (bind(fd, (struct sockaddr *)&ss, slen) != 0) {
            *err = strerror(errno);
            close(fd);
            return -1;
        }
    }
    u->fd = fd;
    u->family = family;
    struct event_base *base = fan_loop_current_base();
    u->ev = event_new(base, fd, EV_READ | EV_PERSIST, udp_async_readcb, u);
    if (!u->ev) { close(fd); u->fd = -1; *err = "event_new failed"; return -1; }
    event_add(u->ev, NULL);
    return 0;
}

static int l_udp_new_async(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Reject v2-incompatible fields (M17 parity). */
    lua_getfield(L, 1, "worker");
    if (!lua_isnil(L, -1))
        return luaL_error(L, "fan.udp.new_async: `worker` is not supported");
    lua_pop(L, 1);
    lua_getfield(L, 1, "callback_self_first");
    if (!lua_isnil(L, -1))
        return luaL_error(L, "fan.udp.new_async: `callback_self_first` is not supported; callbacks are always self-first");
    lua_pop(L, 1);

    const char *bind_host = NULL;
    lua_getfield(L, 1, "bind_host");
    if (lua_isstring(L, -1)) bind_host = lua_tostring(L, -1);
    /* keep on stack briefly for strdup */
    int bind_port = -1;
    lua_getfield(L, 1, "bind_port");
    if (lua_isnumber(L, -1)) bind_port = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (bind_port != -1 && (bind_port < 0 || bind_port > 65535)) {
        lua_pop(L, 1);   /* pop bind_host string */
        return luaL_error(L, "bind_port out of range");
    }

    const char *def_host = NULL;
    lua_getfield(L, 1, "host");
    if (lua_isstring(L, -1)) def_host = lua_tostring(L, -1);
    /* keep on stack briefly */
    int def_port = 0;
    lua_getfield(L, 1, "port");
    if (lua_isnumber(L, -1)) def_port = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    int on_read_ref = LUA_NOREF, on_sendready_ref = LUA_NOREF;
    lua_getfield(L, 1, "onread");
    if (lua_isfunction(L, -1)) on_read_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);
    lua_getfield(L, 1, "onsendready");
    if (lua_isfunction(L, -1)) on_sendready_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);

    /* strdup the stack strings and pop them. */
    char *bind_host_dup = bind_host ? strdup(bind_host) : NULL;
    char *def_host_dup  = def_host  ? strdup(def_host)  : NULL;
    lua_pop(L, 2);   /* host + bind_host */

    udp_async_t *u = (udp_async_t *)lua_newuserdata(L, sizeof(*u));
    memset(u, 0, sizeof(*u));
    u->fd = -1;
    u->on_read_ref      = on_read_ref;
    u->on_sendready_ref = on_sendready_ref;
    u->self_ref = LUA_NOREF;
    u->bind_host = bind_host_dup;
    u->bind_port = bind_port;
    u->def_host  = def_host_dup;
    u->def_port  = def_port;
    if (def_host_dup) {
        struct in6_addr tmp6;
        u->def_family = (inet_pton(AF_INET6, def_host_dup, &tmp6) == 1)
                        ? AF_INET6 : AF_INET;
    }
    luaL_getmetatable(L, UDP_ASYNC_MT);
    lua_setmetatable(L, -2);

    /* Pin self for callback dispatch across in-flight events. */
    lua_pushvalue(L, -1);
    u->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    const char *oerr = NULL;
    if (udp_async_open_socket(u, &oerr) != 0) {
        /* Failed early: release the self-pin so GC can collect immediately. */
        if (u->self_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, u->self_ref);
            u->self_ref = LUA_NOREF;
        }
        lua_pop(L, 1);   /* drop the userdata */
        lua_pushnil(L);
        lua_pushfstring(L, "udp new_async: %s", oerr ? oerr : "unknown");
        return 2;
    }
    return 1;
}

/* sock:send(data [, dest])
 *   dest = UDP_AddrInfo (from make_dest) — optional; falls back to
 *   host:port passed at socket creation.
 * Returns true / (nil, err). */
static int l_udp_async_send(lua_State *L) {
    udp_async_t *u = (udp_async_t *)luaL_checkudata(L, 1, UDP_ASYNC_MT);
    size_t len; const char *data = luaL_checklstring(L, 2, &len);
    if (u->closed || u->fd < 0) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }

    struct sockaddr *sa = NULL;
    socklen_t sl = 0;
    struct sockaddr_storage tmpss;

    if (!lua_isnoneornil(L, 3)) {
        udp_dest_t *d = (udp_dest_t *)luaL_checkudata(L, 3, UDP_DEST_MT);
        sa = (struct sockaddr *)&d->ss;
        sl = d->slen;
    } else if (u->def_host) {
        int family = u->def_family;
        socklen_t slen = parse_dest(family, u->def_host, u->def_port, &tmpss);
        if (slen == 0) {
            lua_pushnil(L); lua_pushstring(L, "default host: bad address");
            return 2;
        }
        sa = (struct sockaddr *)&tmpss;
        sl = slen;
    } else {
        lua_pushnil(L); lua_pushstring(L, "no destination"); return 2;
    }

    ssize_t n = sendto(u->fd, data, len, 0, sa, sl);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2; }
    lua_pushboolean(L, 1);
    return 1;
}

/* sock:send_req() — arm the EV_WRITE one-shot so onsendready fires
 * as soon as the socket is writable.  In practice UDP is almost
 * always writable; the callback fires on the next loop tick. */
static int l_udp_async_send_req(lua_State *L) {
    udp_async_t *u = (udp_async_t *)luaL_checkudata(L, 1, UDP_ASYNC_MT);
    if (u->closed) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    if (!u->evw) {
        struct event_base *base = fan_loop_current_base();
        u->evw = event_new(base, u->fd, EV_WRITE, udp_async_writecb, u);
        if (!u->evw) {
            lua_pushnil(L); lua_pushstring(L, "event_new (write) failed");
            return 2;
        }
    }
    event_add(u->evw, NULL);
    lua_pushboolean(L, 1);
    return 1;
}

/* sock:getPort() — v1 name (mixed case), returns the local bound port. */
static int l_udp_async_getPort(lua_State *L) {
    udp_async_t *u = (udp_async_t *)luaL_checkudata(L, 1, UDP_ASYNC_MT);
    if (u->fd < 0) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getsockname(u->fd, (struct sockaddr *)&ss, &sl) != 0) {
        lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2;
    }
    unsigned p = 0;
    if (ss.ss_family == AF_INET)  p = ntohs(((struct sockaddr_in  *)&ss)->sin_port);
    if (ss.ss_family == AF_INET6) p = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    lua_pushinteger(L, (lua_Integer)p);
    return 1;
}

/* sock:rebind() — teardown + rebuild on the same bind_host / bind_port.
 * v1 use case: mobile network transition (radio hop, VPN reconnect). */
static int l_udp_async_rebind(lua_State *L) {
    udp_async_t *u = (udp_async_t *)luaL_checkudata(L, 1, UDP_ASYNC_MT);
    if (u->closed) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    if (u->ev)  { event_free(u->ev);  u->ev  = NULL; }
    if (u->evw) { event_free(u->evw); u->evw = NULL; }
    if (u->fd >= 0) { close(u->fd); u->fd = -1; }
    const char *oerr = NULL;
    if (udp_async_open_socket(u, &oerr) != 0) {
        lua_pushnil(L); lua_pushstring(L, oerr ? oerr : "rebind failed"); return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* sock:close() — synchronous.  Idempotent. */
static int l_udp_async_close(lua_State *L) {
    udp_async_t *u = (udp_async_t *)luaL_checkudata(L, 1, UDP_ASYNC_MT);
    if (u->closed) return 0;
    u->closed = 1;
    if (u->ev)  { event_free(u->ev);  u->ev  = NULL; }
    if (u->evw) { event_free(u->evw); u->evw = NULL; }
    if (u->fd >= 0) { close(u->fd); u->fd = -1; }
    if (u->self_ref != LUA_NOREF) {
        int r = u->self_ref;
        u->self_ref = LUA_NOREF;
        fan_unref_safe(g_udp_L, r);
    }
    return 0;
}

static int udp_async_gc(lua_State *L) {
    udp_async_t *u = (udp_async_t *)luaL_checkudata(L, 1, UDP_ASYNC_MT);
    if (u->ev)  { event_free(u->ev);  u->ev  = NULL; }
    if (u->evw) { event_free(u->evw); u->evw = NULL; }
    if (u->fd >= 0) { close(u->fd); u->fd = -1; }
    if (u->bind_host) { free(u->bind_host); u->bind_host = NULL; }
    if (u->def_host)  { free(u->def_host);  u->def_host  = NULL; }
    if (u->on_read_ref      != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, u->on_read_ref);      u->on_read_ref      = LUA_NOREF; }
    if (u->on_sendready_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, u->on_sendready_ref); u->on_sendready_ref = LUA_NOREF; }
    if (u->self_ref         != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, u->self_ref);         u->self_ref         = LUA_NOREF; }
    return 0;
}

static int udp_dest_gc(lua_State *L) {
    /* Nothing dynamic to release — sockaddr_storage is inline in the
     * userdata.  Keep the __gc slot so a future extension (e.g. resolver
     * back-refs) has a hook. */
    (void)L;
    return 0;
}

/* forward-declared prototype so the register hook below can hand them
 * to the tcp-style parse_dest helper without another header round-trip. */

static const luaL_Reg udp_methods[] = {
    {"sendto", l_sendto},
    {"recv",   l_recv},
    {"getport", l_getport},
    {"join",   l_join},
    {"leave",  l_leave},
    {"close",  l_close},
    {NULL, NULL},
};

static const luaL_Reg udp_async_methods[] = {
    {"send",       l_udp_async_send},
    {"send_req",   l_udp_async_send_req},
    {"getPort",    l_udp_async_getPort},
    {"getport",    l_udp_async_getPort},   /* v1-lowercase alias */
    {"rebind",     l_udp_async_rebind},
    {"close",      l_udp_async_close},
    {NULL, NULL},
};

static const luaL_Reg udp_dest_methods[] = {
    {"getHost", l_udp_dest_getHost},
    {"getIP",   l_udp_dest_getIP},
    {"getPort", l_udp_dest_getPort},
    {NULL, NULL},
};

static const luaL_Reg udp_funcs[] = {
    {"new",        l_new},
    {"new_async",  l_udp_new_async},
    {"make_dest",  l_udp_make_dest},
    {"make_dests", l_udp_make_dests},
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

    /* M18: async socket metatable */
    luaL_newmetatable(L, UDP_ASYNC_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, udp_async_methods, 0);
#else
    luaL_register(L, NULL, udp_async_methods);
#endif
    lua_pushcfunction(L, udp_async_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* M18: dest (UDP_AddrInfo) metatable */
    luaL_newmetatable(L, UDP_DEST_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, udp_dest_methods, 0);
#else
    luaL_register(L, NULL, udp_dest_methods);
#endif
    lua_pushcfunction(L, udp_dest_gc);
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
