/*
 * httpd.c — LuaFan v2 HTTP/1.1 server (evhttp backend).
 *
 * M14.C-a: bind + single-shot reply(status, headers, body). See httpd.h for
 * milestone scope.
 *
 * Architecture:
 *
 *   fan.httpd_c.bind{ host, port, handler = fn(req) } builds an
 *   `struct evhttp *` bound to a socket and installs a generic callback
 *   (`httpd_gencb`). When a request arrives evhttp finishes parsing headers
 *   + body up-front (single-thread, single reactor -> we do not need chunked
 *   request bodies for M14.C-a), then calls our gencb. There we spin up a
 *   fresh Lua coroutine (same pattern as fan.tcp.bind server_accept_cb),
 *   push the user handler + a fan.httpd.request userdata wrapping the
 *   evhttp_request, and resume. The handler runs to completion (may
 *   fan.sleep between C calls -> coroutine yields, the loop keeps ticking
 *   for other requests) and eventually calls `req:reply(...)`. If it
 *   returns without replying, we send a default 204; if it errors, we send
 *   a 500 provided the head has not been sent yet.
 *
 *   Lifetime of the evhttp_request pointer:
 *     - libevent OWNS it. It is valid from the moment gencb is called
 *       until we call evhttp_send_reply / evhttp_send_error, or until
 *       evhttp_free tears down mid-flight. We stamp the userdata's `ev`
 *       field to NULL at completion so any late Lua-side call raises a
 *       clean error instead of touching freed memory.
 *     - If the server closes while a coroutine is parked (e.g. inside
 *       fan.sleep), the evhttp_free path invalidates every in-flight
 *       request. We wire an evhttp on-completion callback per request so
 *       we know when libevent is done with the pointer even if reply was
 *       already called from Lua.
 *
 * NOT in scope for M14.C-a (each has its own milestone):
 *   - reply_start / reply_chunk / reply_end  (M14.C-b) [DONE]
 *   - addheader accumulation + caller-wins merge (M14.C-b) [DONE]
 *   - HTTPS (evhttp_set_bevcb + fan_tls_server_bev)  (M14.C-c) [DONE]
 *   - WebSocket upgrade -> fan.websocket bridge  (M14.C-d)
 *
 * The Lua-side dispatch shim (lua/fan/httpd.lua) routes bind calls to the
 * pure-Lua backend (httpd_lua.lua) only when opts.backend="lua" or the
 * global override is set. HTTPS bind (opts.ssl=true + cert/key) is
 * handled here directly since M14.C-c.
 */
#include "httpd.h"
#include "httpd_metrics.h"
#include "../platform.h"
#include "../runtime/loop.h"
#include "../runtime/coro.h"
#include "tls.h"
#include "websocket.h"   /* fan_ws_conn_push — M14.D handshake -> ws userdata */

#include <lua.h>
#include <lauxlib.h>

#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/queue.h>    /* TAILQ_INIT for evkeyvalq (libevent's key/value list) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define FAN_HTTPD_SERVER_MT  "fan.httpd_c.server"
#define FAN_HTTPD_REQUEST_MT "fan.httpd_c.request"
#define FAN_HTTPD_HEADERS_MT "fan.httpd_c.headers"

/* Main thread of the owning lua_State, stashed at module load. evhttp callbacks
 * fire from libevent with no state pointer of their own, so we resume every
 * handler on the main state's registry (the same pattern fan.tcp uses). This
 * MUST be the state's main thread (fan_coro_main), never the coroutine that
 * happened to run `require "fan"`. */
static lua_State *g_main_L = NULL;

/* ---- server userdata ---------------------------------------------------- */

typedef struct {
    struct evhttp *http;
    evutil_socket_t bound_fd; /* the listening socket (from _with_handle) */
    int handler_ref;          /* Lua registry ref to the user's handler fn */
    int closed;
    /* M14.C-c: server-side TLS context (SSL_CTX *) when opts.ssl=true;
     * NULL otherwise. Freed by fan_tls_server_ctx_free on close. */
    void *tls_ctx;
    /* M14.C-j: strdup'd metrics path (e.g. "/metrics") when opts.metrics
     * is set, else NULL. When non-NULL, evhttp_set_cb bypasses gencb for
     * that path and dumps Prometheus text directly — we do NOT run the
     * user handler for the scrape URL. */
    char *metrics_path;
} httpd_server_t;

/* Global registry of active TLS servers keyed on struct evhttp *, so the
 * bevcb (which only receives its bev_arg pointer) can look up the SSL_CTX
 * we handed it and be robust across close/reload. The bev_arg IS the
 * SSL_CTX pointer directly — no lookup needed — but we keep this comment
 * so the design intent is obvious. */

/* ---- request/response userdata ------------------------------------------- */

typedef struct {
    /* Valid only while libevent owns the request. Set to NULL by
     * request_completion_cb (evhttp finished writing the reply and freed
     * the underlying object) OR by server:close() invalidation. */
    struct evhttp_request *ev;

    /* Body cache: evhttp buffered the entire request body before calling
     * gencb, so we drain it once into `body` and expose it via :read /
     * :available / :body. Length is `body_len`; the read cursor is
     * `body_pos` (1-based like the Lua-side impl). */
    char  *body;
    size_t body_len;
    size_t body_pos;

    int sent_head;    /* 1 once reply()/reply_start()/send_error has been called */
    int detached;     /* 1 after server:close() cut us off from libevent */

    /* Chunked-reply mode state (M14.C-b). */
    int chunked;      /* 1 between reply_start() and reply_end() */
    int finished;     /* 1 once reply_end() has been called (extra guard) */

    /* Headers accumulated via resp:addheader(k, v) prior to the head being
     * sent (M14.C-b). Kept as a libevent-owned evkeyvalq so we can push
     * them into the request's output headers in one shot at reply time,
     * applying caller-wins case-insensitive replacement first. Multiple
     * addheader() calls with the same key fold values with ", " to match
     * v1 semantics; the fold happens inside l_req_addheader. */
    struct evkeyvalq pending_headers;
    int pending_headers_init;

    /* M14.C-j metrics accounting: mirror v1 httpd's finish_metrics guard.
     * `metrics_finished` = 1 once request_end has been logged; every path
     * that terminates a request (normal reply, chunked reply_end,
     * send_error 500/503/etc., server_close teardown, or gc as a
     * last-resort) checks this flag first, so we never double-count and
     * never miss a request. `status_hint` and `bytes_hint` remember the
     * values so the various exit branches don't have to plumb them
     * through their signatures. */
    int    metrics_finished;
    int    status_hint;
    size_t bytes_hint;
} httpd_request_t;

/* ---- helpers ------------------------------------------------------------- */

/* Terminal metrics logger — idempotent per request. Every exit path calls
 * this; the first call wins. Same guard shape as v1 httpd_finish_metrics.
 * `default_status`: if the caller doesn't know the wire status (e.g.
 * server close mid-request) we use 499 like v1 ("client closed connection"
 * as a stand-in for "server never got to send a reply"). */
static void httpd_finish_metrics(httpd_request_t *r, int default_status) {
    if (!r || r->metrics_finished) return;
    r->metrics_finished = 1;
    int status = r->status_hint > 0 ? r->status_hint : default_status;
    fan_httpd_metrics_request_end(status, r->bytes_hint);
}

static const char *reason_for(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

/* HTTP method -> uppercase name string. evhttp defines the enum but we want
 * to match v1 fan.httpd's `req.method` string form ("GET", "POST", ...). */
static const char *method_name(enum evhttp_cmd_type t) {
    switch (t) {
        case EVHTTP_REQ_GET:     return "GET";
        case EVHTTP_REQ_POST:    return "POST";
        case EVHTTP_REQ_HEAD:    return "HEAD";
        case EVHTTP_REQ_PUT:     return "PUT";
        case EVHTTP_REQ_DELETE:  return "DELETE";
        case EVHTTP_REQ_OPTIONS: return "OPTIONS";
        case EVHTTP_REQ_TRACE:   return "TRACE";
        case EVHTTP_REQ_CONNECT: return "CONNECT";
        case EVHTTP_REQ_PATCH:   return "PATCH";
        default:                 return "UNKNOWN";
    }
}

/* Split URI at '?'. Writes the (possibly empty) query start into *qs, and
 * NUL-terminates the path portion in-place inside a caller-owned copy. */
static void split_path_query(char *uri_copy, char **path_out, char **query_out) {
    char *q = strchr(uri_copy, '?');
    if (q) {
        *q = '\0';
        *query_out = q + 1;
    } else {
        *query_out = uri_copy + strlen(uri_copy);  /* empty */
    }
    *path_out = uri_copy;
}

/* Percent-decode `s` in place, writing to `out` (must be >= strlen(s) + 1
 * bytes). Also converts '+' to space (application/x-www-form-urlencoded
 * style, matching v1 fan.httpd parse_query). Returns the decoded length. */
static size_t urldecode_into(const char *s, char *out) {
    size_t n = 0;
    while (*s) {
        if (*s == '+') { out[n++] = ' '; s++; continue; }
        if (*s == '%' && s[1] && s[2]) {
            int hi = -1, lo = -1;
            char a = s[1], b = s[2];
            if (a >= '0' && a <= '9') hi = a - '0';
            else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
            else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
            if (b >= '0' && b <= '9') lo = b - '0';
            else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
            else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
            if (hi >= 0 && lo >= 0) {
                out[n++] = (char)((hi << 4) | lo);
                s += 3;
                continue;
            }
        }
        out[n++] = *s++;
    }
    out[n] = '\0';
    return n;
}

/* Push a Lua table representing the query string. Called with `qs` pointing
 * at the raw query (may be empty). Table shape: { k = v, ... } — same as
 * v1 fan.httpd parse_query, and the Lua-side impl. */
static void push_query_table(lua_State *L, const char *qs) {
    lua_newtable(L);
    if (!qs || !*qs) return;
    /* We iterate & split by '&'; use a stack buffer sized to the raw pair. */
    const char *p = qs;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
        if (pair_len > 0) {
            /* find '=' inside the pair */
            const char *eq = memchr(p, '=', pair_len);
            const char *kstart = p;
            size_t klen = eq ? (size_t)(eq - p) : pair_len;
            const char *vstart = eq ? eq + 1 : NULL;
            size_t vlen = eq ? (pair_len - klen - 1) : 0;

            /* decode into heap (small keys/values usually, but be safe) */
            char *kbuf = (char *)malloc(klen + 1);
            char *vbuf = (char *)malloc(vlen + 1);
            if (!kbuf || !vbuf) { free(kbuf); free(vbuf); goto next; }
            memcpy(kbuf, kstart, klen); kbuf[klen] = '\0';
            memcpy(vbuf, vstart ? vstart : "", vlen); vbuf[vlen] = '\0';
            char *kdec = (char *)malloc(klen + 1);
            char *vdec = (char *)malloc(vlen + 1);
            if (!kdec || !vdec) { free(kbuf); free(vbuf); free(kdec); free(vdec); goto next; }
            urldecode_into(kbuf, kdec);
            urldecode_into(vbuf, vdec);
            lua_pushstring(L, kdec);
            lua_pushstring(L, vdec);
            lua_settable(L, -3);
            free(kbuf); free(vbuf); free(kdec); free(vdec);
        }
    next:
        if (!amp) break;
        p = amp + 1;
    }
}

/* __index for req.headers: keys are stored lowercased (the documented v2
 * shape), but v1 code looked headers up with their wire spelling
 * ("If-None-Match", "Accept-Encoding"), so retry with a lowercased key. */
static int l_headers_lookup(lua_State *L) {
    if (lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    size_t len = 0;
    const char *k = lua_tolstring(L, 2, &len);
    char stackbuf[128];
    char *low = (len + 1 <= sizeof(stackbuf)) ? stackbuf : (char *)malloc(len + 1);
    if (!low) {
        lua_pushnil(L);
        return 1;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)k[i];
        low[i] = (char)((c >= 'A' && c <= 'Z') ? (c + 32) : c);
    }
    low[len] = '\0';
    lua_pushlstring(L, low, len);
    lua_rawget(L, 1);
    if (low != stackbuf) free(low);
    return 1;
}

/* Push a Lua table with all request headers, lowercased keys, per v1 shape.
 * Duplicate headers get folded with ", " (matches parse_request in the Lua
 * backend and RFC 7230 §3.2.2). A metatable makes lookups case-insensitive. */
static void push_headers_table(lua_State *L, struct evkeyvalq *hs) {
    lua_newtable(L);
    /* v1 parity: header reads stay case-insensitive (see l_headers_lookup). */
    luaL_getmetatable(L, FAN_HTTPD_HEADERS_MT);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaL_newmetatable(L, FAN_HTTPD_HEADERS_MT);
        lua_pushcfunction(L, l_headers_lookup);
        lua_setfield(L, -2, "__index");
    }
    lua_setmetatable(L, -2);
    if (!hs) return;
    struct evkeyval *kv;
    for (kv = hs->tqh_first; kv; kv = kv->next.tqe_next) {
        if (!kv->key || !kv->value) continue;
        /* lowercase the key into a stack buf (headers are typically short) */
        size_t klen = strlen(kv->key);
        char stackbuf[128];
        char *low = (klen + 1 <= sizeof(stackbuf)) ? stackbuf : (char *)malloc(klen + 1);
        if (!low) continue;
        for (size_t i = 0; i < klen; i++) {
            unsigned char c = (unsigned char)kv->key[i];
            low[i] = (char)((c >= 'A' && c <= 'Z') ? (c + 32) : c);
        }
        low[klen] = '\0';

        /* fold on duplicate keys */
        lua_pushstring(L, low);
        lua_pushvalue(L, -1);
        lua_rawget(L, -3);
        if (lua_isstring(L, -1)) {
            const char *existing = lua_tostring(L, -1);
            lua_pushfstring(L, "%s, %s", existing, kv->value);
            lua_remove(L, -2);      /* drop the old value from get */
            lua_settable(L, -3);    /* t[low] = old..", "..new */
        } else {
            lua_pop(L, 1);          /* drop nil from get */
            lua_pushstring(L, kv->value);
            lua_settable(L, -3);    /* t[low] = new */
        }
        if (low != stackbuf) free(low);
    }
}

/* ---- request userdata helpers ------------------------------------------- */

static httpd_request_t *check_request(lua_State *L, int idx) {
    return (httpd_request_t *)luaL_checkudata(L, idx, FAN_HTTPD_REQUEST_MT);
}

/* Push a new request userdata wrapping `ev`; leaves it on top of L. Drains
 * the request body eagerly (evhttp has already fully buffered it). */
static httpd_request_t *request_push_new(lua_State *L, struct evhttp_request *ev) {
    httpd_request_t *r = (httpd_request_t *)lua_newuserdata(L, sizeof(*r));
    memset(r, 0, sizeof(*r));
    r->ev = ev;
    r->body_pos = 1;                 /* 1-based, matches v1 Lua backend */
    /* evkeyvalq requires an explicit init before add/find/remove — memset
     * to zero is not enough on all libevent versions. TAILQ_INIT is a
     * macro from <sys/queue.h> pulled in via evhttp headers. */
    TAILQ_INIT(&r->pending_headers);
    r->pending_headers_init = 1;

    struct evbuffer *in = evhttp_request_get_input_buffer(ev);
    if (in) {
        size_t len = evbuffer_get_length(in);
        if (len > 0) {
            r->body = (char *)malloc(len);
            if (r->body) {
                evbuffer_remove(in, r->body, len);
                r->body_len = len;
            }
        }
    }
    luaL_getmetatable(L, FAN_HTTPD_REQUEST_MT);
    lua_setmetatable(L, -2);
    return r;
}

/* Push req.method / req.path / req.query / req.headers / req.body onto the
 * userdata's uservalue table so `req.method` etc. resolve via __index. We
 * do this once at construction; libevent's parse doesn't change post-gencb. */
#if LUA_VERSION_NUM >= 502
#  define FAN_SETUSERVALUE(L, idx) lua_setuservalue((L), (idx))
#  define FAN_GETUSERVALUE(L, idx) lua_getuservalue((L), (idx))
#else
#  define FAN_SETUSERVALUE(L, idx) lua_setfenv((L), (idx))
#  define FAN_GETUSERVALUE(L, idx) lua_getfenv((L), (idx))
#endif

static void request_fill_fields(lua_State *L, int req_idx, struct evhttp_request *ev) {
    /* Normalise req_idx to an absolute stack index. The rest of this
     * function pushes several table values (uv, uri, headers, ...) which
     * would otherwise shift a negative index off the userdata. */
    if (req_idx < 0 && req_idx > LUA_REGISTRYINDEX) {
        req_idx = lua_gettop(L) + req_idx + 1;
    }

    /* Build the uservalue table with pre-parsed request-line fields. */
    lua_newtable(L);   /* uv */

    /* method */
    lua_pushstring(L, method_name(evhttp_request_get_command(ev)));
    lua_setfield(L, -2, "method");

    /* raw URI (evhttp: request-target). We also split path/query. */
    const char *uri = evhttp_request_get_uri(ev);
    if (!uri) uri = "";
    lua_pushstring(L, uri);
    lua_setfield(L, -2, "target");

    /* path + query: parse a mutable copy */
    size_t ulen = strlen(uri);
    char *ucopy = (char *)malloc(ulen + 1);
    if (ucopy) {
        memcpy(ucopy, uri, ulen + 1);
        char *path = NULL, *query = NULL;
        split_path_query(ucopy, &path, &query);
        lua_pushstring(L, path);
        lua_setfield(L, -2, "path");
        push_query_table(L, query);
        lua_setfield(L, -2, "query");
        free(ucopy);
    } else {
        lua_pushliteral(L, "");
        lua_setfield(L, -2, "path");
        lua_newtable(L);
        lua_setfield(L, -2, "query");
    }

    /* params (v1 shape): the query string merged with an
     * application/x-www-form-urlencoded body — v1 ran evhttp_parse_query_str
     * over both, in this order, so the form body wins on collisions. Values
     * are always strings, matching v1. */
    {
        const char *qmark = strchr(uri, '?');
        push_query_table(L, qmark ? qmark + 1 : "");
        httpd_request_t *rq = (httpd_request_t *)lua_touserdata(L, req_idx);
        const char *ctype = evhttp_find_header(
            evhttp_request_get_input_headers(ev), "Content-Type");
        if (ctype && strstr(ctype, "application/x-www-form-urlencoded") == ctype &&
            rq->body && rq->body_len > 0) {
            char *form = (char *)malloc(rq->body_len + 1);
            if (form) {
                memcpy(form, rq->body, rq->body_len);
                form[rq->body_len] = '\0';
                push_query_table(L, form);
                free(form);
                int fidx = lua_gettop(L);   /* form table */
                int pidx = fidx - 1;        /* params table (just below) */
                lua_pushnil(L);
                while (lua_next(L, fidx) != 0) {
                    lua_pushvalue(L, -2);   /* key */
                    lua_pushvalue(L, -2);   /* value */
                    lua_rawset(L, pidx);    /* params[key] = value */
                    lua_pop(L, 1);          /* keep the key for lua_next */
                }
                lua_pop(L, 1);              /* drop the form table */
            }
        }
        lua_setfield(L, -2, "params");
    }

    /* headers */
    push_headers_table(L, evhttp_request_get_input_headers(ev));
    lua_setfield(L, -2, "headers");

    /* http_version: evhttp exposes major/minor on the request struct */
    lua_pushfstring(L, "%d.%d", (int)ev->major, (int)ev->minor);
    lua_setfield(L, -2, "http_version");

    /* body (mirrored for read-only access as req.body; :read/:available
     * consume the internal cursor and do not touch this string) */
    httpd_request_t *r = (httpd_request_t *)lua_touserdata(L, req_idx);
    if (r->body && r->body_len > 0) {
        lua_pushlstring(L, r->body, r->body_len);
    } else {
        lua_pushliteral(L, "");
    }
    lua_setfield(L, -2, "body");

    /* Stash uv onto the userdata. In Lua 5.2+ this is the sole uservalue;
     * we then use __index to read from it in the method metatable. */
    FAN_SETUSERVALUE(L, req_idx);
}

/* ---- reply path --------------------------------------------------------- */

/* Case-insensitive: remove every header with `name` from `q`. libevent's
 * evhttp_remove_header only removes the first match, so we loop. */
static void remove_header_all_ci(struct evkeyvalq *q, const char *name) {
    while (evhttp_remove_header(q, name) == 0) {
        /* keep going */
    }
}

/* Push every (k,v) from `src` into `dst` via evhttp_add_header. */
static void copy_headers(struct evkeyvalq *dst, struct evkeyvalq *src) {
    struct evkeyval *h;
    for (h = src->tqh_first; h; h = h->next.tqe_next) {
        evhttp_add_header(dst, h->key, h->value);
    }
}

/* Merge headers into the request's output headers, honouring v1 semantics:
 *   1. Start from `pending_headers` (accumulated by resp:addheader).
 *   2. For each key in the caller-supplied `headers_idx` Lua table, drop
 *      any pending entry with that key (case-insensitive) so the caller
 *      value replaces (not appends) — "caller wins".
 *   3. Add the caller headers, skipping Content-Length (evhttp inserts it
 *      itself for send_reply and duplicates confuse some clients).
 * Returns 0 on success, or pushes (nil, err) to L and returns 2. */
static int commit_headers(lua_State *L, httpd_request_t *r, int headers_idx) {
    struct evkeyvalq *out = evhttp_request_get_output_headers(r->ev);

    /* v1 callers pass a reason-phrase string in this slot
     * (:reply(status, message, body) / :reply_start(status, message)); any
     * non-table value is ignored so both call shapes work. */
    /* 1 + 2: seed with pending, then knock out anything the caller
     * overrides. We do the removal BEFORE pushing pending to `out` so we
     * only need to walk pending_headers once. */
    if (headers_idx != 0 && lua_type(L, headers_idx) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, headers_idx) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char *k = lua_tostring(L, -2);
                /* remove any pending header with the same case-insensitive name */
                if (r->pending_headers_init) {
                    remove_header_all_ci(&r->pending_headers, k);
                }
            }
            lua_pop(L, 1);
        }
    }
    /* Flush surviving pending headers to output first. */
    if (r->pending_headers_init) {
        copy_headers(out, &r->pending_headers);
    }
    /* Then push caller-supplied headers. */
    if (headers_idx != 0 && lua_type(L, headers_idx) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, headers_idx) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) {
                const char *k = lua_tostring(L, -2);
                const char *v = lua_tostring(L, -1);
                int skip = 0;
                if (k[0] == 'C' || k[0] == 'c') {
                    if (strcasecmp(k, "Content-Length") == 0) skip = 1;
                }
                if (!skip) {
                    evhttp_add_header(out, k, v);
                }
            }
            lua_pop(L, 1);
        }
    }
    return 0;
}

/* Send a status + headers + body via evhttp_send_reply. Consumes buf. */
static int do_reply(lua_State *L, httpd_request_t *r, int status,
                    int headers_idx, const char *body, size_t body_len) {
    if (r->sent_head) {
        return luaL_error(L, "reply: response head already sent");
    }
    if (r->detached || !r->ev) {
        /* Server was closed while we were parked, or evhttp already freed
         * the request. Nothing safe to send; report an error to Lua so the
         * handler can bail out cleanly. */
        lua_pushnil(L);
        lua_pushliteral(L, "reply: connection detached");
        return 2;
    }

    commit_headers(L, r, headers_idx);

    /* Body -> output buffer. Even for empty body we call send_reply with a
     * (possibly empty) buffer to force libevent to write the head. */
    struct evbuffer *outbuf = evbuffer_new();
    if (!outbuf) {
        lua_pushnil(L);
        lua_pushliteral(L, "reply: evbuffer_new failed");
        return 2;
    }
    if (body && body_len > 0) {
        if (evbuffer_add(outbuf, body, body_len) != 0) {
            evbuffer_free(outbuf);
            lua_pushnil(L);
            lua_pushliteral(L, "reply: evbuffer_add failed");
            return 2;
        }
    }

    r->sent_head = 1;
    /* M14.C-j: record what we're sending BEFORE libevent takes over. */
    r->status_hint = status;
    r->bytes_hint  = body ? body_len : 0;
    evhttp_send_reply(r->ev, status, reason_for(status), outbuf);
    evbuffer_free(outbuf);
    httpd_finish_metrics(r, status);
    /* After send_reply libevent will finish writing the response and then
     * free the request via its own path; our request_completion_cb (if
     * installed) fires then. Do NOT touch r->ev after this point. */

    lua_pushboolean(L, 1);
    return 1;
}

/* ---- Lua-facing methods on the request userdata ------------------------- */

/* req:available() -> bytes remaining in the buffered body. */
static int l_req_available(lua_State *L) {
    httpd_request_t *r = check_request(L, 1);
    lua_Integer remaining = 0;
    if (r->body_len >= r->body_pos - 1) {
        remaining = (lua_Integer)(r->body_len - (r->body_pos - 1));
    }
    lua_pushinteger(L, remaining);
    return 1;
}

/* req:read([n]) -> up to n bytes (or all remaining) or nil at EOF. */
static int l_req_read(lua_State *L) {
    httpd_request_t *r = check_request(L, 1);

    /* Validate the argument up-front so `req:read(-1)` raises even after
     * the body has been fully drained. The pure-Lua backend has the same
     * property (its arg check runs before the remaining-bytes check). */
    lua_Integer n = -1;
    int arg_supplied = !lua_isnoneornil(L, 2);
    if (arg_supplied) {
        if (lua_type(L, 2) != LUA_TNUMBER) {
            return luaL_error(L, "read: n must be a positive number or nil");
        }
        n = lua_tointeger(L, 2);
        if (n <= 0) {
            return luaL_error(L, "read: n must be a positive number or nil");
        }
    }

    size_t remaining = 0;
    if (r->body_len >= r->body_pos - 1) {
        remaining = r->body_len - (r->body_pos - 1);
    }
    if (remaining == 0) {
        lua_pushnil(L);
        return 1;
    }
    if (!arg_supplied) {
        n = (lua_Integer)remaining;
    }
    size_t take = (size_t)((lua_Integer)remaining < n ? (lua_Integer)remaining : n);
    lua_pushlstring(L, r->body + (r->body_pos - 1), take);
    r->body_pos += take;
    return 1;
}

/* req:reply(status, headers, body) — one-shot v1 signature. */
static int l_req_reply(lua_State *L) {
    httpd_request_t *r = check_request(L, 1);
    int status = (int)luaL_checkinteger(L, 2);
    size_t body_len = 0;
    const char *body = NULL;
    if (!lua_isnoneornil(L, 4)) {
        body = luaL_checklstring(L, 4, &body_len);
    } else {
        body = "";
        body_len = 0;
    }
    return do_reply(L, r, status, 3, body, body_len);
}

/* resp:addheader(name, value) — accumulate a response header before the head
 * is sent. Multiple calls with the SAME name fold their values with ", "
 * (HTTP header folding, RFC 7230 §3.2.2 combined form), matching the v1
 * fan.httpd behaviour (see luafan2/lua/fan/httpd_lua.lua Response:addheader).
 * Headers passed later to :reply / :reply_start override these on collision
 * (case-insensitive). */
static int l_req_addheader(lua_State *L) {
    httpd_request_t *r = check_request(L, 1);
    if (r->sent_head) {
        return luaL_error(L, "addheader: response head already sent");
    }
    if (lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "addheader: (name:string, value) required");
    }
    if (lua_isnoneornil(L, 3)) {
        return luaL_error(L, "addheader: (name:string, value) required");
    }
    const char *name = lua_tostring(L, 2);
    /* Coerce value to string (v1 uses tostring(value)). Accept string /
     * number / boolean by delegating to lua_tostring after luaL_tolstring
     * on newer Lua; for portability we just require it convert cleanly. */
    const char *value;
    if (lua_isstring(L, 3)) {
        value = lua_tostring(L, 3);
    } else {
        /* Fall back to tostring() through luaL. */
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, 3);
        lua_call(L, 1, 1);
        value = lua_tostring(L, -1);
        if (!value) {
            return luaL_error(L, "addheader: value must be stringable");
        }
        /* keep the coerced string on stack until we're done copying it */
    }

    if (!r->pending_headers_init) {
        TAILQ_INIT(&r->pending_headers);
        r->pending_headers_init = 1;
    }

    /* Check for an existing case-insensitive match. libevent's
     * evhttp_find_header is already case-insensitive. If present we fold
     * "old, new" and replace; otherwise we just add. */
    const char *existing = evhttp_find_header(&r->pending_headers, name);
    if (existing) {
        /* Build "existing, value" into a heap buffer; evhttp_add_header
         * copies. */
        size_t elen = strlen(existing);
        size_t vlen = strlen(value);
        size_t need = elen + 2 + vlen + 1;
        char *combined = (char *)malloc(need);
        if (!combined) {
            return luaL_error(L, "addheader: out of memory");
        }
        memcpy(combined, existing, elen);
        combined[elen] = ',';
        combined[elen + 1] = ' ';
        memcpy(combined + elen + 2, value, vlen);
        combined[elen + 2 + vlen] = '\0';
        /* Purge every case-insensitive occurrence first, then add the
         * combined value once. */
        remove_header_all_ci(&r->pending_headers, name);
        int rc = evhttp_add_header(&r->pending_headers, name, combined);
        free(combined);
        if (rc != 0) {
            return luaL_error(L, "addheader: evhttp_add_header failed");
        }
    } else {
        if (evhttp_add_header(&r->pending_headers, name, value) != 0) {
            return luaL_error(L, "addheader: evhttp_add_header failed");
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* resp:reply_start(status, headers?) — begin a chunked response. Sends the
 * status line + response headers with Transfer-Encoding: chunked; individual
 * chunks follow via resp:reply_chunk(data). Terminate with resp:reply_end().
 * libevent's evhttp_send_reply_start owns the wire framing (chunk-size lines,
 * final "0\r\n\r\n") so we do not have to emit those ourselves. Any caller
 * Content-Length is silently dropped (chunked mode owns framing) to match
 * the v1 Lua backend. */
static int l_req_reply_start(lua_State *L) {
    httpd_request_t *r = check_request(L, 1);
    if (r->sent_head) {
        return luaL_error(L, "reply_start: response head already sent");
    }
    if (r->detached || !r->ev) {
        lua_pushnil(L);
        lua_pushliteral(L, "reply_start: connection detached");
        return 2;
    }
    int status = (int)luaL_checkinteger(L, 2);

    /* Merge caller headers with pending (addheader) headers, caller-wins.
     * Content-Length is filtered inside commit_headers. Transfer-Encoding
     * is inserted automatically by evhttp_send_reply_start. */
    commit_headers(L, r, lua_isnoneornil(L, 3) ? 0 : 3);

    r->sent_head = 1;
    r->chunked = 1;
    r->finished = 0;
    /* M14.C-j: capture the intended status for the metrics hint; bytes
     * accumulate as reply_chunk fires. finish_metrics runs on reply_end. */
    r->status_hint = status;
    evhttp_send_reply_start(r->ev, status, reason_for(status));
    lua_pushboolean(L, 1);
    return 1;
}

/* resp:reply_chunk(data) — send one chunk of a chunked response. Empty /
 * nil data is a no-op (matches v1 Lua backend). Fails if reply_start has
 * not been called, or reply_end has already been called. */
static int l_req_reply_chunk(lua_State *L) {
    httpd_request_t *r = check_request(L, 1);
    if (!r->chunked) {
        return luaL_error(L, "reply_chunk: not in chunked mode");
    }
    if (r->finished) {
        return luaL_error(L, "reply_chunk: response already ended");
    }
    if (r->detached || !r->ev) {
        lua_pushnil(L);
        lua_pushliteral(L, "reply_chunk: connection detached");
        return 2;
    }
    if (lua_isnoneornil(L, 2)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    size_t dlen = 0;
    const char *data = luaL_checklstring(L, 2, &dlen);
    if (dlen == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    struct evbuffer *chunk = evbuffer_new();
    if (!chunk) {
        return luaL_error(L, "reply_chunk: evbuffer_new failed");
    }
    if (evbuffer_add(chunk, data, dlen) != 0) {
        evbuffer_free(chunk);
        return luaL_error(L, "reply_chunk: evbuffer_add failed");
    }
    evhttp_send_reply_chunk(r->ev, chunk);
    evbuffer_free(chunk);
    /* M14.C-j: accumulate bytes on the metrics hint. */
    r->bytes_hint += dlen;
    lua_pushboolean(L, 1);
    return 1;
}

/* resp:reply_end() — finish a chunked response. Idempotent (matches v1). */
static int l_req_reply_end(lua_State *L) {
    httpd_request_t *r = check_request(L, 1);
    if (!r->chunked) {
        return luaL_error(L, "reply_end: not in chunked mode");
    }
    if (r->finished) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (r->detached || !r->ev) {
        lua_pushnil(L);
        lua_pushliteral(L, "reply_end: connection detached");
        return 2;
    }
    r->finished = 1;
    evhttp_send_reply_end(r->ev);
    /* M14.C-j: chunked reply is finalised — log the metrics now with the
     * status set at reply_start and bytes accumulated across all chunks. */
    httpd_finish_metrics(r, r->status_hint > 0 ? r->status_hint : 200);
    /* After send_reply_end libevent finishes writing and frees the request
     * on its own. Do NOT touch r->ev after this point. */
    lua_pushboolean(L, 1);
    return 1;
}

/* --- M14.C-d: WebSocket upgrade helpers --------------------------------- */

/* Case-insensitive substring search — libevent's evhttp_find_header is
 * case-insensitive on the header name only; header VALUES like
 * "Upgrade" or "keep-alive, Upgrade" need our own compare. */
static int ws_hdr_has_token_ci(const char *hay, const char *needle) {
    if (!hay || !needle) return 0;
    size_t nlen = strlen(needle);
    const char *p = hay;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        const char *tok = p;
        while (*p && *p != ',' ) p++;
        size_t tl = (size_t)(p - tok);
        while (tl > 0 && (tok[tl-1] == ' ' || tok[tl-1] == '\t')) tl--;
        if (tl == nlen) {
            size_t i;
            for (i = 0; i < nlen; i++) {
                char a = tok[i], b = needle[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) break;
            }
            if (i == nlen) return 1;
        }
        if (*p == ',') p++;
    }
    return 0;
}

/* req:is_websocket_upgrade() — true iff the request headers advertise an
 * RFC 6455 upgrade (Upgrade: websocket, Connection includes Upgrade,
 * Sec-WebSocket-Key set, Sec-WebSocket-Version == 13). Read-only, no I/O. */
static int l_req_is_ws_upgrade(lua_State *L) {
    httpd_request_t *r = check_request(L, 1);
    if (!r->ev) { lua_pushboolean(L, 0); return 1; }
    struct evkeyvalq *ih = evhttp_request_get_input_headers(r->ev);
    if (!ih) { lua_pushboolean(L, 0); return 1; }
    const char *upg = evhttp_find_header(ih, "Upgrade");
    const char *con = evhttp_find_header(ih, "Connection");
    const char *key = evhttp_find_header(ih, "Sec-WebSocket-Key");
    const char *ver = evhttp_find_header(ih, "Sec-WebSocket-Version");
    int ok = 0;
    if (upg && con && key && ver && strcmp(ver, "13") == 0) {
        /* upgrade header must equal "websocket" (case-insensitive, single token) */
        size_t ul = strlen(upg);
        if (ul == 9) {
            char buf[16]; size_t i;
            for (i = 0; i < 9; i++) {
                char c = upg[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                buf[i] = c;
            }
            buf[9] = '\0';
            if (strcmp(buf, "websocket") == 0 &&
                ws_hdr_has_token_ci(con, "upgrade")) {
                ok = 1;
            }
        }
    }
    lua_pushboolean(L, ok);
    return 1;
}

#ifdef FAN_WITH_OPENSSL
/* We use openssl's SHA1 + base64 for the accept-key digest so we do not
 * pull yet another SHA1 into httpd.c. The Lua backend uses fan.utils.
 * All builds we care about compile OpenSSL in (client TLS, server TLS),
 * so this is not a real dependency. Guarded so an OpenSSL-off build
 * still links (websocket_accept just raises then). */
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#endif

/* Compute Sec-WebSocket-Accept per RFC 6455: base64(SHA1(key + GUID)).
 * out must have >= 32 bytes. Returns 0 on success, -1 on failure. */
static int ws_compute_accept(const char *key, char *out, size_t outsz) {
#ifdef FAN_WITH_OPENSSL
    static const char *GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char sha[SHA_DIGEST_LENGTH];
    size_t klen = strlen(key), glen = strlen(GUID);
    unsigned char *buf = (unsigned char *)malloc(klen + glen);
    if (!buf) return -1;
    memcpy(buf, key, klen);
    memcpy(buf + klen, GUID, glen);
    SHA1(buf, klen + glen, sha);
    free(buf);
    /* base64 encode into out (SHA1 = 20 bytes -> 28 chars incl padding) */
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    b64 = BIO_push(b64, mem);
    if (BIO_write(b64, sha, SHA_DIGEST_LENGTH) != SHA_DIGEST_LENGTH) {
        BIO_free_all(b64); return -1;
    }
    (void)BIO_flush(b64);
    BUF_MEM *bptr = NULL;
    BIO_get_mem_ptr(b64, &bptr);
    if (!bptr || bptr->length >= outsz) { BIO_free_all(b64); return -1; }
    memcpy(out, bptr->data, bptr->length);
    out[bptr->length] = '\0';
    BIO_free_all(b64);
    return 0;
#else
    (void)key; (void)out; (void)outsz;
    return -1;
#endif
}

/* Parse Sec-WebSocket-Extensions for a permessage-deflate offer. Returns
 * 1 if the client offered permessage-deflate at all, 0 otherwise. The C
 * backend answers with the same "no context takeover" reply the Lua
 * backend uses so fan.ws.conn's send/recv paths behave the same. */
static int ws_client_offers_pmd(const char *ext) {
    if (!ext) return 0;
    /* case-insensitive substring is enough — the header value is a
     * comma-separated list of offers and we accept the first that
     * mentions "permessage-deflate". */
    const char *needle = "permessage-deflate";
    size_t nlen = strlen(needle);
    const char *p = ext;
    while (*p) {
        size_t i;
        for (i = 0; i < nlen && p[i]; i++) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != b) break;
        }
        if (i == nlen) return 1;
        p++;
    }
    return 0;
}

/* req:websocket_accept() — perform the RFC 6455 handshake and hand the
 * underlying bufferevent to fan.ws (C data plane). Returns the ws
 * userdata on success; on failure returns nil + an error string (the
 * handshake head is not on the wire yet so callers may still resp:reply
 * a 4xx). Requires OpenSSL for the SHA1/base64 digest (all supported
 * builds enable it).
 *
 * The upgrade flow (M14.D, contrast with M14.C-d):
 *   1. evhttp_request_own(r->ev) tells evhttp not to free the request
 *      when we return without sending a normal reply.
 *   2. We write the 101 head directly to the underlying bev; evhttp is
 *      no longer in charge of framing.
 *   3. fan_ws_conn_push takes over the bev's callbacks, installs its
 *      own read state machine + evcon closecb, and owns evcon/ev_req
 *      teardown at :close / __gc time. */
static int l_req_ws_accept(lua_State *L) {
    httpd_request_t *r = check_request(L, 1);
    if (!r->ev) {
        lua_pushnil(L);
        lua_pushliteral(L, "websocket_accept: request already completed");
        return 2;
    }
    if (r->sent_head) {
        lua_pushnil(L);
        lua_pushliteral(L, "websocket_accept: response head already sent");
        return 2;
    }
#ifndef FAN_WITH_OPENSSL
    lua_pushnil(L);
    lua_pushliteral(L, "websocket_accept: built without OpenSSL "
                       "(SHA1/base64 unavailable)");
    return 2;
#else
    struct evkeyvalq *ih = evhttp_request_get_input_headers(r->ev);
    const char *upg = ih ? evhttp_find_header(ih, "Upgrade") : NULL;
    const char *con = ih ? evhttp_find_header(ih, "Connection") : NULL;
    const char *key = ih ? evhttp_find_header(ih, "Sec-WebSocket-Key") : NULL;
    const char *ver = ih ? evhttp_find_header(ih, "Sec-WebSocket-Version") : NULL;
    if (!upg || !key || !ver || strcmp(ver, "13") != 0 || !con ||
        !ws_hdr_has_token_ci(con, "upgrade")) {
        lua_pushnil(L);
        lua_pushliteral(L, "websocket_accept: not a websocket upgrade request");
        return 2;
    }

    char accept_key[64];
    if (ws_compute_accept(key, accept_key, sizeof(accept_key)) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "websocket_accept: SHA1/base64 digest failed");
        return 2;
    }

    const char *ext = evhttp_find_header(ih, "Sec-WebSocket-Extensions");
    int deflate = ws_client_offers_pmd(ext);

    struct evhttp_connection *evcon = evhttp_request_get_connection(r->ev);
    struct bufferevent *bev = evcon ? evhttp_connection_get_bufferevent(evcon) : NULL;
    if (!bev) {
        lua_pushnil(L);
        lua_pushliteral(L, "websocket_accept: no underlying bufferevent");
        return 2;
    }

    /* Compose + send the 101 head directly to the bev. bufferevent_write
     * queues bytes; libevent will drain them on the writer callback. */
    char head[512];
    int hn;
    if (deflate) {
        hn = snprintf(head, sizeof(head),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n"
            "Sec-WebSocket-Extensions: permessage-deflate; "
            "server_no_context_takeover; client_no_context_takeover\r\n"
            "\r\n", accept_key);
    } else {
        hn = snprintf(head, sizeof(head),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n"
            "\r\n", accept_key);
    }
    if (hn <= 0 || (size_t)hn >= sizeof(head)) {
        lua_pushnil(L);
        lua_pushliteral(L, "websocket_accept: handshake head format overflow");
        return 2;
    }

    /* Write the 101 head into the bev output buffer BEFORE we hand the
     * bev over to the ws userdata. libevent will flush it on the next
     * event loop tick once we enable EV_WRITE inside fan_ws_conn_push. */
    if (bufferevent_write(bev, head, (size_t)hn) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "websocket_accept: failed to queue 101 head");
        return 2;
    }

    /* Silence the evhttp-side close/timeout callbacks and let the ws
     * userdata install its own closecb; own the request out of evhttp's
     * internal request list so evhttp_request_free is deferred to the
     * ws userdata's :close / __gc. */
    evhttp_connection_set_closecb(evcon, NULL, NULL);
    evhttp_connection_set_timeout(evcon, 0);
    struct evhttp_request *owned_ev = r->ev;
    evhttp_request_own(owned_ev);
    r->ev = NULL;
    r->detached = 1;

    /* Build the fan.ws.conn userdata directly on the bev + evcon. It
     * takes over readcb/writecb/eventcb, installs an evhttp closecb
     * hook so server:close teardown invalidates it cleanly, and owns
     * the evcon + ev_req lifetime from here on. Also stash it under
     * "_ws" on the request uservalue so the shim's proxy can find it
     * via req:websocket_send / websocket_receive. */
    fan_ws_conn_push(L, evcon, bev, owned_ev, deflate);
    /* stack: ... [ws userdata @ top] */

    lua_getuservalue(L, 1);
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, -2);         /* ws */
        lua_setfield(L, -2, "_ws");
    }
    lua_pop(L, 1);   /* pop uservalue (leaves ws on top) */

    return 1;
#endif
}

/* __gc: free the body buffer and any un-flushed pending headers. Do not
 * touch r->ev — libevent owns it. */
static int req_gc(lua_State *L) {
    httpd_request_t *r = check_request(L, 1);
    if (r->body) { free(r->body); r->body = NULL; }
    if (r->pending_headers_init) {
        evhttp_clear_headers(&r->pending_headers);
        r->pending_headers_init = 0;
    }
    return 0;
}

static const luaL_Reg request_methods[] = {
    {"available",             l_req_available},
    {"read",                  l_req_read},
    {"reply",                 l_req_reply},
    {"addheader",             l_req_addheader},
    {"reply_start",           l_req_reply_start},
    {"reply_chunk",           l_req_reply_chunk},
    {"reply_end",             l_req_reply_end},
    /* M14.C-d: WebSocket entry points. is_websocket_upgrade is a pure
     * header predicate on the C userdata; websocket_accept does the
     * handshake, steals the bufferevent into fan.tcp, and returns a
     * fan.websocket ws object. Everything else (send/receive/ping/pong/
     * close/state) is proxied on the Lua side (see wrap_c_handler in
     * lua/fan/httpd.lua) so those v1 aliases go through the ws object. */
    {"is_websocket_upgrade",  l_req_is_ws_upgrade},
    {"websocket_accept",      l_req_ws_accept},
    {NULL, NULL},
};

/* __index for the request userdata: first try the method table, then the
 * uservalue table (which holds method/path/query/headers/body strings). */
static int req_index(lua_State *L) {
    /* method lookup */
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "__methods");
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 3);   /* nil, methods, mt */

    /* uservalue lookup */
    FAN_GETUSERVALUE(L, 1);
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
        return 1;
    }
    lua_pop(L, 1);
    lua_pushnil(L);
    return 1;
}

/* ---- gencb: the main dispatch --------------------------------------------
 * libevent calls this on the main state (we run single-thread), so we can
 * safely grab g_main_L and spawn a coroutine to run the handler in. */
/* M14.C-j: Prometheus /metrics scrape handler. Registered per-server
 * when opts.metrics is set. Uses direct evbuffer_add + evhttp_send_reply
 * so we bypass gencb/handler entirely — the scrape must NOT go through
 * Lua because that would count the scrape itself and let a Lua handler
 * hijack the response, which is what v1 users on OpenTelemetry care
 * about (a metrics scrape must be side-effect free).
 *
 * Note: we DO still count the scrape request itself in requests_total /
 * bytes_sent — v1 does the same, and the alternative (subtract-your-own
 * scrape) is a maintenance headache. Scrape volume is very low
 * (Prometheus default is 15s) so the noise floor is negligible. */
static void httpd_metrics_cb(struct evhttp_request *ev, void *arg) {
    (void)arg;
    fan_httpd_metrics_request_start(method_name(evhttp_request_get_command(ev)));
    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(ev, 500, "Internal Server Error");
        fan_httpd_metrics_request_end(500, 0);
        return;
    }
    size_t n = fan_httpd_metrics_render(buf);
    /* Prometheus 0.0.4 text format — the exact content-type Prometheus
     * expects. Anything else and some scrapers refuse to parse. */
    evhttp_add_header(evhttp_request_get_output_headers(ev),
                      "Content-Type",
                      "text/plain; version=0.0.4; charset=utf-8");
    evhttp_send_reply(ev, 200, "OK", buf);
    evbuffer_free(buf);
    fan_httpd_metrics_request_end(200, n);
}

static void httpd_gencb(struct evhttp_request *ev, void *arg) {
    httpd_server_t *sv = (httpd_server_t *)arg;

    /* M14.C-j: count this request as soon as we own it, so pathological
     * exits (503 / 500 below, or a handler that never replies) still get
     * an accurate request_start entry. request_end fires from
     * httpd_finish_metrics further down (or from the reply / auto-204 /
     * error paths that already know the status). */
    fan_httpd_metrics_request_start(method_name(evhttp_request_get_command(ev)));

    if (sv->closed || sv->handler_ref == LUA_NOREF) {
        /* Server torn down between accept + dispatch. Respond 503 and let
         * libevent free the request on its own. */
        evhttp_send_error(ev, 503, "Service Unavailable");
        fan_httpd_metrics_request_end(503, 0);
        return;
    }

    lua_State *L = g_main_L;
    if (!L) {
        evhttp_send_error(ev, 500, "Internal Server Error");
        fan_httpd_metrics_request_end(500, 0);
        return;
    }

    /* Fresh coroutine + registry pin (same shape as tcp server_accept_cb). */
    lua_State *co = lua_newthread(L);
    lua_pushvalue(L, -1);
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);

    /* Build the handler call: fn(req). The Lua backend's serve_one passes
     * (req, resp) but v2's C backend collapses response methods onto the
     * request userdata (v1 shape). Any Lua-side dispatch shim can pass a
     * shim-wrapped double-return if the caller really wants two args. */
    lua_rawgeti(co, LUA_REGISTRYINDEX, sv->handler_ref);   /* fn */
    httpd_request_t *r = request_push_new(co, ev);
    request_fill_fields(co, -1, ev);

    /* Resume with 1 arg (the req/resp userdata). The coroutine may yield
     * (fan.sleep, tcp receive, etc.) — the loop keeps ticking. */
    int status = fan_coro_resume(co, 1);

    if (status == LUA_OK) {
        /* Handler returned cleanly. Three cases:
         *  a) Chunked reply in flight but not ended -> auto reply_end
         *     (matches serve_one() in the Lua backend).
         *  b) Head never sent at all -> default 204.
         *  c) Reply already fully written -> nothing to do (finish_metrics
         *     already ran inside do_reply / l_req_reply_end). */
        if (r->chunked && !r->finished && !r->detached && r->ev) {
            r->finished = 1;
            evhttp_send_reply_end(r->ev);
            httpd_finish_metrics(r, r->status_hint > 0 ? r->status_hint : 200);
        } else if (!r->sent_head && !r->detached && r->ev) {
            struct evbuffer *empty = evbuffer_new();
            r->sent_head = 1;
            r->status_hint = 204;
            r->bytes_hint  = 0;
            evhttp_send_reply(r->ev, 204, reason_for(204), empty);
            if (empty) evbuffer_free(empty);
            httpd_finish_metrics(r, 204);
        } else {
            /* Reply already fully written (case c). If do_reply / reply_end
             * haven't logged for some reason, do it now. */
            httpd_finish_metrics(r, r->status_hint > 0 ? r->status_hint : 200);
        }
    } else if (status != LUA_YIELD) {
        /* Handler crashed. fan_coro_resume already logged the traceback.
         * If the head has not been committed at all -> send a 500. If we
         * are mid-chunked-reply, we cannot rewrite the status line; the
         * best we can do is terminate the chunk stream cleanly so the
         * client is not left hanging. */
        if (r->chunked && !r->finished && !r->detached && r->ev) {
            r->finished = 1;
            evhttp_send_reply_end(r->ev);
            /* Mid-chunked crash: the client already saw a 2xx head, so
             * bookkeeping-wise we count it as its original status class.
             * Bump errors_total explicitly since a mid-stream crash IS an
             * error even though the wire status was e.g. 200. */
            httpd_finish_metrics(r, r->status_hint > 0 ? r->status_hint : 200);
            fan_httpd_metrics_add_recv(0);  /* no-op; explicit anchor */
        } else if (!r->sent_head && !r->detached && r->ev) {
            struct evbuffer *out = evbuffer_new();
            /* The coroutine's stack still has the error on top. Include it. */
            const char *msg = lua_tostring(co, -1);
            if (!msg) msg = "handler error";
            evbuffer_add_printf(out, "Internal Server Error: %s", msg);
            r->sent_head = 1;
            r->status_hint = 500;
            r->bytes_hint  = evbuffer_get_length(out);
            evhttp_send_reply(r->ev, 500, reason_for(500), out);
            evbuffer_free(out);
            httpd_finish_metrics(r, 500);
        } else {
            /* Head already sent + not chunked (a plain reply already went
             * out) and then the coroutine crashed after replying. Metrics
             * already logged inside do_reply. */
            httpd_finish_metrics(r, r->status_hint > 0 ? r->status_hint : 200);
        }
    } else {
        /* LUA_YIELD: the handler parked. Something else (fan.sleep,
         * a socket wake) will resume it eventually. The follow-up reply /
         * default-204 has to happen from that resume path; the Lua-side
         * shim can enforce that by wrapping the handler with a
         * "did-you-reply" tail, but for M14.C-a we accept that a handler
         * which parks + never replies leaks the request until server
         * teardown. This matches the current Lua backend which has the
         * same property (a handler that yields forever holds the fd). */
    }

    /* If the handler returned/erroed synchronously we can drop the ref now.
     * If it yielded, we STILL drop it: the coroutine is pinned separately
     * by fan.sleep / tcp / etc. via their own park_ref, exactly as
     * fan_coro_wake does for tcp. Same invariant. */
    luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
}

/* ---- bind / close ------------------------------------------------------- */

static httpd_server_t *check_server(lua_State *L, int idx) {
    return (httpd_server_t *)luaL_checkudata(L, idx, FAN_HTTPD_SERVER_MT);
}

#ifdef FAN_WITH_OPENSSL
/* evhttp bufferevent constructor callback: libevent calls this for every
 * accepted connection so we can hand back a TLS-wrapped bufferevent.
 * evhttp then plugs the accepted socket fd into it via bufferevent_setfd.
 * arg is the SSL_CTX * we passed to evhttp_set_bevcb.
 *
 * Note: we build the bev with fd=-1 (like v1 fan.httpd does), because
 * evhttp will attach the accepted socket right after this returns.
 * fan_tls_server_bev already applies BEV_OPT_CLOSE_ON_FREE +
 * BEV_OPT_DEFER_CALLBACKS + allow_dirty_shutdown, which is what we want. */
static struct bufferevent *httpd_bevcb(struct event_base *base, void *arg) {
    const char *err = NULL;
    struct bufferevent *bev = fan_tls_server_bev(base, -1, arg, &err);
    if (!bev) {
        /* Nothing safe to do from here — libevent will treat the NULL as
         * accept failure and drop the connection. Log for the operator. */
        fprintf(stderr, "httpd_c: fan_tls_server_bev failed: %s\n",
                err ? err : "(unknown)");
    }
    return bev;
}
#endif

/* fan.httpd_c.bind{ host, port, handler, ssl?, cert?, key? } -> server userdata | nil,err */
static int l_bind(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "host");
    const char *host = lua_isstring(L, -1) ? lua_tostring(L, -1) : "127.0.0.1";
    /* keep on stack: popped after we're done reading opts */

    lua_getfield(L, 1, "port");
    if (!lua_isnumber(L, -1)) {
        return luaL_error(L, "httpd_c.bind: opts.port required (number)");
    }
    int port = (int)lua_tointeger(L, -1);
    if (port < 0 || port > 65535) {
        return luaL_error(L, "httpd_c.bind: port out of range");
    }

    /* M14.C-c: TLS options. Accepted forms (matching v1 fan.httpd):
     *   opts.ssl = true   (requires opts.cert + opts.key)
     *   opts.cert = "path/to/cert.pem"   (implies ssl=true)
     *   opts.key  = "path/to/key.pem"
     * cert/key are file paths. We defer building the SSL_CTX until after
     * we have handler_ref pinned so the error paths are symmetric. */
    lua_getfield(L, 1, "ssl");
    int want_ssl = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "cert");
    const char *cert_path = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_getfield(L, 1, "key");
    const char *key_path  = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    /* keep cert/key on stack until after ctx creation */

    if ((want_ssl || cert_path || key_path)) {
        if (!cert_path || !key_path) {
            /* Soft failure to match the pure-Lua backend's contract
             * (nil, err) — v1 fan.httpd + test_httpsd.lua rely on this. */
            lua_pushnil(L);
            lua_pushliteral(L,
                "httpd.bind: opts.ssl requires opts.cert and opts.key");
            return 2;
        }
#ifndef FAN_WITH_OPENSSL
        lua_pushnil(L);
        lua_pushliteral(L,
            "httpd.bind: opts.ssl requested but TLS not compiled in "
            "(build with -DFAN_WITH_OPENSSL=ON)");
        return 2;
#endif
    }

    lua_getfield(L, 1, "handler");
    if (!lua_isfunction(L, -1)) {
        return luaL_error(L, "httpd_c.bind: opts.handler required (function)");
    }
    /* pin the handler; leaves the fn popped */
    int handler_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* Build the server SSL context up-front so a bad cert/key aborts
     * before we touch the network. Freed on any failure path below via
     * `tls_ctx_local` -> fan_tls_server_ctx_free, and adopted onto the
     * userdata on the success path. */
    void *tls_ctx_local = NULL;
    if (want_ssl || cert_path) {
#ifdef FAN_WITH_OPENSSL
        const char *terr = NULL;
        tls_ctx_local = fan_tls_server_ctx_new(cert_path, key_path, &terr);
        if (!tls_ctx_local) {
            luaL_unref(L, LUA_REGISTRYINDEX, handler_ref);
            lua_pushnil(L);
            lua_pushfstring(L, "httpd_c.bind: TLS ctx: %s", terr ? terr : "unknown error");
            return 2;
        }
#endif
    }

    /* Build the listening sockaddr. IPv4 only for M14.C-a to match fan.tcp
     * bind's constraint (v6 support is future work). */
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (host && host[0] && strcmp(host, "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, host, &sin.sin_addr) != 1) {
            if (strcmp(host, "localhost") == 0) {
                sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            } else {
#ifdef FAN_WITH_OPENSSL
                if (tls_ctx_local) fan_tls_server_ctx_free(tls_ctx_local);
#endif
                luaL_unref(L, LUA_REGISTRYINDEX, handler_ref);
                return luaL_error(L, "httpd_c.bind: host must be a numeric IPv4 or localhost");
            }
        }
    } else {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    struct event_base *base = fan_loop_current_base();
    if (!base) {
#ifdef FAN_WITH_OPENSSL
        if (tls_ctx_local) fan_tls_server_ctx_free(tls_ctx_local);
#endif
        luaL_unref(L, LUA_REGISTRYINDEX, handler_ref);
        lua_pushnil(L);
        lua_pushliteral(L, "no event base");
        return 2;
    }

    struct evhttp *http = evhttp_new(base);
    if (!http) {
#ifdef FAN_WITH_OPENSSL
        if (tls_ctx_local) fan_tls_server_ctx_free(tls_ctx_local);
#endif
        luaL_unref(L, LUA_REGISTRYINDEX, handler_ref);
        lua_pushnil(L);
        lua_pushliteral(L, "evhttp_new failed");
        return 2;
    }

    /* Bind via a socket we create ourselves so we can pass SO_REUSEADDR
     * and honour our own sockaddr construction. evhttp_bind_socket_with_handle
     * (which takes host+port strings) would also work but limits us to the
     * hostname-string path. */
    evutil_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
#ifdef FAN_WITH_OPENSSL
        if (tls_ctx_local) fan_tls_server_ctx_free(tls_ctx_local);
#endif
        evhttp_free(http);
        luaL_unref(L, LUA_REGISTRYINDEX, handler_ref);
        lua_pushnil(L);
        lua_pushliteral(L, "socket() failed");
        return 2;
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
#ifdef FAN_WITH_OPENSSL
        if (tls_ctx_local) fan_tls_server_ctx_free(tls_ctx_local);
#endif
        evutil_closesocket(fd);
        evhttp_free(http);
        luaL_unref(L, LUA_REGISTRYINDEX, handler_ref);
        lua_pushnil(L);
        lua_pushfstring(L, "bind: %s", strerror(errno));
        return 2;
    }
    if (listen(fd, 128) != 0) {
#ifdef FAN_WITH_OPENSSL
        if (tls_ctx_local) fan_tls_server_ctx_free(tls_ctx_local);
#endif
        evutil_closesocket(fd);
        evhttp_free(http);
        luaL_unref(L, LUA_REGISTRYINDEX, handler_ref);
        lua_pushnil(L);
        lua_pushfstring(L, "listen: %s", strerror(errno));
        return 2;
    }
    if (evutil_make_socket_nonblocking(fd) != 0) {
#ifdef FAN_WITH_OPENSSL
        if (tls_ctx_local) fan_tls_server_ctx_free(tls_ctx_local);
#endif
        evutil_closesocket(fd);
        evhttp_free(http);
        luaL_unref(L, LUA_REGISTRYINDEX, handler_ref);
        lua_pushnil(L);
        lua_pushliteral(L, "evutil_make_socket_nonblocking failed");
        return 2;
    }
    if (evhttp_accept_socket(http, fd) != 0) {
#ifdef FAN_WITH_OPENSSL
        if (tls_ctx_local) fan_tls_server_ctx_free(tls_ctx_local);
#endif
        evutil_closesocket(fd);
        evhttp_free(http);
        luaL_unref(L, LUA_REGISTRYINDEX, handler_ref);
        lua_pushnil(L);
        lua_pushliteral(L, "evhttp_accept_socket failed");
        return 2;
    }

    /* Install the TLS bevcb BEFORE we hand the userdata to Lua so that
     * any connection attempted between now and when Lua starts polling
     * (which is not really possible on a single-threaded loop, but be
     * defensive) is already TLS-terminated. */
#ifdef FAN_WITH_OPENSSL
    if (tls_ctx_local) {
        evhttp_set_bevcb(http, httpd_bevcb, tls_ctx_local);
    }
#endif

    /* M14.C-j: opts.metrics = "/some/path" -> expose Prometheus scrape.
     * Read here (before sv is malloc'd) so a mis-typed opts value fails
     * cleanly. We strdup so evhttp_set_cb's path pointer stays live even
     * after the opts table is GC'd. */
    lua_getfield(L, 1, "metrics");
    const char *metrics_path_raw = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    char *metrics_path_dup = NULL;
    if (metrics_path_raw && metrics_path_raw[0] == '/') {
        metrics_path_dup = strdup(metrics_path_raw);
    } else if (metrics_path_raw) {
        /* Reject relative / malformed paths early. Silent-accept would
         * confuse users when the scrape URL 404's later. */
        lua_pushnil(L);
        lua_pushliteral(L,
            "httpd.bind: opts.metrics must be a path starting with '/'");
        return 2;
    }
    lua_pop(L, 1);

    /* Allocate server userdata AFTER accept succeeded so failures do not
     * leave a half-constructed userdata visible to Lua. */
    httpd_server_t *sv = (httpd_server_t *)lua_newuserdata(L, sizeof(*sv));
    memset(sv, 0, sizeof(*sv));
    sv->http = http;
    sv->bound_fd = fd;
    sv->handler_ref = handler_ref;
    sv->tls_ctx = tls_ctx_local;   /* adopts; freed in close/gc */
    sv->metrics_path = metrics_path_dup;   /* adopts; freed in close/gc */
    luaL_getmetatable(L, FAN_HTTPD_SERVER_MT);
    lua_setmetatable(L, -2);

    evhttp_set_gencb(http, httpd_gencb, sv);
    /* Metrics path registered AFTER gencb so it shadows the general
     * handler for exactly this URL. */
    if (sv->metrics_path) {
        evhttp_set_cb(http, sv->metrics_path, httpd_metrics_cb, NULL);
    }

    /* M14.C-j: bump connection counter now that the listen socket is
     * live. This is a slight over-count vs v1 which incremented on every
     * accept, but v2 has no accept callback to hook — libevent hides
     * accepted fds inside evhttp — and one-per-bind at least tells you
     * the server is up. Follow-up work: wire an accept probe. */
    fan_httpd_metrics_connection();

    return 1;
}

static int l_server_close(lua_State *L) {
    httpd_server_t *sv = check_server(L, 1);
    if (!sv->closed) {
        /* M14.D: no detached_conns bookkeeping is needed anymore. WS
         * upgrades produced fan_ws_conn userdatas that own their evcon
         * outright and installed a closecb via
         * evhttp_connection_set_closecb; libevent's evhttp_free walks
         * http->connections, calls each evcon's closecb (which our ws
         * userdata uses to null out its evcon/ev_req/bev pointers so
         * its own __gc/:close become no-ops), then frees the evcon +
         * bev + fd. That is exactly the libevent-native contract, so
         * we just need evhttp_free itself here.
         *
         * Known M14.C-a limitation: if a handler has yielded (e.g. via
         * fan.sleep) and is still parked when server:close() fires,
         * evhttp_free will invalidate the parked request pointer. The
         * subsequent reply from Lua will hit the sent_head guard OR
         * dereference a freed evhttp_request. M14.C-a's tests always
         * close the server *after* all in-flight requests complete;
         * detach-on-close bookkeeping lands in M14.C-b together with
         * chunked reply state tracking (see planning note in httpd.h). */
        if (sv->http) { evhttp_free(sv->http); sv->http = NULL; }
        /* M14.C-c: free SSL_CTX AFTER evhttp_free — evhttp holds live
         * bufferevents that internally reference the SSL, and each bev's
         * SSL was created via SSL_new(ctx) which up-refs the ctx; the
         * bev free chain releases those refs, so freeing the ctx here is
         * ordering-safe (it drops our own ref, real destruction happens
         * when the last SSL_free lands). */
        if (sv->tls_ctx) {
#ifdef FAN_WITH_OPENSSL
            fan_tls_server_ctx_free(sv->tls_ctx);
#endif
            sv->tls_ctx = NULL;
        }
        if (sv->handler_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, sv->handler_ref);
            sv->handler_ref = LUA_NOREF;
        }
        if (sv->metrics_path) {
            free(sv->metrics_path);
            sv->metrics_path = NULL;
        }
        sv->closed = 1;
    }
    return 0;
}

static int l_server_getport(lua_State *L) {
    httpd_server_t *sv = check_server(L, 1);
    if (sv->closed || sv->bound_fd < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "server closed");
        return 2;
    }
    struct sockaddr_storage ss;
    socklen_t sl = (socklen_t)sizeof(ss);
    if (getsockname(sv->bound_fd, (struct sockaddr *)&ss, &sl) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    unsigned p = 0;
    if (ss.ss_family == AF_INET)  p = ntohs(((struct sockaddr_in  *)&ss)->sin_port);
    if (ss.ss_family == AF_INET6) p = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    lua_pushinteger(L, (lua_Integer)p);
    return 1;
}

static int server_gc(lua_State *L) {
    httpd_server_t *sv = check_server(L, 1);
    if (sv->http) { evhttp_free(sv->http); sv->http = NULL; }
    if (sv->tls_ctx) {
#ifdef FAN_WITH_OPENSSL
        fan_tls_server_ctx_free(sv->tls_ctx);
#endif
        sv->tls_ctx = NULL;
    }
    if (sv->handler_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, sv->handler_ref);
        sv->handler_ref = LUA_NOREF;
    }
    if (sv->metrics_path) {
        free(sv->metrics_path);
        sv->metrics_path = NULL;
    }
    return 0;
}

static const luaL_Reg server_methods[] = {
    {"close",   l_server_close},
    {"getport", l_server_getport},
    {NULL, NULL},
};

/* M14.C-j: fan.httpd_c.metrics() -> flat table of counter values, keyed by
 * short name (requests_total, bytes_sent, uptime_seconds, ...). Same
 * numbers as the /metrics HTTP scrape but shaped for Lua consumers
 * (dashboards embedded in the process, log lines, tests). Callable at
 * any time, whether a server is bound or not. */
static int l_metrics(lua_State *L) {
    fan_httpd_metrics_push_table(L);
    return 1;
}

static const luaL_Reg httpd_funcs[] = {
    {"bind",    l_bind},
    {"metrics", l_metrics},
    {NULL, NULL},
};

void fan_httpd_register(lua_State *L) {
    g_main_L = fan_coro_main(L);  /* stable main thread, not the require() coroutine */

    /* Request metatable. We route __index through a C dispatch so
     * `req.method` (uservalue lookup) and `req:reply(...)` (method table
     * lookup) both work off the same userdata. */
    luaL_newmetatable(L, FAN_HTTPD_REQUEST_MT);
    /* stash method table under __methods so req_index can find it */
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, request_methods, 0);
#else
    luaL_register(L, NULL, request_methods);
#endif
    lua_setfield(L, -2, "__methods");
    lua_pushcfunction(L, req_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, req_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* Server metatable. */
    luaL_newmetatable(L, FAN_HTTPD_SERVER_MT);
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

    /* fan.httpd_c = { bind = ... } on the fan module at -1 */
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, httpd_funcs, 0);
#else
    luaL_register(L, NULL, httpd_funcs);
#endif
    lua_setfield(L, -2, "httpd_c");
}

void fan_httpd_clear_lua_state(void) {
    /* Companion to fan_httpd_register — clear the cached main-thread pointer
     * before lua_close so late evhttp gencb / bufferevent callbacks (which
     * read g_main_L to spawn per-request coroutines) see NULL and take the
     * `if (!L)` early-return branch instead of dereferencing a dangling
     * lua_State. See runtime/coro.h for the runtime-wide teardown contract. */
    g_main_L = NULL;
}
