/*
 * tcp.c — LuaFan v2 asynchronous TCP over libevent bufferevent.
 * See tcp.h for the Lua API surface.
 */
#include "tcp.h"
#include "../platform.h"
#include "../runtime/loop.h"
#include "../runtime/coro.h"
#include "tls.h"

#include <lauxlib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/dns.h>
#include <event2/listener.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define TCP_CONN_MT "fan.tcp.conn"
#define TCP_SERVER_MT "fan.tcp.server"

/* ---- connection object ---------------------------------------------------- */
typedef struct {
    struct bufferevent *bev;
    lua_State *co;      /* coroutine currently parked on this conn, or NULL */
    int        co_ref;  /* registry pin for the parked coroutine */
    int        self_ref;/* registry pin for this conn userdata (connect path) */
    int        want;    /* receive intent: 0=none, -1=any bytes, n=n bytes */
    int        connecting;
    int        connected;
    int        eof;
    int        closed;
    int        draining;      /* close requested but output not yet flushed */
    int        drain_self_ref;/* registry pin keeping conn alive while draining */
    char      *err;     /* last error string (owned), or NULL */
} tcp_conn_t;

/* ---- server object -------------------------------------------------------- */
typedef struct {
    struct evconnlistener *listener; /* not used in M1 impl (we use bufferevent accept via evutil) */
    int on_accept_ref;               /* Lua function ref */
    int closed;
    void *tls_ctx;                   /* OpenSSL SSL_CTX* when ssl=true, else NULL */
} tcp_server_t;

/* forward */
static void conn_readcb(struct bufferevent *bev, void *arg);
static void conn_eventcb(struct bufferevent *bev, short what, void *arg);

static void conn_set_err(tcp_conn_t *c, const char *msg) {
    free(c->err);
    c->err = msg ? strdup(msg) : NULL;
}

/* Main thread of the owning lua_State, stashed at module load, used to unref /
 * resume from libevent callbacks. MUST be the state's main thread (see
 * fan_coro_main): the thread that runs `require "fan"` is usually a throwaway
 * coroutine that can be collected while our callbacks are still armed. */
static lua_State *g_main_L = NULL;

/* push a new conn userdata wrapping bev; leaves it on top of L */
static tcp_conn_t *conn_push_new(lua_State *L, struct bufferevent *bev) {
    tcp_conn_t *c = (tcp_conn_t *)lua_newuserdata(L, sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->bev = bev;
    c->co_ref = LUA_NOREF;
    c->self_ref = LUA_NOREF;
    c->drain_self_ref = LUA_NOREF;
    luaL_getmetatable(L, TCP_CONN_MT);
    lua_setmetatable(L, -2);
    return c;
}

/* Resume the coroutine parked on this conn, passing nargs values already on
 * its stack. Clears the park slot first (so the resumed body can re-park). */
static void conn_wake(tcp_conn_t *c, int nargs) {
    lua_State *co = c->co;
    int ref = c->co_ref;
    c->co = NULL;
    c->co_ref = LUA_NOREF;
    c->want = 0;
    fan_coro_wake(g_main_L, co, ref, nargs);
}

/* ---- receive completion check (called from read/event callbacks) ---------- */
static int conn_try_complete_receive(tcp_conn_t *c) {
    if (!c->co || c->want == 0) return 0;
    struct evbuffer *in = bufferevent_get_input(c->bev);
    size_t avail = evbuffer_get_length(in);

    int satisfied = 0;
    if (c->want == -1) {
        satisfied = (avail > 0) || c->eof || c->err;
    } else {
        satisfied = ((int)avail >= c->want) || c->eof || c->err;
    }
    if (!satisfied) return 0;

    lua_State *co = c->co;
    if (c->err) {
        lua_pushnil(co);
        lua_pushstring(co, c->err);
        conn_wake(c, 2);
        return 1;
    }
    size_t take = (c->want == -1) ? avail : (size_t)c->want;
    if (take > avail) take = avail;
    if (take == 0) {
        /* eof with no data */
        lua_pushnil(co);
        lua_pushstring(co, "eof");
        conn_wake(c, 2);
        return 1;
    }
    char *tmp = (char *)malloc(take);
    if (!tmp) { lua_pushnil(co); lua_pushstring(co, "oom"); conn_wake(c, 2); return 1; }
    evbuffer_remove(in, tmp, take);
    lua_pushlstring(co, tmp, take);
    free(tmp);
    conn_wake(c, 1);
    return 1;
}

static void conn_readcb(struct bufferevent *bev, void *arg) {
    (void)bev;
    tcp_conn_t *c = (tcp_conn_t *)arg;
    conn_try_complete_receive(c);
}

static void conn_eventcb(struct bufferevent *bev, short what, void *arg) {
    (void)bev;
    tcp_conn_t *c = (tcp_conn_t *)arg;
    if (what & BEV_EVENT_CONNECTED) {
        /* Only meaningful when a coroutine parked in fan.tcp.connect is
         * waiting for the handshake to finish (c->connecting == 1). For
         * server-side TLS accept the CONNECTED event fires when the TLS
         * handshake completes; no coroutine is parked on it (the accept
         * path already resumed the handler with the conn object), so
         * ignore it here. Waking the handler's parked receive() with a
         * spurious value would corrupt its state. */
        if (!c->connecting) return;
        c->connecting = 0;
        c->connected = 1;
        if (c->co) {
            /* return the conn userdata as connect()'s result */
            lua_State *co = c->co;
            if (c->self_ref != LUA_NOREF) {
                lua_rawgeti(co, LUA_REGISTRYINDEX, c->self_ref);
            } else {
                lua_pushnil(co);
            }
            int self_ref = c->self_ref;
            c->self_ref = LUA_NOREF;
            conn_wake(c, 1);
            if (self_ref != LUA_NOREF) fan_unref_safe(g_main_L, self_ref);
        }
        return;
    }
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        if (what & BEV_EVENT_EOF) c->eof = 1;
        if (what & BEV_EVENT_ERROR) conn_set_err(c, "connection error");
        if (what & BEV_EVENT_TIMEOUT) conn_set_err(c, "timeout");
        c->connecting = 0;
        if (c->co) {
            if (c->connected == 0) {
                /* failed during connect */
                int self_ref = c->self_ref;
                c->self_ref = LUA_NOREF;
                lua_pushnil(c->co);
                lua_pushstring(c->co, c->err ? c->err : "connect failed");
                conn_wake(c, 2);
                if (self_ref != LUA_NOREF) fan_unref_safe(g_main_L, self_ref);
            } else {
                conn_try_complete_receive(c);
            }
        }
    }
}

/* ---- fan.tcp.connect(host, port[, opts]) ----------------------------------
 * opts (optional table): { ssl=bool, verify_peer=bool, verify_host=bool }.
 * When ssl=true the connection is wrapped in TLS via net/tls (OpenSSL). The
 * rest of the connection lifecycle (send/receive/drain/close) is identical to
 * a plain TCP conn because both share the same bufferevent machinery. */
static int l_connect(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    if (port < 1 || port > 65535) return luaL_error(L, "port out of range");

    int use_ssl = 0, verify_peer = 1, verify_host = 1;
    if (lua_type(L, 3) == LUA_TTABLE) {
        lua_getfield(L, 3, "ssl");
        use_ssl = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "verify_peer");
        if (!lua_isnil(L, -1)) verify_peer = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "verify_host");
        if (!lua_isnil(L, -1)) verify_host = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    int ref = fan_coro_park(L);
    if (ref == LUA_NOREF) {
        return luaL_error(L, "fan.tcp.connect must be called from a coroutine");
    }

    struct event_base *base = fan_loop_current_base();
    struct evdns_base *dns = fan_loop_dnsbase();
    struct bufferevent *bev;
    if (use_ssl) {
        const char *terr = NULL;
        bev = fan_tls_client_bev(base, host, verify_peer, verify_host, &terr);
        if (!bev) {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return luaL_error(L, "tls connect failed: %s", terr ? terr : "unknown");
        }
    } else {
        bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
        if (!bev) {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return luaL_error(L, "bufferevent_socket_new failed");
        }
    }

    tcp_conn_t *c = conn_push_new(L, bev);  /* conn userdata now on L stack */
    c->co = L;
    c->co_ref = ref;
    c->connecting = 1;
    /* pin the conn userdata so the CONNECTED callback (running later on the
     * event loop, when the coroutine's stack is not directly reachable) can
     * push it back as connect()'s return value. */
    lua_pushvalue(L, -1);
    c->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    bufferevent_setcb(bev, conn_readcb, NULL, conn_eventcb, c);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    if (bufferevent_socket_connect_hostname(bev, dns, AF_UNSPEC, host, port) < 0) {
        c->co = NULL; c->co_ref = LUA_NOREF;
        if (c->self_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, c->self_ref); c->self_ref = LUA_NOREF; }
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "connect dispatch failed");
    }
    /* park until CONNECTED (returns conn) or failure (returns nil, err). */
    return lua_yield(L, 0);
}

/* ---- conn:send(data) ------------------------------------------------------ */
static int l_send(lua_State *L) {
    tcp_conn_t *c = (tcp_conn_t *)luaL_checkudata(L, 1, TCP_CONN_MT);
    size_t len; const char *data = luaL_checklstring(L, 2, &len);
    if (c->closed || !c->bev) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    if (bufferevent_write(c->bev, data, len) != 0) {
        lua_pushnil(L); lua_pushstring(L, "write failed"); return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- conn:receive([n]) ---------------------------------------------------- */
static int l_receive(lua_State *L) {
    tcp_conn_t *c = (tcp_conn_t *)luaL_checkudata(L, 1, TCP_CONN_MT);
    int want = -1; /* any */
    if (!lua_isnoneornil(L, 2)) {
        want = (int)luaL_checkinteger(L, 2);
        if (want <= 0) return luaL_error(L, "receive size must be > 0");
    }
    if (c->closed || !c->bev) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }

    /* fast path: data already available and satisfies the request */
    struct evbuffer *in = bufferevent_get_input(c->bev);
    size_t avail = evbuffer_get_length(in);
    if (c->err) { lua_pushnil(L); lua_pushstring(L, c->err); return 2; }
    if ((want == -1 && avail > 0) || (want > 0 && (int)avail >= want)) {
        size_t take = (want == -1) ? avail : (size_t)want;
        char *tmp = (char *)malloc(take);
        if (!tmp) return luaL_error(L, "oom");
        evbuffer_remove(in, tmp, take);
        lua_pushlstring(L, tmp, take);
        free(tmp);
        return 1;
    }
    if (c->eof) { lua_pushnil(L); lua_pushstring(L, "eof"); return 2; }

    /* park until the read/event callback satisfies us */
    int ref = fan_coro_park(L);
    if (ref == LUA_NOREF) return luaL_error(L, "receive must be called from a coroutine");
    c->co = L; c->co_ref = ref; c->want = want;
    return lua_yield(L, 0);
}

/* ---- conn:close() --------------------------------------------------------- */
/* Release the bev the conn is holding. Idempotent. */
static void conn_release_bev(tcp_conn_t *c) {
    if (!c->bev) return;
    bufferevent_free(c->bev);
    c->bev = NULL;
}

/* write callback used only while draining a closing conn: when the output
 * buffer has fully flushed, free the bufferevent and release the drain pin. */
static void conn_drain_writecb(struct bufferevent *bev, void *arg) {
    tcp_conn_t *c = (tcp_conn_t *)arg;
    if (evbuffer_get_length(bufferevent_get_output(bev)) == 0) {
        int ref = c->drain_self_ref;
        c->drain_self_ref = LUA_NOREF;
        c->draining = 0;
        conn_release_bev(c);
        if (ref != LUA_NOREF) fan_unref_safe(g_main_L, ref);
    }
}

static int l_close(lua_State *L) {
    tcp_conn_t *c = (tcp_conn_t *)luaL_checkudata(L, 1, TCP_CONN_MT);
    if (c->closed || c->draining) return 0;
    if (c->bev) {
        /* if there is unsent output, drain it before freeing so the peer is not
         * cut off mid-message (avoids losing data on a send-then-close). */
        if (evbuffer_get_length(bufferevent_get_output(c->bev)) > 0) {
            c->draining = 1;
            c->closed = 1;
            /* pin the conn userdata so Lua GC cannot free it while draining */
            lua_pushvalue(L, 1);
            c->drain_self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
            bufferevent_setcb(c->bev, NULL, conn_drain_writecb, conn_eventcb, c);
            bufferevent_disable(c->bev, EV_READ);
            bufferevent_enable(c->bev, EV_WRITE);
            return 0;
        }
        conn_release_bev(c);
    }
    c->closed = 1;
    return 0;
}

static int conn_gc(lua_State *L) {
    tcp_conn_t *c = (tcp_conn_t *)luaL_checkudata(L, 1, TCP_CONN_MT);
    conn_release_bev(c);
    free(c->err); c->err = NULL;
    return 0;
}

/* ---- v1 parity (M14.C-i): shutdown / pause_read / resume_read /
 * getsockname / getpeername ------------------------------------------------
 *
 * `shutdown(how)` — half-close. Default `how` matches v1: SHUT_WR (write
 *   side) so the peer sees EOF on their reads while we can still read
 *   pending frames from them. Accepts optional integer overrides:
 *     0 = SHUT_RD, 1 = SHUT_WR, 2 = SHUT_RDWR
 *   v1 returned the *pending output bytes* count so Lua could decide
 *   whether to defer the actual shutdown; we do the same. If there are
 *   pending bytes we DO NOT shutdown yet (the drain path in l_close /
 *   the caller is responsible for retrying); we only shutdown when the
 *   output buffer is empty. This matches v1 tcpd.c:298-308 exactly.
 *
 * `pause_read` / `resume_read` — thin bufferevent_disable/enable wrappers
 *   for backpressure. No-op on a closed conn. Do not touch the parked
 *   coroutine or `want` state: a receive already yielded on the same conn
 *   stays yielded until the caller resumes reads and data arrives.
 *
 * `getsockname` / `getpeername` — return (host, port) pair, or (nil, nil)
 *   on failure. IPv4/IPv6 both handled via sockaddr_storage + inet_ntop.
 *   v1 also returned (nil, nil) on any error (no separate errmsg); we
 *   match that so callers don't have to check `type(ret)`.
 */

/* Common helper for getsockname / getpeername; op = 0 (sock) / 1 (peer). */
static int conn_get_addr(lua_State *L, int op) {
    tcp_conn_t *c = (tcp_conn_t *)luaL_checkudata(L, 1, TCP_CONN_MT);
    if (!c->bev || c->closed) {
        lua_pushnil(L); lua_pushnil(L); return 2;
    }
    evutil_socket_t fd = bufferevent_getfd(c->bev);
    if (fd < 0) {
        lua_pushnil(L); lua_pushnil(L); return 2;
    }
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    int rc = (op == 0)
        ? getsockname(fd, (struct sockaddr *)&ss, &slen)
        : getpeername(fd, (struct sockaddr *)&ss, &slen);
    if (rc != 0) {
        lua_pushnil(L); lua_pushnil(L); return 2;
    }
    char ip[INET6_ADDRSTRLEN] = {0};
    int  port = 0;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        port = ntohs(a->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
        port = ntohs(a->sin6_port);
    } else {
        /* AF_UNIX and friends: no host/port pair — return (nil, nil). */
        lua_pushnil(L); lua_pushnil(L); return 2;
    }
    lua_pushstring(L, ip);
    lua_pushinteger(L, port);
    return 2;
}

static int l_shutdown(lua_State *L) {
    tcp_conn_t *c = (tcp_conn_t *)luaL_checkudata(L, 1, TCP_CONN_MT);
    /* v1 default is SHUT_WR (1). Accept 0/1/2 explicitly; anything else is
     * an error to catch typos early. */
    int how = (int)luaL_optinteger(L, 2, SHUT_WR);
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) {
        return luaL_error(L, "shutdown how must be 0 (SHUT_RD), 1 (SHUT_WR), or 2 (SHUT_RDWR)");
    }
    if (!c->bev || c->closed) { lua_pushinteger(L, 0); return 1; }
    struct evbuffer *out = bufferevent_get_output(c->bev);
    size_t pending = evbuffer_get_length(out);
    if (pending == 0) {
        evutil_socket_t fd = bufferevent_getfd(c->bev);
        if (fd >= 0) shutdown(fd, how);
    }
    /* v1 returns pending byte count so the caller can decide to poll and
     * retry the shutdown once the buffer has drained. */
    lua_pushinteger(L, (lua_Integer)pending);
    return 1;
}

static int l_pause_read(lua_State *L) {
    tcp_conn_t *c = (tcp_conn_t *)luaL_checkudata(L, 1, TCP_CONN_MT);
    if (c->bev && !c->closed) bufferevent_disable(c->bev, EV_READ);
    return 0;
}

static int l_resume_read(lua_State *L) {
    tcp_conn_t *c = (tcp_conn_t *)luaL_checkudata(L, 1, TCP_CONN_MT);
    if (c->bev && !c->closed) bufferevent_enable(c->bev, EV_READ);
    return 0;
}

static int l_getsockname(lua_State *L) { return conn_get_addr(L, 0); }
static int l_getpeername(lua_State *L) { return conn_get_addr(L, 1); }

/* ---- server: bind / accept / close ---------------------------------------- */

static void server_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                             struct sockaddr *addr, int socklen, void *arg) {
    (void)listener; (void)addr; (void)socklen;
    tcp_server_t *sv = (tcp_server_t *)arg;
    if (sv->closed || sv->on_accept_ref == LUA_NOREF) {
        evutil_closesocket(fd);
        return;
    }
    struct event_base *base = fan_loop_current_base();
    struct bufferevent *bev;
    if (sv->tls_ctx) {
        const char *terr = NULL;
        bev = fan_tls_server_bev(base, fd, sv->tls_ctx, &terr);
        if (!bev) {
            /* TLS wrap failed: close the accepted fd and abort this conn */
            evutil_closesocket(fd);
            return;
        }
    } else {
        bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
        if (!bev) { evutil_closesocket(fd); return; }
    }

    /* create the connection coroutine: on_accept(conn) */
    lua_State *L = g_main_L;
    if (!L) {
        bufferevent_free(bev);
        return;
    }
    lua_State *co = lua_newthread(L);
    /* pin the coroutine across its run */
    lua_pushvalue(L, -1);
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);  /* pop the thread pushed by newthread */

    /* push on_accept fn + conn onto the coroutine stack */
    lua_rawgeti(co, LUA_REGISTRYINDEX, sv->on_accept_ref);   /* fn */
    tcp_conn_t *c = conn_push_new(co, bev);                  /* conn on co */
    c->connected = 1;
    bufferevent_setcb(bev, conn_readcb, NULL, conn_eventcb, c);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    fan_coro_wake(L, co, co_ref, 1);  /* resume with 1 arg (conn); unref after */
}

/* fan.tcp.bind(host, port, on_accept[, opts])
 *   opts (optional table): { ssl=bool, cert=path, key=path }
 * When ssl=true the listener wraps each accepted socket in a server-side TLS
 * bufferevent (BUFFEREVENT_SSL_ACCEPTING) using the cert/key pair. When ssl
 * is false or opts is absent the listener behaves as plain TCP (backwards
 * compatible with M2/M3 callers). */
static int l_bind(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (port < 0 || port > 65535) return luaL_error(L, "port out of range");

    int use_ssl = 0;
    const char *cert = NULL, *key = NULL;
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "ssl");
        use_ssl = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 4, "cert");
        if (lua_isstring(L, -1)) cert = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 4, "key");
        if (lua_isstring(L, -1)) key = lua_tostring(L, -1);
        lua_pop(L, 1);
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (host && strcmp(host, "0.0.0.0") != 0 && host[0]) {
        if (inet_pton(AF_INET, host, &sin.sin_addr) != 1) {
            /* only numeric IPv4 in M2 bind (hostname bind is future work) */
            if (strcmp(host, "localhost") == 0) sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            else return luaL_error(L, "bind host must be a numeric IPv4 or localhost");
        }
    } else {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    /* build the tls server context up-front (before we allocate the server
     * userdata) so failures return nil,err without leaking a listener. */
    void *tls_ctx = NULL;
    if (use_ssl) {
        if (!cert || !key) {
            lua_pushnil(L);
            lua_pushstring(L, "bind{ssl=true} requires cert=... and key=...");
            return 2;
        }
        const char *terr = NULL;
        tls_ctx = fan_tls_server_ctx_new(cert, key, &terr);
        if (!tls_ctx) {
            lua_pushnil(L);
            lua_pushfstring(L, "tls server ctx: %s", terr ? terr : "unknown");
            return 2;
        }
    }

    /* stash the on_accept function */
    lua_pushvalue(L, 3);
    int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    tcp_server_t *sv = (tcp_server_t *)lua_newuserdata(L, sizeof(*sv));
    memset(sv, 0, sizeof(*sv));
    sv->on_accept_ref = fn_ref;
    sv->tls_ctx = tls_ctx;
    luaL_getmetatable(L, TCP_SERVER_MT);
    lua_setmetatable(L, -2);

    struct event_base *base = fan_loop_current_base();
    sv->listener = evconnlistener_new_bind(base, server_accept_cb, sv,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
        (struct sockaddr *)&sin, sizeof(sin));
    if (!sv->listener) {
        luaL_unref(L, LUA_REGISTRYINDEX, fn_ref);
        sv->on_accept_ref = LUA_NOREF;
        if (sv->tls_ctx) { fan_tls_server_ctx_free(sv->tls_ctx); sv->tls_ctx = NULL; }
        lua_pushnil(L);
        lua_pushfstring(L, "bind failed on %s:%d: errno=%d (%s)",
                        host, port, errno, strerror(errno));
        return 2;
    }
    /* server userdata is the single return value */
    return 1;
}

static int l_server_close(lua_State *L) {
    tcp_server_t *sv = (tcp_server_t *)luaL_checkudata(L, 1, TCP_SERVER_MT);
    if (!sv->closed) {
        if (sv->listener) { evconnlistener_free(sv->listener); sv->listener = NULL; }
        if (sv->on_accept_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, sv->on_accept_ref); sv->on_accept_ref = LUA_NOREF; }
        if (sv->tls_ctx) { fan_tls_server_ctx_free(sv->tls_ctx); sv->tls_ctx = NULL; }
        sv->closed = 1;
    }
    return 0;
}

/* fan.tcp.bind returns a server userdata. server:getport() reports the
 * actual bound TCP port via getsockname() on the listener fd. Useful when
 * the caller passed port=0 to let the kernel pick one (M6 workers). */
static int l_server_getport(lua_State *L) {
    tcp_server_t *sv = (tcp_server_t *)luaL_checkudata(L, 1, TCP_SERVER_MT);
    if (!sv->listener) { lua_pushnil(L); lua_pushliteral(L, "listener closed"); return 2; }
    evutil_socket_t fd = evconnlistener_get_fd(sv->listener);
    if (fd < 0) { lua_pushnil(L); lua_pushliteral(L, "no fd"); return 2; }
    struct sockaddr_storage ss;
    socklen_t sl = (socklen_t)sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) {
        lua_pushnil(L); lua_pushstring(L, strerror(errno));
        return 2;
    }
    unsigned p = 0;
    if (ss.ss_family == AF_INET)  p = ntohs(((struct sockaddr_in  *)&ss)->sin_port);
    if (ss.ss_family == AF_INET6) p = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    lua_pushinteger(L, (lua_Integer)p);
    return 1;
}

static int server_gc(lua_State *L) {
    tcp_server_t *sv = (tcp_server_t *)luaL_checkudata(L, 1, TCP_SERVER_MT);
    if (sv->listener) { evconnlistener_free(sv->listener); sv->listener = NULL; }
    if (sv->on_accept_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, sv->on_accept_ref); sv->on_accept_ref = LUA_NOREF; }
    if (sv->tls_ctx) { fan_tls_server_ctx_free(sv->tls_ctx); sv->tls_ctx = NULL; }
    return 0;
}

/*
 * fan_tcp_clear_lua_state — teardown hook invoked immediately before the
 * owning `lua_close(L)`. NULLs out the cached main-thread pointer so any
 * libevent callback that fires between lua_close and fan_loop_cleanup
 * sees a well-defined sentinel instead of dereferencing dangling memory.
 *
 * Paired with the `if (!L)` early-return in server_accept_cb (above at
 * line ~473): with g_main_L == NULL, an accepted socket is closed
 * without touching Lua and the connection is quietly dropped. This is
 * the luafan2 counterpart of v1's problem-12 fix (see the v1 tests
 * test_tcpd_cleanup_mainthread.lua and test_http_client_timer_linger.lua
 * for the exact regression class).
 *
 * KNOWN GAP (do not silently "fix" without a plan): the guard covers
 * server_accept_cb only. conn_wake -> fan_coro_wake and
 * conn_drain_writecb still touch g_main_L directly (see
 * fan_coro_wake in runtime/coro.c: luaL_unref(main_L, ...)). Currently
 * safe because the three real callers (main.c teardown paths) invoke
 * this hook after fan_loop_run has returned and before lua_close, so
 * no wake / drain callback can fire. If a future caller inverts that
 * order, or a __gc-driven bufferevent_free ends up synchronously
 * firing a pending write callback, that path will UAF. The
 * incremental fix is to push a `if (!main_L) return;` into
 * fan_coro_wake (covers all 8 modules that cache g_*_L in one edit).
 *
 * Contract tested in tests/c/unit/test_tcp_clear_lua_state.c:
 * idempotent, safe before any register(), and reversible via a
 * subsequent fan_tcp_register(L_new).
 */
void fan_tcp_clear_lua_state(void) {
    g_main_L = NULL;
}

/* ---- registration --------------------------------------------------------- */
static const luaL_Reg conn_methods[] = {
    {"send",         l_send},
    {"receive",      l_receive},
    {"close",        l_close},
    /* v1 parity — M14.C-i */
    {"shutdown",     l_shutdown},
    {"pause_read",   l_pause_read},
    {"resume_read",  l_resume_read},
    {"getsockname",  l_getsockname},
    {"getpeername",  l_getpeername},
    {NULL, NULL},
};

static const luaL_Reg server_methods[] = {
    {"close",   l_server_close},
    {"getport", l_server_getport},
    {NULL, NULL},
};

static const luaL_Reg tcp_funcs[] = {
    {"connect", l_connect},
    {"bind",    l_bind},
    {NULL, NULL},
};

void fan_tcp_register(lua_State *L) {
    g_main_L = fan_coro_main(L);  /* stable main thread, not the require() coroutine */

    /* conn metatable */
    luaL_newmetatable(L, TCP_CONN_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, conn_methods, 0);
#else
    luaL_register(L, NULL, conn_methods);
#endif
    lua_pushcfunction(L, conn_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* server metatable */
    luaL_newmetatable(L, TCP_SERVER_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, server_methods, 0);
#else
    luaL_register(L, NULL, server_methods);
#endif
    lua_pushcfunction(L, server_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* fan.tcp table */
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, tcp_funcs, 0);
#else
    luaL_register(L, NULL, tcp_funcs);
#endif
    lua_setfield(L, -2, "tcp");  /* fan.tcp = {...} ; module table at -2 */
}
