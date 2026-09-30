/*
 * tcp.c — LuaFan v2 asynchronous TCP over libevent bufferevent.
 * See tcp.h for the Lua API surface.
 */
#include "tcp.h"
#include "../platform.h"
#include "../runtime/loop.h"
#include "../runtime/coro.h"
#include "tls.h"
#include "evdns.h"

#include <lauxlib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/dns.h>
#include <event2/listener.h>
#include <event2/util.h>              /* EVUTIL_SOCKET_ERROR, evutil_socket_error_to_string */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* M21.2 — pull in OpenSSL + libevent-openssl symbols so conn_eventcb can
 * surface the *specific* handshake / socket reason instead of the generic
 * "connection error".  Guarded by FAN_WITH_OPENSSL: builds without OpenSSL
 * fall back to socket-errno only. */
#if FAN_WITH_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <event2/bufferevent_ssl.h>
#endif

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

/* M21.2 — Build a specific error string for a bufferevent that just fired
 * BEV_EVENT_ERROR.  Priority order (most-specific first):
 *
 *   1. DNS error via bufferevent_socket_get_dns_error(bev)  (non-zero when
 *      the hostname lookup failed — evutil_gai_strerror gives "nodename nor
 *      servname provided" / "Name or service not known" / etc.)
 *   2. OpenSSL error queue via bufferevent_get_openssl_error(bev)  (non-zero
 *      when the TLS bev is in the CONNECTING state and the handshake failed
 *      — includes "certificate verify failed" / "wrong version number" / etc)
 *   3. For TLS bevs whose OpenSSL queue is empty but the peer-verify result
 *      is not X509_V_OK: X509_verify_cert_error_string  ("unable to get local
 *      issuer certificate" etc).  This catches the "SSL_VERIFY_NONE would
 *      have succeeded" case where the queue is drained but the result is
 *      still bad.
 *   4. Fall back to EVUTIL_SOCKET_ERROR() → evutil_socket_error_to_string
 *      ("Connection refused", "Network is unreachable", ...).
 *   5. Absolute fallback: "connection error" (only when nothing was
 *      actionable — shouldn't happen in practice).
 *
 * The buffer is caller-owned; the returned string is `buf` on success or
 * a static literal.  Never returns NULL. */
static const char *conn_describe_bev_error(struct bufferevent *bev,
                                           char *buf, size_t buflen) {
    if (!bev || !buf || buflen == 0) return "connection error";
    /* 1. DNS */
    int dns_err = bufferevent_socket_get_dns_error(bev);
    if (dns_err) {
        snprintf(buf, buflen, "dns error: %s", evutil_gai_strerror(dns_err));
        return buf;
    }
#if FAN_WITH_OPENSSL
    /* 2. OpenSSL queue.  bufferevent_get_openssl_error only makes sense
     *    when the bev is an openssl bev; on a plain socket bev it returns 0
     *    (per libevent docs), so this is safe to always call. */
    unsigned long ssl_err = bufferevent_get_openssl_error(bev);
    if (ssl_err) {
        char ssl_buf[192];
        ERR_error_string_n(ssl_err, ssl_buf, sizeof ssl_buf);
        /* ERR_clear_error() so the next request doesn't inherit stale
         * queue entries — the queue is per-thread and we've now consumed
         * the meaningful bits. */
        ERR_clear_error();
        snprintf(buf, buflen, "tls error: %s", ssl_buf);
        return buf;
    }
    /* 3. Peer-verify reason for TLS bevs (queue was empty but verify
     *    failed — happens with SSL_VERIFY_NONE + a bad cert, or when
     *    OpenSSL cleared the queue between error and callback). */
    SSL *ssl = bufferevent_openssl_get_ssl(bev);
    if (ssl) {
        char why[192];
        if (fan_tls_client_verify_reason(ssl, why, sizeof why)) {
            snprintf(buf, buflen, "tls verify failed: %s", why);
            return buf;
        }
    }
#endif
    /* 4. Socket errno. */
    int se = EVUTIL_SOCKET_ERROR();
    if (se) {
        snprintf(buf, buflen, "socket error: %s",
                 evutil_socket_error_to_string(se));
        return buf;
    }
    /* 5. Fallback. */
    snprintf(buf, buflen, "connection error");
    return buf;
}

static void conn_eventcb(struct bufferevent *bev, short what, void *arg) {
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
        if (what & BEV_EVENT_ERROR) {
            /* M21.2 — replace "connection error" with a specific reason
             * (DNS / TLS / socket errno).  Local buffer is copied into
             * c->err by conn_set_err, so its lifetime ends here. */
            char errbuf[256];
            const char *msg = conn_describe_bev_error(bev, errbuf, sizeof errbuf);
            conn_set_err(c, msg);
        }
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
 * opts (optional table):
 *   { ssl=bool,                       -- wrap in TLS via net/tls (OpenSSL)
 *     verify_peer=bool,               -- SSL_VERIFY_PEER on/off (default on)
 *     verify_host=bool,               -- X509_CHECK_FLAG hostname bind (default on)
 *     ssl_host=string,                -- override SNI + verify hostname (M21.2)
 *     cainfo=path, capath=path        -- custom CA bundle / dir for verify (M21.2)
 *   }.
 * When ssl=true the connection is wrapped in TLS via net/tls (OpenSSL). The
 * rest of the connection lifecycle (send/receive/drain/close) is identical to
 * a plain TCP conn because both share the same bufferevent machinery.
 *
 * M21.2 — Extended the opts surface (ssl_host / cainfo / capath) so
 * pure-Lua HTTPS through fan.http_lua can pin its own CA bundle rather
 * than being locked to the process-wide default trust store.  The
 * extended path routes through fan_tls_client_bev_ex; the legacy no-CA
 * path still uses fan_tls_client_bev for byte-compatibility with
 * unchanged callers. */
static int l_connect(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    if (port < 1 || port > 65535) return luaL_error(L, "port out of range");

    int use_ssl = 0, verify_peer = 1, verify_host = 1;
    const char *ssl_host = NULL, *cainfo = NULL, *capath = NULL;
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
        /* M21.2 — cainfo / capath / ssl_host.  The string pointers live on
         * the Lua stack for the duration of this C call, which is enough:
         * fan_tls_client_bev_ex builds the SSL_CTX (and caches it via
         * fingerprint) before returning, so the strings need not survive
         * past bev construction. */
        lua_getfield(L, 3, "ssl_host");
        if (lua_isstring(L, -1)) ssl_host = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "cainfo");
        if (lua_isstring(L, -1)) cainfo = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "capath");
        if (lua_isstring(L, -1)) capath = lua_tostring(L, -1);
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
        /* M21.2 — route through _ex when the caller supplied any extended
         * TLS param.  The legacy fan_tls_client_bev path is kept for the
         * common case (no per-request options) so unchanged callers hit
         * exactly the same code as before. */
        if (ssl_host || cainfo || capath) {
            bev = fan_tls_client_bev_ex(base, host, ssl_host,
                                        verify_peer, verify_host,
                                        cainfo, capath, NULL, NULL, &terr);
        } else {
            bev = fan_tls_client_bev(base, host, verify_peer, verify_host, &terr);
        }
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

/* ==========================================================================
 * M17: callback-based async TCP client (fan.tcp.connect_async{...})
 *
 * Restores v1 fan.tcpd semantics on top of libevent bufferevent:
 *   - immediate handle return (unlike the coroutine-yielding fan.tcp.connect)
 *   - onconnected(self) / onread(self, data) / ondisconnected(self, reason)
 *   - pre-connect send queue: conn:send() before onconnected buffers into the
 *     bufferevent output queue; libevent flushes on connect
 *   - close mid-connect race: ondisconnected(self, "closed") fires exactly once
 *     via the `dispatched_disc` flag; onconnected never fires post-close
 *   - reconnect(): tear the bev down and rebuild it to the same host:port
 *
 * Not in M17-1 (arrive in M17-2/-3):
 *   - connect_timeout / read_timeout / write_timeout
 *   - onsendready
 *   - ssl_host / cainfo / capath / ssl_verifyhost / ssl_verifypeer / pkcs12
 *   - evdns
 *   - server-side bind_async
 *
 * Rejected in M17-1 already (v2 has no notion of these):
 *   - worker (multi-loop-in-process not available in v2)
 *   - callback_self_first (callbacks always take self as the first argument)
 * ========================================================================== */
#define TCP_ASYNC_CONN_MT "fan.tcp.async_conn"

typedef struct {
    struct bufferevent *bev;
    /* Reconnect uses these to rebuild the bev. */
    char *host;
    int   port;
    int   use_ssl;
    int   verify_peer;
    int   verify_host;

    /* M17-2: TLS parameter surface — all strdup'd copies, freed in __gc.  NULL
     * means "not set", passed through to fan_tls_client_bev_ex which then
     * either falls back to defaults (cainfo/capath) or skips the option
     * entirely (ssl_host / pkcs12_path). */
    char *ssl_host;
    char *cainfo;
    char *capath;
    char *pkcs12_path;
    char *pkcs12_password;

    /* M17-2: timeouts in milliseconds; 0 means "no timeout" (v1 parity).
     * read/write are enforced by libevent via bufferevent_set_timeouts.
     * connect is enforced by an independent one-shot timer armed at connect
     * time and disarmed on CONNECTED / disc; without it a stuck SYN would
     * only fire read/write timeouts, which do not cover the SYN state. */
    int connect_timeout_ms;
    int read_timeout_ms;
    int write_timeout_ms;
    struct event *connect_timer;   /* NULL when disarmed */

    /* M17-2: onsendready dispatched from bufferevent's writecb when the
     * output buffer drains to empty.  writecb is only armed while the caller
     * has registered onsendready (otherwise we skip the callback slot in
     * bufferevent_setcb to save the dispatch cost). */
    int on_sendready_ref;

    /* M17-2: caller-supplied evdns_base.  If NULL we fall back to
     * fan_loop_dnsbase().  We pin the fan.evdns userdata (evdns_ref) to keep
     * the base alive for the connection lifetime; unref in __gc / disc. */
    struct evdns_base *dnsbase;
    int                evdns_ref;

    /* Lua registry refs — LUA_NOREF when absent. */
    int on_connected_ref;
    int on_read_ref;
    int on_disc_ref;
    int self_ref;          /* pin the userdata across in-flight callbacks */

    /* State machine flags — mutually consistent, not enum. */
    int connecting;        /* connect_hostname issued, waiting for CONNECTED */
    int connected;         /* handshake done, callbacks can fire */
    int closed;            /* user called close() */
    int dispatched_disc;   /* ondisconnected has fired (exactly-once guard) */
} tcp_async_conn_t;

/* forward */
static void async_conn_readcb(struct bufferevent *bev, void *arg);
static void async_conn_eventcb(struct bufferevent *bev, short what, void *arg);
static int  async_conn_rebuild_bev(tcp_async_conn_t *c, const char **err);

/* Dispatch a callback: create a coroutine, push fn + args, resume + unref.
 * `ref` is the registry pin for the fn to invoke; leaves the fn registered.
 * `push_args(co, ud)` pushes the callback arguments onto `co`; returns nargs. */
static void async_dispatch(tcp_async_conn_t *c, int cb_ref,
                           int (*push_args)(lua_State *, tcp_async_conn_t *)) {
    if (cb_ref == LUA_NOREF) return;
    lua_State *L = g_main_L;
    if (!L) return;   /* teardown window; drop the callback silently */
    lua_State *co = lua_newthread(L);
    lua_pushvalue(L, -1);
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    lua_rawgeti(co, LUA_REGISTRYINDEX, cb_ref);   /* fn */
    int nargs = push_args(co, c);
    fan_coro_wake(L, co, co_ref, nargs);
}

/* Push `self` (the async conn userdata) as arg 1. */
static int async_push_self(lua_State *co, tcp_async_conn_t *c) {
    if (c->self_ref != LUA_NOREF) {
        lua_rawgeti(co, LUA_REGISTRYINDEX, c->self_ref);
    } else {
        lua_pushnil(co);
    }
    return 1;
}

/* Push `self` + a stashed string (allocated in the caller). */
typedef struct { tcp_async_conn_t *c; const char *reason; } async_disc_args_t;
static __thread async_disc_args_t g_disc_args;   /* single-threaded dispatch */
static int async_push_self_and_reason(lua_State *co, tcp_async_conn_t *c) {
    (void)c;
    if (g_disc_args.c && g_disc_args.c->self_ref != LUA_NOREF) {
        lua_rawgeti(co, LUA_REGISTRYINDEX, g_disc_args.c->self_ref);
    } else {
        lua_pushnil(co);
    }
    lua_pushstring(co, g_disc_args.reason ? g_disc_args.reason : "");
    return 2;
}

/* Stash for the onread dispatch: we consume all currently-available bytes
 * from the input evbuffer, push {self, data}, and hand off. */
typedef struct { tcp_async_conn_t *c; char *data; size_t len; } async_read_args_t;
static __thread async_read_args_t g_read_args;
static int async_push_self_and_data(lua_State *co, tcp_async_conn_t *c) {
    (void)c;
    if (g_read_args.c && g_read_args.c->self_ref != LUA_NOREF) {
        lua_rawgeti(co, LUA_REGISTRYINDEX, g_read_args.c->self_ref);
    } else {
        lua_pushnil(co);
    }
    lua_pushlstring(co, g_read_args.data, g_read_args.len);
    return 2;
}

/* Fire ondisconnected exactly once. Idempotent: subsequent calls are no-ops.
 * Releases the self-pin AFTER the dispatch so the callback sees a valid self.
 * `reason` is copied into the thread-local stash for the duration of the
 * dispatch call (which is synchronous from our POV — fan_coro_wake resumes
 * the fresh coroutine inline). */
static void async_fire_disc(tcp_async_conn_t *c, const char *reason) {
    if (c->dispatched_disc) return;
    c->dispatched_disc = 1;
    int cb = c->on_disc_ref;
    g_disc_args.c = c;
    g_disc_args.reason = reason;
    async_dispatch(c, cb, async_push_self_and_reason);
    g_disc_args.c = NULL;
    g_disc_args.reason = NULL;
    /* Release the self-pin so GC can collect the conn once user drops it. */
    if (c->self_ref != LUA_NOREF) {
        int r = c->self_ref;
        c->self_ref = LUA_NOREF;
        fan_unref_safe(g_main_L, r);
    }
}

static void async_conn_readcb(struct bufferevent *bev, void *arg) {
    tcp_async_conn_t *c = (tcp_async_conn_t *)arg;
    if (c->closed || c->dispatched_disc) return;
    struct evbuffer *in = bufferevent_get_input(bev);
    size_t avail = evbuffer_get_length(in);
    if (avail == 0) return;
    if (c->on_read_ref == LUA_NOREF) {
        /* drain silently so we do not spin on the readable event */
        evbuffer_drain(in, avail);
        return;
    }
    char *tmp = (char *)malloc(avail);
    if (!tmp) return;    /* OOM: leave data in buffer; next event may retry */
    evbuffer_remove(in, tmp, avail);
    g_read_args.c = c;
    g_read_args.data = tmp;
    g_read_args.len = avail;
    async_dispatch(c, c->on_read_ref, async_push_self_and_data);
    g_read_args.c = NULL;
    g_read_args.data = NULL;
    g_read_args.len = 0;
    free(tmp);
}

/* M17-2: bufferevent write callback.  Fires each time the output evbuffer
 * transitions to empty; that is our "send-ready" signal.  Only armed when
 * the caller registered onsendready (otherwise we would pay dispatch cost
 * for every full-drain even though no one listens). */
static void async_conn_writecb(struct bufferevent *bev, void *arg) {
    tcp_async_conn_t *c = (tcp_async_conn_t *)arg;
    if (c->closed || c->dispatched_disc) return;
    if (c->on_sendready_ref == LUA_NOREF) return;
    /* Only fire when the buffer really is empty — libevent's watermark
     * default (write low = 0) means writecb triggers on drain-to-empty, so
     * this is just belt-and-braces. */
    if (evbuffer_get_length(bufferevent_get_output(bev)) != 0) return;
    async_dispatch(c, c->on_sendready_ref, async_push_self);
}

/* M17-2: disarm and free the connect-timeout timer if it exists.
 * Idempotent; safe to call at any point in the state machine. */
static void async_conn_disarm_connect_timer(tcp_async_conn_t *c) {
    if (!c->connect_timer) return;
    event_del(c->connect_timer);
    event_free(c->connect_timer);
    c->connect_timer = NULL;
}

/* M17-2: one-shot connect_timeout callback.  Fires only if the socket has
 * not yet transitioned to CONNECTED by the deadline; releases the bev and
 * dispatches ondisconnected(reason="timeout"). */
static void async_conn_connect_timer_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    tcp_async_conn_t *c = (tcp_async_conn_t *)arg;
    /* If we already connected, closed, or fired disc, the timer is stale. */
    if (c->closed || c->connected || c->dispatched_disc) return;
    /* Tear down the pending bev — otherwise a later CONNECTED / ERROR event
     * would fire against a conn that has already dispatched disc. */
    if (c->bev) { bufferevent_free(c->bev); c->bev = NULL; }
    c->connecting = 0;
    async_fire_disc(c, "connect_timeout");
}

static void async_conn_eventcb(struct bufferevent *bev, short what, void *arg) {
    (void)bev;
    tcp_async_conn_t *c = (tcp_async_conn_t *)arg;
    if (what & BEV_EVENT_CONNECTED) {
        if (c->closed || c->dispatched_disc) return;
        c->connecting = 0;
        c->connected = 1;
        /* Handshake done: the connect_timeout guard is no longer needed. */
        async_conn_disarm_connect_timer(c);
        async_dispatch(c, c->on_connected_ref, async_push_self);
        return;
    }
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        /* A libevent BEV_EVENT_TIMEOUT here is a *read/write* timeout,
         * fired via bufferevent_set_timeouts.  connect_timeout is delivered
         * separately through async_conn_connect_timer_cb (see above). */
        const char *reason =
            (what & BEV_EVENT_EOF)     ? "eof" :
            (what & BEV_EVENT_TIMEOUT) ? "timeout" :
                                         "error";
        c->connecting = 0;
        c->connected = 0;
        async_conn_disarm_connect_timer(c);
        async_fire_disc(c, reason);
    }
}

/* Build a fresh bufferevent for the async conn using the stored host/port and
 * SSL parameters. Used by both the initial connect and reconnect(). On failure
 * returns -1 and *err is set to a static message. */
static int async_conn_rebuild_bev(tcp_async_conn_t *c, const char **err) {
    struct event_base *base = fan_loop_current_base();
    /* M17-2: prefer a caller-supplied evdns_base (via `evdns = ...` in the
     * option table) so downstream code can point at a custom resolver; fall
     * back to the loop's default. */
    struct evdns_base *dns = c->dnsbase ? c->dnsbase : fan_loop_dnsbase();
    struct bufferevent *bev;
    if (c->use_ssl) {
        /* M17-2: fan_tls_client_bev_ex handles the extended TLS parameter
         * surface (ssl_host / cainfo / capath / pkcs12).  NULL fields
         * degrade to the same behaviour as the legacy fan_tls_client_bev. */
        const char *terr = NULL;
        bev = fan_tls_client_bev_ex(base, c->host, c->ssl_host,
                                    c->verify_peer, c->verify_host,
                                    c->cainfo, c->capath,
                                    c->pkcs12_path, c->pkcs12_password,
                                    &terr);
        if (!bev) { if (err) *err = terr ? terr : "tls init failed"; return -1; }
    } else {
        bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
        if (!bev) { if (err) *err = "bufferevent_socket_new failed"; return -1; }
    }
    c->bev = bev;

    /* M17-2: writecb only when onsendready is set.  Passing NULL as the
     * writecb slot to bufferevent_setcb avoids the (small) per-drain
     * dispatch overhead in the far more common no-onsendready case. */
    bufferevent_data_cb wcb =
        (c->on_sendready_ref != LUA_NOREF) ? async_conn_writecb : NULL;
    bufferevent_setcb(bev, async_conn_readcb, wcb, async_conn_eventcb, c);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    /* M17-2: read / write timeouts.  bufferevent_set_timeouts takes two
     * struct timeval* — NULL for either slot means "disabled".  Zero
     * timeouts (ms == 0) map to NULL to preserve v1's "0 = disabled". */
    if (c->read_timeout_ms > 0 || c->write_timeout_ms > 0) {
        struct timeval rtv = {
            c->read_timeout_ms / 1000,
            (c->read_timeout_ms % 1000) * 1000,
        };
        struct timeval wtv = {
            c->write_timeout_ms / 1000,
            (c->write_timeout_ms % 1000) * 1000,
        };
        bufferevent_set_timeouts(bev,
            c->read_timeout_ms  > 0 ? &rtv : NULL,
            c->write_timeout_ms > 0 ? &wtv : NULL);
    }

    if (bufferevent_socket_connect_hostname(bev, dns, AF_UNSPEC, c->host, c->port) < 0) {
        bufferevent_free(bev);
        c->bev = NULL;
        if (err) *err = "connect dispatch failed";
        return -1;
    }
    c->connecting = 1;

    /* M17-2: arm the connect_timeout timer.  bufferevent read/write timeouts
     * only apply post-handshake; a stuck SYN or slow TLS negotiation needs
     * this independent guard.  Zero means disabled. */
    if (c->connect_timeout_ms > 0) {
        struct timeval ctv = {
            c->connect_timeout_ms / 1000,
            (c->connect_timeout_ms % 1000) * 1000,
        };
        c->connect_timer = event_new(base, -1, 0,
                                     async_conn_connect_timer_cb, c);
        if (c->connect_timer) event_add(c->connect_timer, &ctv);
        /* Timer alloc failure is non-fatal: worst case the caller does not
         * see a connect_timeout event, but read/write timeouts still cover
         * post-handshake progress. */
    }
    return 0;
}

/* ---- fan.tcp.connect_async{...} ------------------------------------------ */
static int l_connect_async(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Reject fields we intentionally do not support in v2. */
    lua_getfield(L, 1, "worker");
    if (!lua_isnil(L, -1)) {
        return luaL_error(L,
            "fan.tcp.connect_async: the `worker` field is not supported "
            "(v2 has no in-process worker event bases)");
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "callback_self_first");
    if (!lua_isnil(L, -1)) {
        return luaL_error(L,
            "fan.tcp.connect_async: the `callback_self_first` field is not "
            "supported; callbacks always receive `self` as the first argument");
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "host");
    const char *host = luaL_checkstring(L, -1);
    lua_getfield(L, 1, "port");
    int port = (int)luaL_checkinteger(L, -1);
    if (port < 1 || port > 65535) return luaL_error(L, "port out of range");
    lua_pop(L, 2);

    int use_ssl = 0, verify_peer = 1, verify_host = 1;
    lua_getfield(L, 1, "ssl");
    use_ssl = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "ssl_verifypeer");
    if (!lua_isnil(L, -1)) verify_peer = lua_tointeger(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 1, "ssl_verifyhost");
    if (!lua_isnil(L, -1)) verify_host = lua_tointeger(L, -1) != 0;
    lua_pop(L, 1);

    /* M17-2: extended TLS parameter surface.  strdup each string as we
     * see it so the ownership boundary is clean: everything below owns
     * heap-allocated copies, and the option-table Lua strings can be popped
     * immediately.  NULL means "not set", which lines up with the conn
     * userdata's memset(0) so no explicit re-nulling is needed. */
    char *ssl_host_dup = NULL, *cainfo_dup = NULL, *capath_dup = NULL;
    lua_getfield(L, 1, "ssl_host");
    if (lua_isstring(L, -1)) ssl_host_dup = strdup(lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "cainfo");
    if (lua_isstring(L, -1)) cainfo_dup   = strdup(lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "capath");
    if (lua_isstring(L, -1)) capath_dup   = strdup(lua_tostring(L, -1));
    lua_pop(L, 1);
    /* pkcs12 = { path = ..., password = ... } is a nested table.  Extract
     * both children then pop the outer table. */
    char *pkcs12_path_dup = NULL, *pkcs12_password_dup = NULL;
    lua_getfield(L, 1, "pkcs12");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "path");
        if (lua_isstring(L, -1)) pkcs12_path_dup = strdup(lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, -1, "password");
        if (lua_isstring(L, -1)) pkcs12_password_dup = strdup(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    /* M17-2: timeouts.  v1 fan.tcpd took seconds as a Lua number (may be
     * fractional); convert to milliseconds and store as int for the timer
     * arithmetic.  0 (unset field) means "no timeout" — v1 parity. */
    int connect_timeout_ms = 0, read_timeout_ms = 0, write_timeout_ms = 0;
    lua_getfield(L, 1, "connect_timeout");
    if (lua_isnumber(L, -1)) connect_timeout_ms = (int)(lua_tonumber(L, -1) * 1000);
    lua_pop(L, 1);
    lua_getfield(L, 1, "read_timeout");
    if (lua_isnumber(L, -1)) read_timeout_ms = (int)(lua_tonumber(L, -1) * 1000);
    lua_pop(L, 1);
    lua_getfield(L, 1, "write_timeout");
    if (lua_isnumber(L, -1)) write_timeout_ms = (int)(lua_tonumber(L, -1) * 1000);
    lua_pop(L, 1);

    /* M17-2: caller-supplied evdns_base.  Pin the userdata so its __gc
     * cannot free the base while our connection still holds it. */
    int evdns_ref = LUA_NOREF;
    struct evdns_base *dnsbase = NULL;
    lua_getfield(L, 1, "evdns");
    if (!lua_isnil(L, -1)) {
        dnsbase = fan_evdns_get_base(L, -1);
        if (dnsbase) {
            lua_pushvalue(L, -1);
            evdns_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
    }
    lua_pop(L, 1);

    /* Pin the four callbacks before we build state we would need to unwind
     * on failure. */
    int on_connected = LUA_NOREF, on_read = LUA_NOREF, on_disc = LUA_NOREF;
    int on_sendready = LUA_NOREF;
    lua_getfield(L, 1, "onconnected");
    if (lua_isfunction(L, -1)) on_connected = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);
    lua_getfield(L, 1, "onread");
    if (lua_isfunction(L, -1)) on_read = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);
    lua_getfield(L, 1, "onsendready");
    if (lua_isfunction(L, -1)) on_sendready = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);
    lua_getfield(L, 1, "ondisconnected");
    if (lua_isfunction(L, -1)) on_disc = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);

    /* Allocate the userdata. */
    tcp_async_conn_t *c = (tcp_async_conn_t *)lua_newuserdata(L, sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->host = strdup(host);
    c->port = port;
    c->use_ssl = use_ssl;
    c->verify_peer = verify_peer;
    c->verify_host = verify_host;
    c->ssl_host        = ssl_host_dup;
    c->cainfo          = cainfo_dup;
    c->capath          = capath_dup;
    c->pkcs12_path     = pkcs12_path_dup;
    c->pkcs12_password = pkcs12_password_dup;
    c->connect_timeout_ms = connect_timeout_ms;
    c->read_timeout_ms    = read_timeout_ms;
    c->write_timeout_ms   = write_timeout_ms;
    c->dnsbase   = dnsbase;
    c->evdns_ref = evdns_ref;
    c->on_connected_ref = on_connected;
    c->on_read_ref      = on_read;
    c->on_sendready_ref = on_sendready;
    c->on_disc_ref      = on_disc;
    c->self_ref = LUA_NOREF;
    luaL_getmetatable(L, TCP_ASYNC_CONN_MT);
    lua_setmetatable(L, -2);

    /* Pin the userdata itself so the eventcb can dispatch callbacks with a
     * live self even after the caller drops the local. Released either by
     * async_fire_disc or by close() (which fires disc synthetically). */
    lua_pushvalue(L, -1);
    c->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* Kick off the connect. On dispatch failure, schedule a disc(self,reason)
     * via the loop rather than reporting synchronously — v1 fan.tcpd returns
     * the handle first and reports build failures through ondisconnected. */
    const char *berr = NULL;
    if (async_conn_rebuild_bev(c, &berr) != 0) {
        /* We cannot fire the callback synchronously (caller has not received
         * the handle yet, so `self` is not what they hold). Defer to next
         * loop tick via event_base_once. Simpler alternative: return the
         * handle now and rely on the caller to hit send() before we fire —
         * but the v1 contract is "handle first, disc later". Use a zero-
         * timeout timer. */
        struct event_base *base = fan_loop_current_base();
        struct timeval zero = {0, 0};
        /* We stash reason on the struct itself because event_base_once has
         * no user arg pass-through for a strdup we do not want to leak. */
        c->connecting = 0;
        /* Fire disc from a stub. We reuse a one-shot timer. */
        struct { tcp_async_conn_t *c; const char *reason; } *args =
            (void *)malloc(sizeof(*args));
        if (args) {
            args->c = c;
            args->reason = strdup(berr ? berr : "build failed");
            /* Note: v2 tcp.c does not currently need this pattern elsewhere;
             * we invoke event_base_once from libevent to defer. */
            extern void fan_tcp_async_deferred_disc(evutil_socket_t, short, void *);
            event_base_once(base, -1, EV_TIMEOUT, fan_tcp_async_deferred_disc,
                            args, &zero);
        }
    }
    /* Return the handle. */
    return 1;
}

/* One-shot timer callback for deferred build-failure ondisconnected. */
void fan_tcp_async_deferred_disc(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    struct { tcp_async_conn_t *c; const char *reason; } *args = arg;
    tcp_async_conn_t *c = args->c;
    const char *reason = args->reason;
    if (!c->closed) async_fire_disc(c, reason);
    free((void *)reason);
    free(args);
}

/* ---- async conn:send(data) — buffers even before onconnected ------------ */
static int l_async_send(lua_State *L) {
    tcp_async_conn_t *c = (tcp_async_conn_t *)luaL_checkudata(L, 1, TCP_ASYNC_CONN_MT);
    size_t len; const char *data = luaL_checklstring(L, 2, &len);
    if (c->closed || !c->bev) {
        lua_pushnil(L); lua_pushstring(L, "closed"); return 2;
    }
    /* bufferevent_write accepts writes before CONNECTED and flushes them
     * automatically once the socket is writable — this is the C-side
     * pre-connect send queue. */
    if (bufferevent_write(c->bev, data, len) != 0) {
        lua_pushnil(L); lua_pushstring(L, "write failed"); return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- async conn:close() ------------------------------------------------- */
static int l_async_close(lua_State *L) {
    tcp_async_conn_t *c = (tcp_async_conn_t *)luaL_checkudata(L, 1, TCP_ASYNC_CONN_MT);
    if (c->closed) return 0;
    c->closed = 1;
    /* M17-2: disarm the connect timer BEFORE freeing the bev so its callback
     * cannot fire against a torn-down conn. */
    async_conn_disarm_connect_timer(c);
    if (c->bev) { bufferevent_free(c->bev); c->bev = NULL; }
    /* v1 contract: close() also fires ondisconnected(self, "closed") — makes
     * downstream state-machine cleanup uniform (one path for both peer-driven
     * and local-driven disconnect). Skip if we already fired for another
     * reason (race between remote EOF and local close). */
    if (!c->dispatched_disc) async_fire_disc(c, "closed");
    return 0;
}

/* ---- async conn:reconnect() --------------------------------------------- */
static int l_async_reconnect(lua_State *L) {
    tcp_async_conn_t *c = (tcp_async_conn_t *)luaL_checkudata(L, 1, TCP_ASYNC_CONN_MT);
    if (c->closed) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    /* M17-2: tear down any stale timer along with the bev so rebuild starts
     * from a clean slate.  The timer would otherwise be reused with a fresh
     * event_new inside rebuild_bev and we would leak the old one. */
    async_conn_disarm_connect_timer(c);
    if (c->bev) { bufferevent_free(c->bev); c->bev = NULL; }
    c->connecting = 0;
    c->connected = 0;
    c->dispatched_disc = 0;
    /* Re-pin self if it was released by a prior ondisconnected fire. */
    if (c->self_ref == LUA_NOREF) {
        lua_pushvalue(L, 1);
        c->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    const char *berr = NULL;
    if (async_conn_rebuild_bev(c, &berr) != 0) {
        lua_pushnil(L); lua_pushstring(L, berr ? berr : "reconnect failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- async conn: getpeername / getsockname / pause_read / resume_read /
 *      shutdown — thin wrappers that mostly mirror the sync path. */
static int l_async_getaddr(lua_State *L, int peer) {
    tcp_async_conn_t *c = (tcp_async_conn_t *)luaL_checkudata(L, 1, TCP_ASYNC_CONN_MT);
    if (!c->bev) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    evutil_socket_t fd = bufferevent_getfd(c->bev);
    if (fd < 0) { lua_pushnil(L); lua_pushstring(L, "no fd"); return 2; }
    struct sockaddr_storage ss;
    socklen_t sl = (socklen_t)sizeof(ss);
    int rc = peer ? getpeername(fd, (struct sockaddr *)&ss, &sl)
                  : getsockname(fd, (struct sockaddr *)&ss, &sl);
    if (rc != 0) { lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2; }
    char ipbuf[64] = {0};
    unsigned port = 0;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf));
        port = ntohs(sin->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &sin6->sin6_addr, ipbuf, sizeof(ipbuf));
        port = ntohs(sin6->sin6_port);
    }
    lua_pushstring(L, ipbuf);
    lua_pushinteger(L, (lua_Integer)port);
    return 2;
}
static int l_async_getpeername(lua_State *L) { return l_async_getaddr(L, 1); }
static int l_async_getsockname(lua_State *L) { return l_async_getaddr(L, 0); }

static int l_async_pause_read(lua_State *L) {
    tcp_async_conn_t *c = (tcp_async_conn_t *)luaL_checkudata(L, 1, TCP_ASYNC_CONN_MT);
    if (c->bev && !c->closed) bufferevent_disable(c->bev, EV_READ);
    return 0;
}
static int l_async_resume_read(lua_State *L) {
    tcp_async_conn_t *c = (tcp_async_conn_t *)luaL_checkudata(L, 1, TCP_ASYNC_CONN_MT);
    if (c->bev && !c->closed) bufferevent_enable(c->bev, EV_READ);
    return 0;
}
static int l_async_shutdown(lua_State *L) {
    tcp_async_conn_t *c = (tcp_async_conn_t *)luaL_checkudata(L, 1, TCP_ASYNC_CONN_MT);
    if (c->bev) {
        evutil_socket_t fd = bufferevent_getfd(c->bev);
        if (fd >= 0) shutdown(fd, SHUT_WR);
    }
    return 0;
}

static int async_conn_gc(lua_State *L) {
    tcp_async_conn_t *c = (tcp_async_conn_t *)luaL_checkudata(L, 1, TCP_ASYNC_CONN_MT);
    /* M17-2: disarm the connect timer first so it cannot fire against a
     * conn whose registry refs are already unref'd. */
    async_conn_disarm_connect_timer(c);
    if (c->bev) { bufferevent_free(c->bev); c->bev = NULL; }
    /* Free every strdup'd string. */
    if (c->host)             { free(c->host);             c->host = NULL; }
    if (c->ssl_host)         { free(c->ssl_host);         c->ssl_host = NULL; }
    if (c->cainfo)           { free(c->cainfo);           c->cainfo = NULL; }
    if (c->capath)           { free(c->capath);           c->capath = NULL; }
    if (c->pkcs12_path)      { free(c->pkcs12_path);      c->pkcs12_path = NULL; }
    if (c->pkcs12_password)  { free(c->pkcs12_password);  c->pkcs12_password = NULL; }
    /* Release Lua registry refs (callbacks + optional evdns pin). */
    if (c->on_connected_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, c->on_connected_ref); c->on_connected_ref = LUA_NOREF; }
    if (c->on_read_ref      != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, c->on_read_ref);      c->on_read_ref      = LUA_NOREF; }
    if (c->on_sendready_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, c->on_sendready_ref); c->on_sendready_ref = LUA_NOREF; }
    if (c->on_disc_ref      != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, c->on_disc_ref);      c->on_disc_ref      = LUA_NOREF; }
    if (c->evdns_ref        != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, c->evdns_ref);        c->evdns_ref        = LUA_NOREF; }
    /* self_ref should already be gone by the time __gc runs (it was the only
     * strong root that kept us alive); if it lingers, drop it. */
    if (c->self_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, c->self_ref); c->self_ref = LUA_NOREF; }
    return 0;
}

/* ==========================================================================
 * M17-3: callback-based async TCP server (fan.tcp.bind_async{...})
 *
 * Restores v1 fan.tcpd.bind semantics on top of libevent evconnlistener +
 * bufferevent.  Two-userdata model:
 *
 *   server  — result of fan.tcp.bind_async{...}; owns the listener,
 *             the on_accept callback, TLS server context, and the
 *             rebind parameters.  Methods: close / rebind / getport.
 *
 *   accept  — a fresh userdata per accepted connection, handed to the
 *             user's on_accept callback.  The user MUST call
 *             accept:bind{ onread=..., onsendready=..., ondisconnected=... }
 *             inside on_accept to wire up the callbacks; without that
 *             call the accept is treated as unbound and the socket is
 *             torn down (matches v1 fan.tcpd behaviour).  Methods:
 *             send / close / flush / remoteinfo / pause_read /
 *             resume_read / bind.
 *
 * Reject list (same as connect_async): worker, callback_self_first.
 * ========================================================================== */
#define TCP_ASYNC_SERVER_MT "fan.tcp.async_server"
#define TCP_ASYNC_ACCEPT_MT "fan.tcp.async_accept"

typedef struct {
    struct evconnlistener *listener;

    /* Rebind parameters — strdup'd copies so we can rebuild the listener
     * without holding a Lua reference to the original option table. */
    char *host;                 /* NULL means bind to INADDR_ANY */
    int   port;
    int   use_ssl;
    char *cert_path;
    char *key_path;
    int   send_buffer_size;     /* 0 = leave kernel default */
    int   receive_buffer_size;  /* 0 = leave kernel default */

    /* Lua registry refs. */
    int on_accept_ref;
    int on_sslhostname_ref;

    void *tls_ctx;              /* SSL_CTX* when ssl=true, else NULL */
    int   closed;

    /* Pin of the server userdata itself so async_server_accept_cb can push
     * it back into Lua as the first argument of onaccept(self, accept) —
     * v1 API contract.  Unref'd in async_server_gc.  Zero when the sv is
     * mid-construction and no registry pin has been taken yet. */
    int   self_ref;
} tcp_async_server_t;

typedef struct {
    struct bufferevent *bev;

    /* Back-pointer to the owning server so we can inherit its TLS ctx
     * (for informational logging) and reach out to bind_async-supplied
     * defaults if we grow more delegated fields in future.  Optional; we
     * never dereference the server after it has been closed. */
    tcp_async_server_t *sv;

    /* Callbacks — set by accept:bind{...}.  LUA_NOREF until then. */
    int on_read_ref;
    int on_sendready_ref;
    int on_disc_ref;
    int self_ref;               /* pin the userdata while callbacks are armed */

    int bound;                  /* accept:bind{...} has been called */
    int closed;
    int dispatched_disc;
} tcp_async_accept_t;

/* forward */
static void async_accept_readcb (struct bufferevent *bev, void *arg);
static void async_accept_writecb(struct bufferevent *bev, void *arg);
static void async_accept_eventcb(struct bufferevent *bev, short what, void *arg);
static void async_server_accept_cb(struct evconnlistener *l, evutil_socket_t fd,
                                   struct sockaddr *addr, int socklen, void *arg);

/* ---- accept callback dispatch (mirrors the client-side helpers) --------- */

typedef struct { tcp_async_accept_t *a; const char *reason; } async_accept_disc_args_t;
static __thread async_accept_disc_args_t g_accept_disc_args;
static int async_accept_push_self(lua_State *co, tcp_async_accept_t *a) {
    if (a->self_ref != LUA_NOREF) lua_rawgeti(co, LUA_REGISTRYINDEX, a->self_ref);
    else lua_pushnil(co);
    return 1;
}
static int async_accept_push_self_and_reason(lua_State *co,
                                             tcp_async_accept_t *a) {
    (void)a;
    tcp_async_accept_t *ac = g_accept_disc_args.a;
    if (ac && ac->self_ref != LUA_NOREF)
        lua_rawgeti(co, LUA_REGISTRYINDEX, ac->self_ref);
    else lua_pushnil(co);
    lua_pushstring(co, g_accept_disc_args.reason ? g_accept_disc_args.reason : "");
    return 2;
}
typedef struct { tcp_async_accept_t *a; char *data; size_t len; } async_accept_read_args_t;
static __thread async_accept_read_args_t g_accept_read_args;
static int async_accept_push_self_and_data(lua_State *co,
                                           tcp_async_accept_t *a) {
    (void)a;
    tcp_async_accept_t *ac = g_accept_read_args.a;
    if (ac && ac->self_ref != LUA_NOREF)
        lua_rawgeti(co, LUA_REGISTRYINDEX, ac->self_ref);
    else lua_pushnil(co);
    lua_pushlstring(co, g_accept_read_args.data, g_accept_read_args.len);
    return 2;
}

static void async_accept_dispatch(tcp_async_accept_t *a, int cb_ref,
                                  int (*push)(lua_State *, tcp_async_accept_t *)) {
    if (cb_ref == LUA_NOREF) return;
    lua_State *L = g_main_L;
    if (!L) return;
    lua_State *co = lua_newthread(L);
    lua_pushvalue(L, -1);
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    lua_rawgeti(co, LUA_REGISTRYINDEX, cb_ref);
    int nargs = push(co, a);
    fan_coro_wake(L, co, co_ref, nargs);
}

/* Fire ondisconnected exactly once against an accepted conn.  Releases the
 * self-pin AFTER dispatch so the callback sees a live self. */
static void async_accept_fire_disc(tcp_async_accept_t *a, const char *reason) {
    if (a->dispatched_disc) return;
    a->dispatched_disc = 1;
    g_accept_disc_args.a = a;
    g_accept_disc_args.reason = reason;
    async_accept_dispatch(a, a->on_disc_ref, async_accept_push_self_and_reason);
    g_accept_disc_args.a = NULL;
    g_accept_disc_args.reason = NULL;
    if (a->self_ref != LUA_NOREF) {
        int r = a->self_ref;
        a->self_ref = LUA_NOREF;
        fan_unref_safe(g_main_L, r);
    }
}

static void async_accept_readcb(struct bufferevent *bev, void *arg) {
    tcp_async_accept_t *a = (tcp_async_accept_t *)arg;
    if (a->closed || a->dispatched_disc) return;
    struct evbuffer *in = bufferevent_get_input(bev);
    size_t avail = evbuffer_get_length(in);
    if (avail == 0) return;
    if (a->on_read_ref == LUA_NOREF) {
        /* Unbound: drop the bytes so libevent does not spin.  If we reach
         * here at all, on_accept did not call accept:bind{...}; the
         * connection will be torn down at end of on_accept anyway (see
         * async_server_accept_cb tail). */
        evbuffer_drain(in, avail);
        return;
    }
    char *tmp = (char *)malloc(avail);
    if (!tmp) return;
    evbuffer_remove(in, tmp, avail);
    g_accept_read_args.a = a;
    g_accept_read_args.data = tmp;
    g_accept_read_args.len = avail;
    async_accept_dispatch(a, a->on_read_ref, async_accept_push_self_and_data);
    g_accept_read_args.a = NULL;
    g_accept_read_args.data = NULL;
    g_accept_read_args.len = 0;
    free(tmp);
}

static void async_accept_writecb(struct bufferevent *bev, void *arg) {
    tcp_async_accept_t *a = (tcp_async_accept_t *)arg;
    if (a->closed || a->dispatched_disc) return;
    if (a->on_sendready_ref == LUA_NOREF) return;
    if (evbuffer_get_length(bufferevent_get_output(bev)) != 0) return;
    async_accept_dispatch(a, a->on_sendready_ref, async_accept_push_self);
}

static void async_accept_eventcb(struct bufferevent *bev, short what, void *arg) {
    (void)bev;
    tcp_async_accept_t *a = (tcp_async_accept_t *)arg;
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        const char *reason =
            (what & BEV_EVENT_EOF)     ? "eof" :
            (what & BEV_EVENT_TIMEOUT) ? "timeout" :
                                         "error";
        async_accept_fire_disc(a, reason);
    }
    /* Server-side bufferevents also receive CONNECTED once the TLS
     * handshake completes; we do not surface it as a callback (the client
     * on_accept was called at the point the socket was accepted, not on
     * TLS completion) but we DO need to avoid treating it as an error. */
}

/* ---- accept:bind{...} / accept:send / close / flush / remoteinfo / ...  */

static int l_async_accept_bind(lua_State *L) {
    tcp_async_accept_t *a =
        (tcp_async_accept_t *)luaL_checkudata(L, 1, TCP_ASYNC_ACCEPT_MT);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (a->closed) {
        lua_pushnil(L); lua_pushstring(L, "closed"); return 2;
    }
    if (a->bound) {
        /* Rebinding would race with in-flight callbacks against the previous
         * refs; require the caller to build a new accept if they want fresh
         * callbacks. */
        lua_pushnil(L); lua_pushstring(L, "accept already bound"); return 2;
    }

    /* Reject fields the server side also does not accept in v2. */
    lua_getfield(L, 2, "callback_self_first");
    if (!lua_isnil(L, -1)) {
        return luaL_error(L,
            "accept:bind: the `callback_self_first` field is not supported; "
            "callbacks always receive `self` as the first argument");
    }
    lua_pop(L, 1);

    int r_read = LUA_NOREF, r_sendready = LUA_NOREF, r_disc = LUA_NOREF;
    lua_getfield(L, 2, "onread");
    if (lua_isfunction(L, -1)) r_read = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);
    lua_getfield(L, 2, "onsendready");
    if (lua_isfunction(L, -1)) r_sendready = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);
    lua_getfield(L, 2, "ondisconnected");
    if (lua_isfunction(L, -1)) r_disc = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);

    a->on_read_ref      = r_read;
    a->on_sendready_ref = r_sendready;
    a->on_disc_ref      = r_disc;

    /* Wire up the bufferevent callbacks.  writecb slot is NULL unless the
     * caller registered onsendready, mirroring the client-side treatment. */
    bufferevent_data_cb wcb =
        (r_sendready != LUA_NOREF) ? async_accept_writecb : NULL;
    bufferevent_setcb(a->bev, async_accept_readcb, wcb, async_accept_eventcb, a);
    bufferevent_enable(a->bev, EV_READ | EV_WRITE);

    a->bound = 1;
    /* Pin self so the callbacks have a live handle to push as arg 1 even
     * after the user drops the local from on_accept. */
    lua_pushvalue(L, 1);
    a->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_pushboolean(L, 1);
    return 1;
}

static int l_async_accept_send(lua_State *L) {
    tcp_async_accept_t *a =
        (tcp_async_accept_t *)luaL_checkudata(L, 1, TCP_ASYNC_ACCEPT_MT);
    size_t len; const char *data = luaL_checklstring(L, 2, &len);
    if (a->closed || !a->bev) {
        lua_pushnil(L); lua_pushstring(L, "closed"); return 2;
    }
    if (bufferevent_write(a->bev, data, len) != 0) {
        lua_pushnil(L); lua_pushstring(L, "write failed"); return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_async_accept_close(lua_State *L) {
    tcp_async_accept_t *a =
        (tcp_async_accept_t *)luaL_checkudata(L, 1, TCP_ASYNC_ACCEPT_MT);
    if (a->closed) return 0;
    a->closed = 1;
    if (a->bev) { bufferevent_free(a->bev); a->bev = NULL; }
    if (!a->dispatched_disc) async_accept_fire_disc(a, "closed");
    return 0;
}

static int l_async_accept_flush(lua_State *L) {
    tcp_async_accept_t *a =
        (tcp_async_accept_t *)luaL_checkudata(L, 1, TCP_ASYNC_ACCEPT_MT);
    if (!a->closed && a->bev) {
        /* bufferevent_flush(bev, EV_WRITE, BEV_FLUSH) is a no-op on a
         * socket-backed bev by design; the write path is already
         * write-through.  We keep the method for API parity with v1
         * fan.tcpd.accept:flush() and return 1 to signal "accepted". */
        (void)bufferevent_flush(a->bev, EV_WRITE, BEV_FLUSH);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_async_accept_remoteinfo(lua_State *L) {
    tcp_async_accept_t *a =
        (tcp_async_accept_t *)luaL_checkudata(L, 1, TCP_ASYNC_ACCEPT_MT);
    if (!a->bev) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    evutil_socket_t fd = bufferevent_getfd(a->bev);
    if (fd < 0) { lua_pushnil(L); lua_pushstring(L, "no fd"); return 2; }
    struct sockaddr_storage ss;
    socklen_t sl = (socklen_t)sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) {
        lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2;
    }
    char ipbuf[64] = {0};
    unsigned port = 0;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf));
        port = ntohs(sin->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &sin6->sin6_addr, ipbuf, sizeof(ipbuf));
        port = ntohs(sin6->sin6_port);
    }
    /* v1 returned a table { ip = ..., port = ... }; match that shape. */
    lua_createtable(L, 0, 2);
    lua_pushstring(L, ipbuf);
    lua_setfield(L, -2, "ip");
    lua_pushinteger(L, (lua_Integer)port);
    lua_setfield(L, -2, "port");
    return 1;
}

static int l_async_accept_pause_read(lua_State *L) {
    tcp_async_accept_t *a =
        (tcp_async_accept_t *)luaL_checkudata(L, 1, TCP_ASYNC_ACCEPT_MT);
    if (a->bev && !a->closed) bufferevent_disable(a->bev, EV_READ);
    return 0;
}
static int l_async_accept_resume_read(lua_State *L) {
    tcp_async_accept_t *a =
        (tcp_async_accept_t *)luaL_checkudata(L, 1, TCP_ASYNC_ACCEPT_MT);
    if (a->bev && !a->closed) bufferevent_enable(a->bev, EV_READ);
    return 0;
}

static int async_accept_gc(lua_State *L) {
    tcp_async_accept_t *a =
        (tcp_async_accept_t *)luaL_checkudata(L, 1, TCP_ASYNC_ACCEPT_MT);
    if (a->bev) { bufferevent_free(a->bev); a->bev = NULL; }
    if (a->on_read_ref      != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, a->on_read_ref);      a->on_read_ref      = LUA_NOREF; }
    if (a->on_sendready_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, a->on_sendready_ref); a->on_sendready_ref = LUA_NOREF; }
    if (a->on_disc_ref      != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, a->on_disc_ref);      a->on_disc_ref      = LUA_NOREF; }
    if (a->self_ref         != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, a->self_ref);         a->self_ref         = LUA_NOREF; }
    return 0;
}

/* ---- listener accept-cb: build an accept userdata, hand it to on_accept -- */

static void async_server_accept_cb(struct evconnlistener *l, evutil_socket_t fd,
                                   struct sockaddr *addr, int socklen, void *arg) {
    (void)l; (void)addr; (void)socklen;
    tcp_async_server_t *sv = (tcp_async_server_t *)arg;
    if (sv->closed || sv->on_accept_ref == LUA_NOREF) {
        evutil_closesocket(fd);
        return;
    }

    /* Apply per-accept socket options.  SO_SNDBUF / SO_RCVBUF must be set
     * before the socket sees traffic; doing it here (before we wrap the
     * fd in a bufferevent) keeps the kernel buffers sized for the entire
     * connection lifetime. */
    if (sv->send_buffer_size > 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                   &sv->send_buffer_size, sizeof(sv->send_buffer_size));
    }
    if (sv->receive_buffer_size > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                   &sv->receive_buffer_size, sizeof(sv->receive_buffer_size));
    }

    struct event_base *base = fan_loop_current_base();
    struct bufferevent *bev;
    if (sv->tls_ctx) {
        const char *terr = NULL;
        bev = fan_tls_server_bev(base, fd, sv->tls_ctx, &terr);
        if (!bev) { evutil_closesocket(fd); return; }
    } else {
        bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
        if (!bev) { evutil_closesocket(fd); return; }
    }

    lua_State *L = g_main_L;
    if (!L) { bufferevent_free(bev); return; }

    /* Build the accept userdata on a fresh coroutine (matches the sync
     * bind's pattern — see server_accept_cb up top).  The user's
     * on_accept runs inside this coroutine; whatever accept:bind does
     * with the accept userdata (or fails to do — see the "unbound"
     * teardown below) leaves the accept in a well-defined state before
     * we return to libevent. */
    lua_State *co = lua_newthread(L);
    lua_pushvalue(L, -1);
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);

    lua_rawgeti(co, LUA_REGISTRYINDEX, sv->on_accept_ref);   /* fn */
    /* Push self (the server userdata) as arg 1 — v1 contract is
     * onaccept(self, accept).  We pinned sv itself in l_bind_async under
     * sv->self_ref so the accept callback can resolve it here. */
    if (sv->self_ref != LUA_NOREF) {
        lua_rawgeti(co, LUA_REGISTRYINDEX, sv->self_ref);
    } else {
        lua_pushnil(co);
    }

    tcp_async_accept_t *a =
        (tcp_async_accept_t *)lua_newuserdata(co, sizeof(*a));
    memset(a, 0, sizeof(*a));
    a->bev = bev;
    a->sv  = sv;
    a->on_read_ref      = LUA_NOREF;
    a->on_sendready_ref = LUA_NOREF;
    a->on_disc_ref      = LUA_NOREF;
    a->self_ref         = LUA_NOREF;
    luaL_getmetatable(co, TCP_ASYNC_ACCEPT_MT);
    lua_setmetatable(co, -2);

    /* Do NOT enable EV_READ yet: the user's on_accept must call
     * accept:bind{...} to install onread before data starts flowing.
     * l_async_accept_bind is the point at which bufferevent_enable is
     * called. */

    /* Resume on_accept(self=server-lightud, accept) synchronously via the
     * fresh coroutine; when it returns, if accept:bind was not called,
     * the accept is considered orphaned and we tear it down. */
    fan_coro_wake(L, co, co_ref, 2);

    /* Post-return: if the user didn't bind, close the fd so we don't
     * leak an unread bufferevent.  We cannot inspect `a->bound` here
     * safely because `a` may already have been GC'd if the user held no
     * reference.  Deferred cleanup instead: async_accept_gc handles it.
     * Nothing to do here beyond what GC will already do. */
}

/* SNI dispatch to Lua (onsslhostname) is a stretch goal held for a later
 * patch: OpenSSL's SSL_CTX_set_tlsext_servername_callback wiring lives in
 * tls.c so tcp.c can stay OpenSSL-agnostic.  The ref is stored on the
 * server userdata and reserved for that future extension.  Callers that
 * pass onsslhostname today see the callback ref pinned (leak-free) but
 * never actually dispatched — this is a deliberate divergence from v1
 * documented in manifest/tcp.md. */

/* ---- fan.tcp.bind_async{...} --------------------------------------------- */

static int l_bind_async(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Reject the same unsupported fields as connect_async. */
    lua_getfield(L, 1, "worker");
    if (!lua_isnil(L, -1)) {
        return luaL_error(L, "fan.tcp.bind_async: the `worker` field is not supported");
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "callback_self_first");
    if (!lua_isnil(L, -1)) {
        return luaL_error(L, "fan.tcp.bind_async: the `callback_self_first` field is not supported");
    }
    lua_pop(L, 1);

    /* Parse strings + numbers. */
    const char *host = NULL;
    lua_getfield(L, 1, "host");
    if (lua_isstring(L, -1)) host = lua_tostring(L, -1);
    /* keep on stack; will pop after strdup */
    int port = 0;
    lua_getfield(L, 1, "port");
    if (lua_isnumber(L, -1)) port = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (port < 0 || port > 65535) {
        lua_pop(L, 1);   /* pop host string */
        return luaL_error(L, "bind_async: port out of range");
    }

    int use_ssl = 0;
    lua_getfield(L, 1, "ssl");
    use_ssl = lua_toboolean(L, -1);
    lua_pop(L, 1);
    const char *cert = NULL, *key = NULL;
    lua_getfield(L, 1, "cert");
    if (lua_isstring(L, -1)) cert = lua_tostring(L, -1);
    /* keep on stack briefly */
    lua_getfield(L, 1, "key");
    if (lua_isstring(L, -1)) key = lua_tostring(L, -1);
    /* keep on stack briefly */

    int send_buf = 0, recv_buf = 0;
    lua_getfield(L, 1, "send_buffer_size");
    if (lua_isnumber(L, -1)) send_buf = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "receive_buffer_size");
    if (lua_isnumber(L, -1)) recv_buf = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    /* Callbacks. */
    int on_accept_ref = LUA_NOREF, on_sslhostname_ref = LUA_NOREF;
    lua_getfield(L, 1, "onaccept");
    if (lua_isfunction(L, -1)) on_accept_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else {
        lua_pop(L, 4);   /* onaccept + key + cert + host */
        return luaL_error(L, "bind_async: onaccept must be a function");
    }
    lua_getfield(L, 1, "onsslhostname");
    if (lua_isfunction(L, -1)) on_sslhostname_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);

    /* Now that we have all we need from the option table, strdup the
     * strings we still need and pop everything from the stack. */
    char *host_dup = host ? strdup(host) : NULL;
    char *cert_dup = cert ? strdup(cert) : NULL;
    char *key_dup  = key  ? strdup(key)  : NULL;
    lua_pop(L, 3);   /* key + cert + host */

    /* Build the TLS server ctx up front (v2 parity: bind should reject
     * bad SSL config immediately rather than at accept time). */
    void *tls_ctx = NULL;
    if (use_ssl) {
        if (!cert_dup || !key_dup) {
            free(host_dup); free(cert_dup); free(key_dup);
            if (on_accept_ref     != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, on_accept_ref);
            if (on_sslhostname_ref!= LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, on_sslhostname_ref);
            lua_pushnil(L);
            lua_pushstring(L, "bind_async{ssl=true} requires cert=... and key=...");
            return 2;
        }
        const char *terr = NULL;
        tls_ctx = fan_tls_server_ctx_new(cert_dup, key_dup, &terr);
        if (!tls_ctx) {
            free(host_dup); free(cert_dup); free(key_dup);
            if (on_accept_ref     != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, on_accept_ref);
            if (on_sslhostname_ref!= LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, on_sslhostname_ref);
            lua_pushnil(L);
            lua_pushfstring(L, "tls server ctx: %s", terr ? terr : "unknown");
            return 2;
        }

        /* onsslhostname (SNI dispatch to Lua) is reserved for a later
         * patch — see the comment near the top of the bind_async block.
         * The ref is already pinned above so leak-free either way. */
    }

    /* Build the sockaddr. */
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (host_dup && strcmp(host_dup, "0.0.0.0") != 0 && host_dup[0]) {
        if (inet_pton(AF_INET, host_dup, &sin.sin_addr) != 1) {
            if (strcmp(host_dup, "localhost") == 0) sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            else {
                free(host_dup); free(cert_dup); free(key_dup);
                if (tls_ctx) fan_tls_server_ctx_free(tls_ctx);
                if (on_accept_ref     != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, on_accept_ref);
                if (on_sslhostname_ref!= LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, on_sslhostname_ref);
                return luaL_error(L, "bind_async host must be numeric IPv4 or localhost");
            }
        }
    } else {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    /* Allocate + populate the server userdata. */
    tcp_async_server_t *sv =
        (tcp_async_server_t *)lua_newuserdata(L, sizeof(*sv));
    memset(sv, 0, sizeof(*sv));
    sv->host = host_dup;
    sv->port = port;
    sv->use_ssl = use_ssl;
    sv->cert_path = cert_dup;
    sv->key_path  = key_dup;
    sv->send_buffer_size    = send_buf;
    sv->receive_buffer_size = recv_buf;
    sv->on_accept_ref       = on_accept_ref;
    sv->on_sslhostname_ref  = on_sslhostname_ref;
    sv->tls_ctx = tls_ctx;
    luaL_getmetatable(L, TCP_ASYNC_SERVER_MT);
    lua_setmetatable(L, -2);

    /* Pin the userdata itself so async_server_accept_cb can push it as
     * arg 1 of onaccept(self, accept).  Unref'd in async_server_gc /
     * l_async_server_close. */
    sv->self_ref = LUA_NOREF;
    lua_pushvalue(L, -1);
    sv->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    struct event_base *base = fan_loop_current_base();
    sv->listener = evconnlistener_new_bind(base, async_server_accept_cb, sv,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
        (struct sockaddr *)&sin, sizeof(sin));
    if (!sv->listener) {
        /* Bev_gc will run for this userdata as soon as we pop it — release
         * the self-pin so it does not keep the failed sv alive. */
        if (sv->self_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, sv->self_ref);
            sv->self_ref = LUA_NOREF;
        }
        lua_pop(L, 1);   /* drop the userdata */
        /* Strings + refs owned by sv are freed via async_server_gc — no
         * manual free() here (double-free otherwise). */
        lua_pushnil(L);
        lua_pushfstring(L, "bind_async listener failed: %s", strerror(errno));
        return 2;
    }
    return 1;
}

/* ---- async server methods: close / rebind / getport --------------------- */

static int l_async_server_close(lua_State *L) {
    tcp_async_server_t *sv =
        (tcp_async_server_t *)luaL_checkudata(L, 1, TCP_ASYNC_SERVER_MT);
    if (sv->closed) return 0;
    if (sv->listener) { evconnlistener_free(sv->listener); sv->listener = NULL; }
    if (sv->on_accept_ref      != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, sv->on_accept_ref);      sv->on_accept_ref      = LUA_NOREF; }
    if (sv->on_sslhostname_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, sv->on_sslhostname_ref); sv->on_sslhostname_ref = LUA_NOREF; }
    if (sv->self_ref           != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, sv->self_ref);           sv->self_ref           = LUA_NOREF; }
    if (sv->tls_ctx) { fan_tls_server_ctx_free(sv->tls_ctx); sv->tls_ctx = NULL; }
    sv->closed = 1;
    return 0;
}

static int l_async_server_rebind(lua_State *L) {
    tcp_async_server_t *sv =
        (tcp_async_server_t *)luaL_checkudata(L, 1, TCP_ASYNC_SERVER_MT);
    if (sv->closed) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    if (sv->listener) { evconnlistener_free(sv->listener); sv->listener = NULL; }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)sv->port);
    if (sv->host && strcmp(sv->host, "0.0.0.0") != 0 && sv->host[0]) {
        if (inet_pton(AF_INET, sv->host, &sin.sin_addr) != 1) {
            if (strcmp(sv->host, "localhost") == 0) sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            else { lua_pushnil(L); lua_pushstring(L, "rebind: bad host"); return 2; }
        }
    } else {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    struct event_base *base = fan_loop_current_base();
    sv->listener = evconnlistener_new_bind(base, async_server_accept_cb, sv,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
        (struct sockaddr *)&sin, sizeof(sin));
    if (!sv->listener) {
        lua_pushnil(L);
        lua_pushfstring(L, "rebind failed: %s", strerror(errno));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_async_server_getport(lua_State *L) {
    tcp_async_server_t *sv =
        (tcp_async_server_t *)luaL_checkudata(L, 1, TCP_ASYNC_SERVER_MT);
    if (!sv->listener) { lua_pushnil(L); lua_pushliteral(L, "listener closed"); return 2; }
    evutil_socket_t fd = evconnlistener_get_fd(sv->listener);
    if (fd < 0) { lua_pushnil(L); lua_pushliteral(L, "no fd"); return 2; }
    struct sockaddr_storage ss;
    socklen_t sl = (socklen_t)sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) {
        lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2;
    }
    unsigned p = 0;
    if (ss.ss_family == AF_INET)  p = ntohs(((struct sockaddr_in  *)&ss)->sin_port);
    if (ss.ss_family == AF_INET6) p = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    lua_pushinteger(L, (lua_Integer)p);
    return 1;
}

static int async_server_gc(lua_State *L) {
    tcp_async_server_t *sv =
        (tcp_async_server_t *)luaL_checkudata(L, 1, TCP_ASYNC_SERVER_MT);
    if (sv->listener) { evconnlistener_free(sv->listener); sv->listener = NULL; }
    if (sv->on_accept_ref      != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, sv->on_accept_ref);      sv->on_accept_ref      = LUA_NOREF; }
    if (sv->on_sslhostname_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, sv->on_sslhostname_ref); sv->on_sslhostname_ref = LUA_NOREF; }
    if (sv->self_ref           != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, sv->self_ref);           sv->self_ref           = LUA_NOREF; }
    if (sv->tls_ctx) { fan_tls_server_ctx_free(sv->tls_ctx); sv->tls_ctx = NULL; }
    if (sv->host)      { free(sv->host);      sv->host      = NULL; }
    if (sv->cert_path) { free(sv->cert_path); sv->cert_path = NULL; }
    if (sv->key_path)  { free(sv->key_path);  sv->key_path  = NULL; }
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

static const luaL_Reg async_conn_methods[] = {
    {"send",         l_async_send},
    {"close",        l_async_close},
    {"reconnect",    l_async_reconnect},
    {"shutdown",     l_async_shutdown},
    {"pause_read",   l_async_pause_read},
    {"resume_read",  l_async_resume_read},
    {"getsockname",  l_async_getsockname},
    {"getpeername",  l_async_getpeername},
    {NULL, NULL},
};

static const luaL_Reg async_accept_methods[] = {
    {"bind",         l_async_accept_bind},
    {"send",         l_async_accept_send},
    {"close",        l_async_accept_close},
    {"flush",        l_async_accept_flush},
    {"remoteinfo",   l_async_accept_remoteinfo},
    {"pause_read",   l_async_accept_pause_read},
    {"resume_read",  l_async_accept_resume_read},
    {NULL, NULL},
};

static const luaL_Reg async_server_methods[] = {
    {"close",   l_async_server_close},
    {"rebind",  l_async_server_rebind},
    {"getport", l_async_server_getport},
    {NULL, NULL},
};

static const luaL_Reg server_methods[] = {
    {"close",   l_server_close},
    {"getport", l_server_getport},
    {NULL, NULL},
};

static const luaL_Reg tcp_funcs[] = {
    {"connect",       l_connect},
    {"connect_async", l_connect_async},
    {"bind",          l_bind},
    {"bind_async",    l_bind_async},
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

    /* async conn metatable (M17: fan.tcp.connect_async result handle) */
    luaL_newmetatable(L, TCP_ASYNC_CONN_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, async_conn_methods, 0);
#else
    luaL_register(L, NULL, async_conn_methods);
#endif
    lua_pushcfunction(L, async_conn_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* async accept metatable (M17-3: accepts handed to onaccept) */
    luaL_newmetatable(L, TCP_ASYNC_ACCEPT_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, async_accept_methods, 0);
#else
    luaL_register(L, NULL, async_accept_methods);
#endif
    lua_pushcfunction(L, async_accept_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* async server metatable (M17-3: fan.tcp.bind_async result handle) */
    luaL_newmetatable(L, TCP_ASYNC_SERVER_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, async_server_methods, 0);
#else
    luaL_register(L, NULL, async_server_methods);
#endif
    lua_pushcfunction(L, async_server_gc);
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
