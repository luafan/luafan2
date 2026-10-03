/*
 * http.c — LuaFan v2 HTTP/1.1 client backend (libcurl multi + libevent).
 *
 *   * Milestone M13.C-a: expose `fan.http_c.request{...}` performing a single
 *     HTTP request using libcurl's multi interface driven by our libevent
 *     event_base. GET/POST/PUT/DELETE/HEAD/PATCH/OPTIONS/UPDATE, headers,
 *     body, query merge, timeout, TLS verify. Redirects are followed at the
 *     Lua shim level (kept out of C so redirect-loop bookkeeping is one
 *     place); this backend passes the raw response for the shim to inspect.
 *
 *     Also exposes escape/unescape via curl_easy_escape / curl_easy_unescape
 *     for exact v1 byte-for-byte parity.
 *
 * Architecture notes:
 *
 *   * ONE global CURLM (curl_multi_init) is shared across the process,
 *     analogous to the single event_base in loop.c. libcurl multi is
 *     thread-affine; we run everything on the main thread's event_base so
 *     this is safe.
 *
 *   * libevent integration: CURLMOPT_SOCKETFUNCTION + CURLMOPT_TIMERFUNCTION
 *     let libcurl tell us which fds it wants us to watch and when the next
 *     internal timeout fires. On each fd/timer wake we call
 *     curl_multi_socket_action, then drain curl_multi_info_read for
 *     completions and resume the associated Lua coroutine.
 *
 *   * Each pending request pins its parked coroutine in the registry via
 *     fan_coro_park (same shape as tcp/conn:receive) so the fresh coroutine
 *     created by fan.spawn survives across GC.
 *
 *   * l_request is a blocking Lua call from the caller's perspective: it
 *     parks the current coroutine on the request and yields; the multi-info
 *     drain callback pushes the result onto the coroutine's stack and
 *     resumes it, which returns from lua_yield with the right values.
 *
 *   * We COPY the response body into a growable buffer during the transfer
 *     (WRITEFUNCTION), then materialise it as a single Lua string at
 *     completion. Same shape as the Lua backend's do_once.
 *
 * Only compiled when FAN_WITH_CURL is defined; otherwise fan_http_register
 * installs a small stub table with `available=false` so the Lua shim can
 * detect the missing backend and fall back to the pure-Lua implementation.
 */
#include "http.h"

#include <lauxlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef FAN_WITH_CURL

#include <curl/curl.h>
#include <event2/event.h>
#include <event2/buffer.h>

#include "../runtime/loop.h"
#include "../runtime/coro.h"
#include "../platform.h"

/* ---- module-global state -------------------------------------------------
 * The multi handle and its one-shot "internal timer" are process-global.
 * We init them lazily on first request so tests that never touch fan.http
 * don't pay the price. */
static lua_State  *g_main_L    = NULL;
static CURLM      *g_multi     = NULL;
static struct event *g_timer   = NULL;      /* CURLMOPT_TIMERFUNCTION drives it */
static int         g_ncurrent  = 0;         /* outstanding request count */

/* ---- curl_easy_cleanup debug probe --------------------------------------
 * We are chasing a flaky "double free or corruption (!prev)" glibc abort
 * that only surfaces under FAN_COVERAGE builds of test_httpd_c / test_http_c.
 * Backtrace consistently ends at libcurl's curl_easy_cleanup → __libc_free,
 * so *some* CURL* is being freed twice — but the crash aborts before we
 * can see which call site did what.
 *
 * The probe: wrap every curl_easy_cleanup() call through fan_curl_cleanup()
 * which records the pointer in a small ring buffer keyed by call site
 * ("multi_info" / "l_request_fail" / "req_gc"). If we see the same pointer
 * cleaned twice, we fprintf stderr with both call sites and abort() BEFORE
 * libc's malloc arena corruption kicks in — that way the resulting core /
 * gdb backtrace points straight at the offending logic instead of at
 * whichever unlucky free tripped the arena check.
 *
 * Zero cost when FAN_HTTP_C_TRACE_CLEANUP is not defined; enable via
 * -DFAN_HTTP_C_TRACE_CLEANUP=1 in the CMake coverage flags. */
#ifdef FAN_HTTP_C_TRACE_CLEANUP
#include <execinfo.h>
#define CLEANUP_RING_N 128
#define CLEANUP_BT_DEPTH 24
static struct {
    CURL       *easy;
    const char *site;
    int         bt_n;
    void       *bt[CLEANUP_BT_DEPTH];
} g_cleanup_ring[CLEANUP_RING_N];
static unsigned g_cleanup_pos = 0;

static void fan_curl_cleanup(CURL *easy, const char *site) {
    if (!easy) return;
    /* Walk the ring newest→oldest. If we find a cleanup with the same easy
     * pointer AND its "reinit" flag (bt_n < 0 sentinel) has not marked it
     * stale, that's a genuine double-free. If the pointer got reused by a
     * later curl_easy_init we mark that ring slot stale on the init call
     * (see fan_curl_after_init below). */
    for (unsigned i = 0; i < CLEANUP_RING_N; i++) {
        if (g_cleanup_ring[i].easy == easy && g_cleanup_ring[i].bt_n > 0) {
            fprintf(stderr,
                "\n[fan.http_c] DOUBLE curl_easy_cleanup DETECTED\n"
                "  easy    = %p\n"
                "  first   = %s (bt %d frames)\n"
                "  second  = %s\n",
                (void *)easy, g_cleanup_ring[i].site,
                g_cleanup_ring[i].bt_n, site);
            fprintf(stderr, "  --- FIRST call site backtrace ---\n");
            fflush(stderr);
            backtrace_symbols_fd(g_cleanup_ring[i].bt,
                                 g_cleanup_ring[i].bt_n, 2);
            fprintf(stderr, "  --- SECOND call site (current) — gdb will show ---\n");
            fflush(stderr);
            abort();
        }
    }
    g_cleanup_ring[g_cleanup_pos].easy = easy;
    g_cleanup_ring[g_cleanup_pos].site = site;
    g_cleanup_ring[g_cleanup_pos].bt_n =
        backtrace(g_cleanup_ring[g_cleanup_pos].bt, CLEANUP_BT_DEPTH);
    g_cleanup_pos = (g_cleanup_pos + 1) % CLEANUP_RING_N;
    curl_easy_cleanup(easy);
}

/* Mark any prior ring entries with this pointer as stale — the pointer has
 * been re-issued by curl_easy_init, so any future cleanup of it is a
 * distinct object, not a double-free of the earlier one. Called right after
 * every curl_easy_init in this file. */
static void fan_curl_after_init(CURL *easy) {
    if (!easy) return;
    for (unsigned i = 0; i < CLEANUP_RING_N; i++) {
        if (g_cleanup_ring[i].easy == easy) {
            g_cleanup_ring[i].bt_n = -1;   /* stale marker */
        }
    }
}
#define CURL_EASY_INIT_TRACKED(e) do { fan_curl_after_init(e); } while (0)
#define CURL_EASY_CLEANUP(easy_, site_) fan_curl_cleanup((easy_), (site_))
#else
#define CURL_EASY_INIT_TRACKED(e) ((void)(e))
#define CURL_EASY_CLEANUP(easy_, site_) curl_easy_cleanup(easy_)
#endif

#define REQ_MT "fan.http_c.request"

/* Per-request context. Referenced from:
 *   - the easy handle via CURLOPT_PRIVATE (recovered in the info-read loop)
 *   - a Lua userdata (metatable REQ_MT) on the parked coroutine's stack
 *     (keeps it alive until we replace it with the result on resume)
 * so we don't need a separate registry ref for the ctx itself. */
typedef struct req_ctx {
    CURL           *easy;
    struct curl_slist *resolve;
    struct curl_slist *req_headers;   /* owned; freed at completion */
    char           *body_copy;        /* owned copy of POST body if any */
    size_t          body_len;
    size_t          upload_pos;
    curl_off_t      upload_len; /* -1: use body_len; otherwise v1 callback length */
    /* Response accumulators. Headers come line-by-line via HEADERFUNCTION and
     * are pushed into a Lua table at completion; body is a plain buffer. */
    struct evbuffer *resp_body;
    struct evbuffer *resp_headers;    /* raw wire headers, joined at end */
    char            errbuf[CURL_ERROR_SIZE];
    /* Parked coroutine bookkeeping (fan_coro_park style). */
    lua_State      *co;
    int             co_ref;           /* pins the coroutine across the yield */
    int             done;             /* set by info-drain, checked by wake */
    int             curl_result;      /* CURLcode from info-read */
    /* M20.2: v1-era streaming callbacks. Both are optional. When
     * `onreceive_ref` is set, cb_write invokes it for each libcurl-delivered
     * chunk (already decoded — chunked-transfer framing is stripped by
     * libcurl before WRITEFUNCTION sees it). When `onheader_ref` is set,
     * cb_header invokes it exactly once at the header/body boundary with
     * a {status, responseCode, reason, headers} table.
     *
     * `buffered` (default 1) controls whether the response body is still
     * accumulated in resp_body for the final `response.body` field.
     * Callers that want memory-cap streaming for large bodies pass
     * buffered=false to skip evbuffer_add and get `response.body = ""`.
     *
     * `header_dispatched` is a one-shot latch so cb_header can call
     * onheader exactly once even though libcurl invokes it per header
     * line (including the trailing empty CRLF terminator we key off of).
     *
     * `cancel_err` is set when a callback returns false or raises; we
     * return 0 from the curl callback to make libcurl abort the transfer
     * with CURLE_WRITE_ERROR, then check_multi_info surfaces the message
     * verbatim rather than curl's generic "Failed writing received data
     * to disk/application" string. */
    int             onreceive_ref;    /* LUA_NOREF when unset */
    int             onheader_ref;     /* LUA_NOREF when unset */
    int             onprogress_ref;   /* LUA_NOREF when unset */
    int             onsend_ref;       /* LUA_NOREF when unset */
    int             oncomplete_ref;   /* LUA_NOREF when unset */
    int             suppress_return;  /* oncomplete ran for this completion */
    int             buffered;         /* 0 -> don't accumulate resp_body */
    int             empty_body_string;
    int             header_dispatched;
    char           *cancel_err;       /* strdup'd; freed at completion */
} req_ctx_t;

/* Per-socket state we hand to libevent. libcurl gives us a socket + a
 * "what" bitmask; we translate to EV_READ/EV_WRITE and (re)arm one event
 * per socket. Stored on the socket via CURLMOPT_SOCKETDATA. */
typedef struct sock_ctx {
    struct event   *ev;
    curl_socket_t   fd;
    int             action;    /* last CURL_POLL_* we armed for */
} sock_ctx_t;

/* Forward decls. */
static void   check_multi_info(void);
static void   sock_event_cb(evutil_socket_t fd, short what, void *arg);
static void   multi_timer_cb(evutil_socket_t fd, short what, void *arg);
static int    cb_socket(CURL *easy, curl_socket_t s, int what, void *up, void *sp);
static int    cb_timer(CURLM *m, long timeout_ms, void *up);
static size_t cb_write(char *ptr, size_t size, size_t nmemb, void *up);
static size_t cb_header(char *ptr, size_t size, size_t nmemb, void *up);
static int cb_progress(void *up, double dltotal, double dlnow, double ultotal, double ulnow);
static int invoke_oncomplete(lua_State *L, req_ctx_t *r);
/* M20.2 header-parse helpers (defined after check_multi_info, forward-decl'd
 * here so check_multi_info's completion tail can call them). */
static void push_reason(lua_State *L, const char *hbuf, size_t hlen);
static void push_headers_table(lua_State *L, const char *hbuf, size_t hlen);

/* ---- lazy init ---------------------------------------------------------- */
static int ensure_multi(lua_State *L) {
    if (g_multi) return 0;
    /* curl_global_init is thread-hostile: call it once, before any threads,
     * from the main state at register-time (fan_http_register does it). We
     * only need to build the multi handle + timer event here. */
    g_multi = curl_multi_init();
    if (!g_multi) return luaL_error(L, "curl_multi_init failed");
    curl_multi_setopt(g_multi, CURLMOPT_SOCKETFUNCTION, cb_socket);
    curl_multi_setopt(g_multi, CURLMOPT_TIMERFUNCTION,  cb_timer);
    struct event_base *base = fan_loop_base();
    if (!base) return luaL_error(L, "no event base for fan.http_c");
    /* Note: multi_timer_cb only fires from a curl-set timeout; we do not
     * arm it here. libcurl calls cb_timer with a positive timeout when it
     * needs a wake, or with -1 to cancel. */
    g_timer = evtimer_new(base, multi_timer_cb, NULL);
    if (!g_timer) {
        curl_multi_cleanup(g_multi); g_multi = NULL;
        return luaL_error(L, "evtimer_new failed for fan.http_c");
    }
    return 0;
}

/* ---- libcurl -> libevent socket bridge ---------------------------------- */
static void sock_ctx_free(sock_ctx_t *sc) {
    if (!sc) return;
    if (sc->ev) { event_del(sc->ev); event_free(sc->ev); sc->ev = NULL; }
    free(sc);
}

/* Called by libcurl any time it wants to add/modify/remove a poll on a
 * socket. `what` is CURL_POLL_IN / OUT / INOUT / REMOVE. */
static int cb_socket(CURL *easy, curl_socket_t s, int what, void *up, void *sp) {
    (void)easy; (void)up;
    sock_ctx_t *sc = (sock_ctx_t *)sp;

    if (what == CURL_POLL_REMOVE) {
        if (sc) {
            curl_multi_assign(g_multi, s, NULL);
            sock_ctx_free(sc);
        }
        return 0;
    }
    short kind = 0;
    if (what == CURL_POLL_IN)     kind = EV_READ;
    else if (what == CURL_POLL_OUT) kind = EV_WRITE;
    else if (what == CURL_POLL_INOUT) kind = EV_READ | EV_WRITE;
    kind |= EV_PERSIST;

    if (!sc) {
        sc = (sock_ctx_t *)calloc(1, sizeof(*sc));
        if (!sc) return 0;
        sc->fd = s;
        curl_multi_assign(g_multi, s, sc);
    } else {
        /* fd unchanged; drop the old event and rearm with the new mask. */
        if (sc->ev) { event_del(sc->ev); event_free(sc->ev); sc->ev = NULL; }
    }
    sc->action = what;
    struct event_base *base = fan_loop_base();
    sc->ev = event_new(base, s, kind, sock_event_cb, sc);
    if (sc->ev) event_add(sc->ev, NULL);
    return 0;
}

static void sock_event_cb(evutil_socket_t fd, short what, void *arg) {
    (void)arg;
    int flags = 0;
    if (what & EV_READ)  flags |= CURL_CSELECT_IN;
    if (what & EV_WRITE) flags |= CURL_CSELECT_OUT;
    int still_running = 0;
    curl_multi_socket_action(g_multi, fd, flags, &still_running);
    check_multi_info();
}

/* libcurl asks us to arm ONE timer for the whole multi handle. */
static int cb_timer(CURLM *m, long timeout_ms, void *up) {
    (void)m; (void)up;
    if (!g_timer) return 0;
    if (timeout_ms < 0) {
        evtimer_del(g_timer);
        return 0;
    }
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    evtimer_add(g_timer, &tv);
    return 0;
}

static void multi_timer_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what; (void)arg;
    int still_running = 0;
    curl_multi_socket_action(g_multi, CURL_SOCKET_TIMEOUT, 0, &still_running);
    check_multi_info();
}

/* ---- transfer completion (drain) ---------------------------------------- */
/* Pop CURLMSG_DONE messages from libcurl and resume their coroutines. Runs
 * from cb_socket / cb_timer, i.e. from the event loop thread. */
static void check_multi_info(void) {
    CURLMsg *m;
    int inqueue = 0;
    while ((m = curl_multi_info_read(g_multi, &inqueue))) {
        if (m->msg != CURLMSG_DONE) continue;
        CURL *easy = m->easy_handle;
        req_ctx_t *r = NULL;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, (char **)&r);
        if (!r) {
            /* Shouldn't happen; be defensive. */
            curl_multi_remove_handle(g_multi, easy);
            CURL_EASY_CLEANUP(easy, "multi_info_orphan");
            continue;
        }
        r->curl_result = m->data.result;
        r->done = 1;
        curl_multi_remove_handle(g_multi, easy);
        g_ncurrent--;

        /* Materialise the response only while the owning Lua state is alive.
         * During teardown, callbacks may still drain CURL messages after
         * fan_http_clear_lua_state() has cleared g_main_L; `co` then belongs
         * to a dead or closing state and must never be touched. */
        lua_State *co = r->co;
        int ref = r->co_ref;
        int lua_alive = g_main_L != NULL;
        r->co = NULL;
        r->co_ref = LUA_NOREF;
        int nrets = 0;

        if (lua_alive && r->curl_result == CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);

            lua_newtable(co);
            lua_pushinteger(co, code);
            lua_setfield(co, -2, "status");
            lua_pushinteger(co, code);
            lua_setfield(co, -2, "responseCode");

            size_t hlen = evbuffer_get_length(r->resp_headers);
            const char *hbuf = hlen ? (const char *)evbuffer_pullup(r->resp_headers, -1) : "";
            push_reason(co, hbuf, hlen);
            lua_setfield(co, -2, "reason");
            lua_getfield(co, -1, "reason");
            lua_setfield(co, -2, "responseMessage");
            push_headers_table(co, hbuf, hlen);
            lua_setfield(co, -2, "headers");

            size_t blen = evbuffer_get_length(r->resp_body);
            if (blen) {
                const char *bbuf = (const char *)evbuffer_pullup(r->resp_body, -1);
                lua_pushlstring(co, bbuf, blen);
            } else {
                /* A completed buffered response always exposes a string body;
                 * this preserves the v1 empty-body contract for 200/204/HEAD
                 * alike and avoids backend-dependent nil results. */
                lua_pushliteral(co, "");
            }
            lua_setfield(co, -2, "body");

            double timing = 0.0;
            if (curl_easy_getinfo(easy, CURLINFO_NAMELOOKUP_TIME, &timing) == CURLE_OK) {
                lua_pushnumber(co, timing); lua_setfield(co, -2, "dns_time");
            }
            if (curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME, &timing) == CURLE_OK) {
                lua_pushnumber(co, timing); lua_setfield(co, -2, "connect_time");
            }
            if (curl_easy_getinfo(easy, CURLINFO_APPCONNECT_TIME, &timing) == CURLE_OK) {
                lua_pushnumber(co, timing); lua_setfield(co, -2, "appconnect_time");
            }
            if (curl_easy_getinfo(easy, CURLINFO_PRETRANSFER_TIME, &timing) == CURLE_OK) {
                lua_pushnumber(co, timing); lua_setfield(co, -2, "pretransfer_time");
            }
            if (curl_easy_getinfo(easy, CURLINFO_STARTTRANSFER_TIME, &timing) == CURLE_OK) {
                lua_pushnumber(co, timing); lua_setfield(co, -2, "starttransfer_time");
            }
            if (curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME, &timing) == CURLE_OK) {
                lua_pushnumber(co, timing); lua_setfield(co, -2, "total_time");
            }

            struct curl_slist *cookies = NULL;
            if (curl_easy_getinfo(easy, CURLINFO_COOKIELIST, &cookies) == CURLE_OK && cookies) {
                lua_newtable(co);
                int ci = 1;
                for (struct curl_slist *item = cookies; item; item = item->next) {
                    lua_pushstring(co, item->data);
                    lua_rawseti(co, -2, ci++);
                }
                lua_setfield(co, -2, "cookies");
                curl_slist_free_all(cookies);
            }
            if (invoke_oncomplete(co, r)) {
                lua_pop(co, 1);
                r->suppress_return = 1;
                nrets = 0;
            } else {
                nrets = 1;
            }
        } else if (lua_alive) {
            const char *msg;
            if (r->cancel_err) {
                msg = r->cancel_err;
            } else if (r->errbuf[0]) {
                msg = r->errbuf;
            } else {
                msg = curl_easy_strerror(r->curl_result);
            }
            lua_newtable(co);
            lua_pushinteger(co, 0);
            lua_setfield(co, -2, "responseCode");
            lua_pushinteger(co, 0);
            lua_setfield(co, -2, "status");
            lua_newtable(co);
            lua_setfield(co, -2, "headers");
            lua_pushstring(co, msg ? msg : "curl error");
            lua_setfield(co, -2, "error");
            if (invoke_oncomplete(co, r)) {
                lua_pop(co, 1);
                r->suppress_return = 1;
                nrets = 0;
            } else {
                lua_pushstring(co, msg ? msg : "curl error");
                nrets = 2;
            }
        }

        /* Release the per-request C resources BEFORE we resume the
         * coroutine. All response data the caller needs is already on
         * `co`'s stack (status/headers/body as Lua values); r's evbuffers
         * and body_copy no longer serve any purpose. It is CRITICAL to do
         * this before fan_coro_wake because the resumed coroutine can run
         * arbitrary Lua — including collectgarbage() or simply dropping
         * the last reference to the parked ctx userdata — which invokes
         * l_req_gc on this same `r`. If r->easy is still set at that
         * moment, l_req_gc will curl_easy_cleanup it, and then the loop
         * below (line: CURL_EASY_CLEANUP(easy, ...)) frees the SAME
         * pointer again — the flaky "double free or corruption (!prev)"
         * we hunted under coverage. Freeing here, NULL-ing the fields,
         * and only THEN waking, closes that window. */
        if (r->req_headers) { curl_slist_free_all(r->req_headers); r->req_headers = NULL; }
        if (r->resolve) { curl_slist_free_all(r->resolve); r->resolve = NULL; }
        if (r->body_copy)   { free(r->body_copy); r->body_copy = NULL; }
        r->body_len = 0;
        /* Release callback refs only while the Lua registry is alive. */
        if (lua_alive && r->onreceive_ref != LUA_NOREF) luaL_unref(g_main_L, LUA_REGISTRYINDEX, r->onreceive_ref);
        r->onreceive_ref = LUA_NOREF;
        if (lua_alive && r->onheader_ref != LUA_NOREF) luaL_unref(g_main_L, LUA_REGISTRYINDEX, r->onheader_ref);
        r->onheader_ref = LUA_NOREF;
        if (lua_alive && r->onprogress_ref != LUA_NOREF) luaL_unref(g_main_L, LUA_REGISTRYINDEX, r->onprogress_ref);
        r->onprogress_ref = LUA_NOREF;
        if (lua_alive && r->onsend_ref != LUA_NOREF) luaL_unref(g_main_L, LUA_REGISTRYINDEX, r->onsend_ref);
        r->onsend_ref = LUA_NOREF;
        if (lua_alive && r->oncomplete_ref != LUA_NOREF) luaL_unref(g_main_L, LUA_REGISTRYINDEX, r->oncomplete_ref);
        r->oncomplete_ref = LUA_NOREF;
        if (r->cancel_err) { free(r->cancel_err); r->cancel_err = NULL; }
        CURL_EASY_CLEANUP(easy, "multi_info_done");
        r->easy = NULL;

        if (!lua_alive) {
            /* Teardown path: all Lua-owned state is already invalid or
             * closing. C/CURL resources are released, but no Lua API or
             * coroutine wake is allowed. */
            continue;
        }
        if (ref == LUA_NOREF) {
            /* l_request has not parked yet; values are already on its stack. */
            (void)nrets;
        } else {
            fan_coro_wake(g_main_L, co, ref, nrets);
        }
    }
}

/* ---- M20.2: header-parse helpers shared by cb_header (early dispatch) and
 * check_multi_info (final response table). Both operate on the raw wire
 * buffer `r->resp_headers` accumulated by cb_header line-by-line.
 * ------------------------------------------------------------------------- */

/* Push a Lua string with the HTTP reason phrase parsed from the status line
 * "HTTP/1.1 CODE REASON\r\n". Pushes "" on any parse failure so callers can
 * always `lua_setfield(L, -2, "reason")` unconditionally. */
static void push_reason(lua_State *L, const char *hbuf, size_t hlen) {
    if (!hlen) { lua_pushstring(L, ""); return; }
    const char *eol = memchr(hbuf, '\r', hlen);
    size_t line_len = eol ? (size_t)(eol - hbuf) : hlen;
    const char *p = memchr(hbuf, ' ', line_len);
    if (!p) { lua_pushstring(L, ""); return; }
    p++;
    const char *p2 = memchr(p, ' ', hbuf + line_len - p);
    if (!p2) { lua_pushstring(L, ""); return; }
    p2++;
    lua_pushlstring(L, p2, hbuf + line_len - p2);
}

/* Push a Lua table containing lower-cased response headers parsed from the
 * raw wire buffer. A first occurrence is a string; repeated occurrences are
 * converted to a 1-based array, exactly matching the v1 contract. The status
 * line is skipped. Terminates at the empty CRLF line. */
static void push_headers_table(lua_State *L, const char *hbuf, size_t hlen) {
    lua_newtable(L);
    const char *cur = hbuf;
    const char *end = hbuf + hlen;
    while (cur < end && *cur != '\n') cur++;
    if (cur < end) cur++;
    while (cur < end) {
        const char *eol = memchr(cur, '\n', end - cur);
        if (!eol) break;
        size_t linelen = eol - cur;
        if (linelen && cur[linelen - 1] == '\r') linelen--;
        if (linelen == 0) break;
        const char *colon = memchr(cur, ':', linelen);
        if (colon) {
            size_t keylen = colon - cur;
            const char *val = colon + 1;
            size_t vlen = linelen - keylen - 1;
            while (vlen && (*val == ' ' || *val == '\t')) { val++; vlen--; }
            char kbuf[512];
            size_t kn = keylen < sizeof(kbuf) ? keylen : sizeof(kbuf);
            for (size_t i = 0; i < kn; i++) {
                char c = cur[i];
                if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
                kbuf[i] = c;
            }
            lua_pushlstring(L, kbuf, kn);
            lua_pushvalue(L, -1);
            lua_gettable(L, -3);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_pushlstring(L, val, vlen);
            } else if (lua_isstring(L, -1)) {
                lua_newtable(L);
                lua_pushvalue(L, -2); lua_rawseti(L, -2, 1);
                lua_pushlstring(L, val, vlen); lua_rawseti(L, -2, 2);
                lua_remove(L, -2);
            } else {
                size_t n = lua_rawlen(L, -1);
                lua_pushlstring(L, val, vlen); lua_rawseti(L, -2, n + 1);
            }
            lua_settable(L, -3);
        }
        cur = eol + 1;
    }
}

/* Common tail: record a callback cancellation so check_multi_info surfaces
 * the exact message the user's callback triggered. Only the first cancel
 * wins (subsequent aborts are curl's own reaction). */
static void record_cancel(req_ctx_t *r, const char *msg) {
    if (r->cancel_err || !msg) return;
    r->cancel_err = strdup(msg);
}

static int invoke_oncomplete(lua_State *L, req_ctx_t *r) {
    if (r->oncomplete_ref == LUA_NOREF) return 0;
    if (g_main_L && g_main_L != L) {
        int response_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_rawgeti(L, LUA_REGISTRYINDEX, r->oncomplete_ref);
        lua_xmove(L, g_main_L, 1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, response_ref);
        lua_xmove(L, g_main_L, 1);
        if (lua_pcall(g_main_L, 1, 0, 0) != LUA_OK) {
            const char *msg = lua_tostring(g_main_L, -1);
            fprintf(stderr, "[luafan2] oncomplete callback error: %s\\n",
                    msg ? msg : "(non-string error)");
            lua_pop(g_main_L, 1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, response_ref);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, r->oncomplete_ref);
    lua_pushvalue(L, -2);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "[luafan2] oncomplete callback error: %s\\n",
                msg ? msg : "(non-string error)");
        lua_pop(L, 1);
    }
    return 1;
}

/* ---- easy-handle write/header callbacks --------------------------------- */
static size_t cb_write(char *ptr, size_t size, size_t nmemb, void *up) {
    req_ctx_t *r = (req_ctx_t *)up;
    size_t n = size * nmemb;
    /* Buffered mode (default): keep accumulating so response.body is a
     * complete string at completion. Streaming mode (buffered=false):
     * skip the copy so large bodies do not sit in memory. Both modes
     * still dispatch to onreceive when it is set. */
    if (r->buffered && r->resp_body) evbuffer_add(r->resp_body, ptr, n);
    if (r->onreceive_ref == LUA_NOREF) return n;
    if (r->cancel_err) return 0;  /* already aborting */

    /* Dispatch onreceive(chunk). Pcall on the main state so a raising
     * callback does not propagate through libcurl / the event loop.
     * Returning false or raising records cancel_err and returns 0 so
     * curl aborts with CURLE_WRITE_ERROR; check_multi_info then
     * surfaces cancel_err verbatim. */
    if (!g_main_L) return n;   /* teardown window: silently drop */
    lua_rawgeti(g_main_L, LUA_REGISTRYINDEX, r->onreceive_ref);
    lua_pushlstring(g_main_L, ptr, n);
    int status = lua_pcall(g_main_L, 1, 1, 0);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(g_main_L, -1);
        char buf[256];
        snprintf(buf, sizeof(buf), "onreceive callback error: %s",
                 msg ? msg : "(non-string error)");
        record_cancel(r, buf);
        lua_pop(g_main_L, 1);
        return 0;
    }
    /* Cancel iff the callback returned exactly the boolean false. */
    int cancel = (lua_type(g_main_L, -1) == LUA_TBOOLEAN && !lua_toboolean(g_main_L, -1));
    lua_pop(g_main_L, 1);
    if (cancel) {
        record_cancel(r, "onreceive callback canceled");
        return 0;
    }
    return n;
}

static int cb_progress(void *up, double dltotal, double dlnow,
                       double ultotal, double ulnow) {
    req_ctx_t *r = (req_ctx_t *)up;
    if (r->cancel_err || !g_main_L) return 1;
    lua_rawgeti(g_main_L, LUA_REGISTRYINDEX, r->onprogress_ref);
    lua_pushinteger(g_main_L, (lua_Integer)dltotal);
    lua_pushinteger(g_main_L, (lua_Integer)dlnow);
    lua_pushinteger(g_main_L, (lua_Integer)ultotal);
    lua_pushinteger(g_main_L, (lua_Integer)ulnow);
    int status = lua_pcall(g_main_L, 4, 1, 0);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(g_main_L, -1);
        char buf[256];
        snprintf(buf, sizeof(buf), "onprogress callback error: %s", msg ? msg : "(non-string error)");
        record_cancel(r, buf);
        lua_pop(g_main_L, 1);
        return 1;
    }
    int cancel = lua_isnumber(g_main_L, -1) && lua_tointeger(g_main_L, -1) != 0;
    lua_pop(g_main_L, 1);
    if (cancel) record_cancel(r, "onprogress callback canceled");
    return cancel ? 1 : 0;
}

static size_t cb_header(char *ptr, size_t size, size_t nmemb, void *up) {
    req_ctx_t *r = (req_ctx_t *)up;
    size_t n = size * nmemb;
    if (r->resp_headers) evbuffer_add(r->resp_headers, ptr, n);

    /* Detect the header/body boundary: libcurl invokes HEADERFUNCTION with
     * an "\r\n" (or bare "\n") line after the final response header of a
     * given block. On that line — and only once per request, guarded by
     * header_dispatched — dispatch onheader if configured.
     *
     * Note: intermediate 1xx responses have their own header block that
     * ends with the same empty-line marker; libcurl calls HEADERFUNCTION
     * for both. We fire onheader on the FIRST such marker only. The
     * common case (no 1xx) sees the correct final block; the 1xx case
     * would fire early with the 100-Continue table. Live with that for
     * now — v1 had the same behaviour and no caller depends on 1xx
     * inspection. */
    int is_terminator = (n == 2 && ptr[0] == '\r' && ptr[1] == '\n')
                     || (n == 1 && ptr[0] == '\n');
    if (!is_terminator) return n;
    if (r->header_dispatched) return n;
    r->header_dispatched = 1;
    if (r->onheader_ref == LUA_NOREF) return n;
    if (r->cancel_err) return 0;
    if (!g_main_L) return n;   /* teardown window: silently drop */

    /* Build {status, responseCode, reason, headers} from the raw wire
     * buffer we just finished accumulating. */
    long code = 0;
    curl_easy_getinfo(r->easy, CURLINFO_RESPONSE_CODE, &code);
    size_t hlen = evbuffer_get_length(r->resp_headers);
    const char *hbuf = hlen ? (const char *)evbuffer_pullup(r->resp_headers, -1) : "";

    lua_rawgeti(g_main_L, LUA_REGISTRYINDEX, r->onheader_ref);
    lua_newtable(g_main_L);
    lua_pushinteger(g_main_L, code);       lua_setfield(g_main_L, -2, "status");
    lua_pushinteger(g_main_L, code);       lua_setfield(g_main_L, -2, "responseCode");
    push_reason(g_main_L, hbuf, hlen);     lua_setfield(g_main_L, -2, "reason");
    lua_getfield(g_main_L, -1, "reason");
    lua_setfield(g_main_L, -2, "responseMessage");
    push_headers_table(g_main_L, hbuf, hlen); lua_setfield(g_main_L, -2, "headers");

    int status = lua_pcall(g_main_L, 1, 1, 0);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(g_main_L, -1);
        char buf[256];
        snprintf(buf, sizeof(buf), "onheader callback error: %s",
                 msg ? msg : "(non-string error)");
        record_cancel(r, buf);
        lua_pop(g_main_L, 1);
        return 0;
    }
    int cancel = (lua_type(g_main_L, -1) == LUA_TBOOLEAN && !lua_toboolean(g_main_L, -1));
    lua_pop(g_main_L, 1);
    if (cancel) {
        record_cancel(r, "onheader callback canceled");
        return 0;
    }
    return n;
}

/* ---- request userdata metatable ----------------------------------------- */
static int l_req_gc(lua_State *L) {
    req_ctx_t *r = (req_ctx_t *)luaL_checkudata(L, 1, REQ_MT);
    if (r->easy) {
        /* Never reached in practice: l_request only unwinds by returning
         * after the coroutine wake, which cleans easy up. This is a safety
         * net for e.g. a coroutine that got collected mid-flight. */
        curl_multi_remove_handle(g_multi, r->easy);
        CURL_EASY_CLEANUP(r->easy, "req_gc");
        r->easy = NULL;
    }
    if (r->req_headers) { curl_slist_free_all(r->req_headers); r->req_headers = NULL; }
    if (r->resolve) { curl_slist_free_all(r->resolve); r->resolve = NULL; }
    if (r->body_copy)   { free(r->body_copy); r->body_copy = NULL; }
    if (r->resp_body)   { evbuffer_free(r->resp_body);   r->resp_body = NULL; }
    if (r->resp_headers){ evbuffer_free(r->resp_headers);r->resp_headers = NULL; }
    /* M20.2: release callback refs if the request never made it to the
     * completion path (rare — l_req_gc mostly fires as a safety net). */
    if (r->onreceive_ref != LUA_NOREF && g_main_L) luaL_unref(g_main_L, LUA_REGISTRYINDEX, r->onreceive_ref);
    r->onreceive_ref = LUA_NOREF;
    if (r->onheader_ref != LUA_NOREF && g_main_L) luaL_unref(g_main_L, LUA_REGISTRYINDEX, r->onheader_ref);
    r->onheader_ref = LUA_NOREF;
    if (r->onprogress_ref != LUA_NOREF && g_main_L) luaL_unref(g_main_L, LUA_REGISTRYINDEX, r->onprogress_ref);
    r->onprogress_ref = LUA_NOREF;
    if (r->onsend_ref != LUA_NOREF && g_main_L) luaL_unref(g_main_L, LUA_REGISTRYINDEX, r->onsend_ref);
    r->onsend_ref = LUA_NOREF;
    if (r->oncomplete_ref != LUA_NOREF && g_main_L) luaL_unref(g_main_L, LUA_REGISTRYINDEX, r->oncomplete_ref);
    r->oncomplete_ref = LUA_NOREF;
    if (r->cancel_err) { free(r->cancel_err); r->cancel_err = NULL; }
    return 0;
}

static void ensure_req_mt(lua_State *L) {
    if (luaL_newmetatable(L, REQ_MT)) {
        lua_pushcfunction(L, l_req_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

/* ---- fan.http_c.request{...} -------------------------------------------- */
/* We accept the same opts table as the Lua backend's do_once (see
 * lua/fan/http.lua). Redirects are handled in the Lua shim, so we do NOT
 * enable CURLOPT_FOLLOWLOCATION here. */
static int l_request(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ensure_multi(L);
    ensure_req_mt(L);

    /* Must run in a coroutine (we'll yield). Same guard fan.sleep uses. */
    int is_main = lua_pushthread(L);
    lua_pop(L, 1);
    if (is_main) {
        return luaL_error(L, "fan.http_c.request must be called from a coroutine");
    }

    /* Read fields. */
    lua_getfield(L, 1, "url");
    const char *url = luaL_checkstring(L, -1);
    lua_pop(L, 1);


    lua_getfield(L, 1, "method");
    const char *method = luaL_optstring(L, -1, "GET");
    lua_pop(L, 1);
    /* Uppercase into a stack buffer. libcurl accepts any custom method
     * via CUSTOMREQUEST, but we still normalise here so error messages
     * and CURLOPT_NOBODY logic work. */
    char meth[32] = {0};
    for (int i = 0; method[i] && i < (int)sizeof(meth) - 1; i++) {
        char c = method[i];
        if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
        meth[i] = c;
    }

    lua_getfield(L, 1, "body");
    size_t body_len = 0;
    const char *body = (lua_type(L, -1) == LUA_TSTRING) ? lua_tolstring(L, -1, &body_len) : NULL;
    /* pop *after* copy below */

    /* Allocate the request ctx (as userdata for the GC guarantees). */
    req_ctx_t *r = (req_ctx_t *)lua_newuserdata(L, sizeof(*r));
    memset(r, 0, sizeof(*r));
    r->co_ref = LUA_NOREF;
    /* M20.2: default callback refs / streaming flags. */
    r->onreceive_ref = LUA_NOREF;
    r->onheader_ref  = LUA_NOREF;
    r->onprogress_ref = LUA_NOREF;
    r->onsend_ref = LUA_NOREF;
    r->oncomplete_ref = LUA_NOREF;
    r->upload_len    = -1;
    r->buffered      = 1;             /* accumulate response.body by default */
    r->resp_body    = evbuffer_new();
    r->resp_headers = evbuffer_new();
    if (!r->resp_body || !r->resp_headers) {
        return luaL_error(L, "evbuffer_new failed");
    }
    luaL_getmetatable(L, REQ_MT);
    lua_setmetatable(L, -2);
    /* stack: [opts, body_str_or_nil, ctx_ud] */

    if (body && body_len > 0) {
        r->body_copy = (char *)malloc(body_len);
        if (!r->body_copy) return luaL_error(L, "oom body copy");
        memcpy(r->body_copy, body, body_len);
        r->body_len = body_len;
    }
    lua_remove(L, -2);  /* drop body_str */

    /* Build easy handle. */
    r->easy = curl_easy_init();
    if (!r->easy) return luaL_error(L, "curl_easy_init failed");
    CURL_EASY_INIT_TRACKED(r->easy);
    curl_easy_setopt(r->easy, CURLOPT_URL,             url);
    curl_easy_setopt(r->easy, CURLOPT_WRITEFUNCTION,   cb_write);
    curl_easy_setopt(r->easy, CURLOPT_WRITEDATA,       r);
    curl_easy_setopt(r->easy, CURLOPT_HEADERFUNCTION,  cb_header);
    curl_easy_setopt(r->easy, CURLOPT_HEADERDATA,      r);
    curl_easy_setopt(r->easy, CURLOPT_PRIVATE,         r);
    curl_easy_setopt(r->easy, CURLOPT_ERRORBUFFER,     r->errbuf);
    curl_easy_setopt(r->easy, CURLOPT_NOSIGNAL,        1L);
    curl_easy_setopt(r->easy, CURLOPT_FOLLOWLOCATION,  0L);
    /* Method mapping. */
    if      (strcmp(meth, "GET")     == 0) { /* default */ }
    else if (strcmp(meth, "HEAD")    == 0) { curl_easy_setopt(r->easy, CURLOPT_NOBODY, 1L); }
    else if (strcmp(meth, "POST")    == 0) {
        curl_easy_setopt(r->easy, CURLOPT_POST, 1L);
        curl_easy_setopt(r->easy, CURLOPT_POSTFIELDSIZE, (long)r->body_len);
        curl_easy_setopt(r->easy, CURLOPT_POSTFIELDS, r->body_copy ? r->body_copy : "");
    }
    else {
        /* PUT/DELETE/PATCH/OPTIONS/UPDATE etc. — send via CUSTOMREQUEST
         * and, if a body is present, feed it through POSTFIELDS which
         * libcurl also honours for arbitrary methods. */
        curl_easy_setopt(r->easy, CURLOPT_CUSTOMREQUEST, meth);
        if (r->body_len > 0) {
            curl_easy_setopt(r->easy, CURLOPT_POSTFIELDSIZE, (long)r->body_len);
            curl_easy_setopt(r->easy, CURLOPT_POSTFIELDS, r->body_copy);
        }
    }

    /* Headers. */
    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING) {
                const char *k = lua_tostring(L, -2);
                const char *v = lua_tostring(L, -1);
                size_t n = strlen(k) + 2 + strlen(v) + 1;
                char *line = (char *)malloc(n);
                if (line) {
                    snprintf(line, n, "%s: %s", k, v);
                    r->req_headers = curl_slist_append(r->req_headers, line);
                    free(line);
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    /* Force Connection: close so the connection is not held in libcurl's
     * pool across requests — matches the Lua backend's "one request per
     * connection" isolation contract. */
    r->req_headers = curl_slist_append(r->req_headers, "Connection: close");
    if (r->req_headers) {
        curl_easy_setopt(r->easy, CURLOPT_HTTPHEADER, r->req_headers);
    }

    /* v1 timeout semantics: timeout is low-speed timeout; conntimeout is
     * the overall/connect timeout in seconds. Keep v2's timeout as a
     * compatible fallback when conntimeout is absent. */
    lua_getfield(L, 1, "timeout");
    if (lua_isnumber(L, -1)) {
        long sec = (long)lua_tonumber(L, -1);
        if (sec > 0) {
            curl_easy_setopt(r->easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(r->easy, CURLOPT_LOW_SPEED_TIME, sec);
            curl_easy_setopt(r->easy, CURLOPT_TIMEOUT, sec);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "conntimeout");
    if (lua_isnumber(L, -1)) {
        long sec = (long)lua_tonumber(L, -1);
        if (sec > 0) curl_easy_setopt(r->easy, CURLOPT_CONNECTTIMEOUT, sec);
    } else {
        lua_getfield(L, 1, "timeout");
        if (lua_isnumber(L, -1) && lua_tonumber(L, -1) > 0)
            curl_easy_setopt(r->easy, CURLOPT_CONNECTTIMEOUT, (long)lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "forbid_reuse");
    if (lua_isnumber(L, -1)) curl_easy_setopt(r->easy, CURLOPT_FORBID_REUSE, lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "dns_servers");
    if (lua_isstring(L, -1)) curl_easy_setopt(r->easy, CURLOPT_DNS_SERVERS, lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "verbose");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) curl_easy_setopt(r->easy, CURLOPT_VERBOSE, 1L);
    lua_pop(L, 1);

    /* TLS verify (default true). Accept a single opts.verify, or the
     * finer-grained opts.verify_peer / opts.verify_host that the v1
     * setopt(SSL_VERIFYPEER/HOST) split exposed. */
    int verify_peer = 1, verify_host = 1;
    lua_getfield(L, 1, "ssl_verifypeer");
    if (lua_isnumber(L, -1)) verify_peer = lua_tointeger(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 1, "ssl_verifyhost");
    if (lua_isnumber(L, -1)) verify_host = lua_tointeger(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 1, "verify");
    if (lua_isboolean(L, -1)) { int v = lua_toboolean(L, -1); verify_peer = v; verify_host = v; }
    lua_pop(L, 1);
    lua_getfield(L, 1, "verify_peer");
    if (lua_isboolean(L, -1)) verify_peer = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "verify_host");
    if (lua_isboolean(L, -1)) verify_host = lua_toboolean(L, -1);
    lua_pop(L, 1);
    curl_easy_setopt(r->easy, CURLOPT_SSL_VERIFYPEER, (long)(verify_peer ? 1 : 0));
    /* SSL_VERIFYHOST is 2 (strict) when enabled, 0 when off. */
    curl_easy_setopt(r->easy, CURLOPT_SSL_VERIFYHOST, (long)(verify_host ? 2 : 0));

    const char *cert_fields[] = {"sslcert", "sslcertpasswd", "sslcerttype", "sslkey", "sslkeypasswd", "sslkeytype"};
    CURLoption cert_opts[] = {CURLOPT_SSLCERT, CURLOPT_SSLCERTPASSWD, CURLOPT_SSLCERTTYPE, CURLOPT_SSLKEY, CURLOPT_SSLKEYPASSWD, CURLOPT_SSLKEYTYPE};
    for (int ci = 0; ci < 6; ci++) {
        lua_getfield(L, 1, cert_fields[ci]);
        if (lua_isstring(L, -1)) curl_easy_setopt(r->easy, cert_opts[ci], lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_getfield(L, 1, "proxy");
    if (lua_isstring(L, -1)) curl_easy_setopt(r->easy, CURLOPT_PROXY, lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "proxyport");
    if (lua_isnumber(L, -1)) curl_easy_setopt(r->easy, CURLOPT_PROXYPORT, lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "proxyuser");
    if (lua_isstring(L, -1)) curl_easy_setopt(r->easy, CURLOPT_PROXYUSERNAME, lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "proxypassword");
    if (lua_isstring(L, -1)) curl_easy_setopt(r->easy, CURLOPT_PROXYPASSWORD, lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "proxytunnel");
    if (lua_isnumber(L, -1)) curl_easy_setopt(r->easy, CURLOPT_HTTPPROXYTUNNEL, lua_tointeger(L, -1));
    lua_pop(L, 1);

    /* Optional global settings (cookiejar / cainfo / capath) live on the
     * Lua-side module table (fan.http._cookiejar etc.); the shim passes
     * them through as fields on `opts` for us to honour. */
    lua_getfield(L, 1, "cookiejar");
    if (lua_isstring(L, -1)) {
        const char *cj = lua_tostring(L, -1);
        curl_easy_setopt(r->easy, CURLOPT_COOKIEFILE, cj);
        curl_easy_setopt(r->easy, CURLOPT_COOKIEJAR,  cj);
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "cainfo");
    if (lua_isstring(L, -1)) curl_easy_setopt(r->easy, CURLOPT_CAINFO, lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "capath");
    if (lua_isstring(L, -1)) curl_easy_setopt(r->easy, CURLOPT_CAPATH, lua_tostring(L, -1));
    lua_pop(L, 1);

    /* ---- M20.2: streaming callbacks + buffered flag --------------------
     * onreceive(chunk) — fires from cb_write for each libcurl-delivered
     *   body segment.
     * onheader(info)  — fires once from cb_header at the header/body
     *   boundary with {status, responseCode, reason, headers}.
     * buffered (default true) — false skips response.body accumulation
     *   for memory-cap streaming of large downloads.
     *
     * Refs are unref'd in check_multi_info's completion tail (or
     * l_req_gc if the request never completed). Non-function values
     * are rejected loudly rather than silently ignored. */
    lua_getfield(L, 1, "onreceive");
    if (lua_isfunction(L, -1)) {
        r->onreceive_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
        CURL_EASY_CLEANUP(r->easy, "l_request_bad_onreceive"); r->easy = NULL;
        return luaL_error(L, "onreceive must be a function");
    }
    lua_getfield(L, 1, "onheader");
    if (lua_isfunction(L, -1)) {
        r->onheader_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
        /* If we already luaL_ref'd onreceive, unref it before erroring. */
        if (r->onreceive_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, r->onreceive_ref);
            r->onreceive_ref = LUA_NOREF;
        }
        CURL_EASY_CLEANUP(r->easy, "l_request_bad_onheader"); r->easy = NULL;
        return luaL_error(L, "onheader must be a function");
    }
    lua_getfield(L, 1, "buffered");
    if (lua_isboolean(L, -1)) r->buffered = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (!r->buffered) r->empty_body_string = 1;
    if (strcmp(meth, "HEAD") == 0) r->empty_body_string = 1;

    lua_getfield(L, 1, "onprogress");
    if (lua_isfunction(L, -1)) {
        r->onprogress_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        curl_easy_setopt(r->easy, CURLOPT_XFERINFOFUNCTION, cb_progress);
        curl_easy_setopt(r->easy, CURLOPT_XFERINFODATA, r);
        curl_easy_setopt(r->easy, CURLOPT_PROGRESSFUNCTION, cb_progress);
        curl_easy_setopt(r->easy, CURLOPT_PROGRESSDATA, r);
        curl_easy_setopt(r->easy, CURLOPT_NOPROGRESS, 0L);
    } else if (lua_isnil(L, -1)) lua_pop(L, 1);
    else { lua_pop(L, 1); return luaL_error(L, "onprogress must be a function"); }

    lua_getfield(L, 1, "onsend");
    if (lua_isfunction(L, -1)) {
        r->onsend_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        size_t cap = 0;
        for (;;) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, r->onsend_ref);
            lua_pushinteger(L, 16384);
            if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
                const char *msg = lua_tostring(L, -1);
                return luaL_error(L, "onsend callback error: %s", msg ? msg : "unknown");
            }
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            size_t part_len = 0;
            const char *part = lua_tolstring(L, -1, &part_len);
            if (!part) { lua_pop(L, 1); return luaL_error(L, "onsend must return string or nil"); }
            char *next = (char *)realloc(r->body_copy, r->body_len + part_len + 1);
            if (!next) { lua_pop(L, 1); return luaL_error(L, "onsend body out of memory"); }
            r->body_copy = next;
            memcpy(r->body_copy + r->body_len, part, part_len);
            r->body_len += part_len;
            r->body_copy[r->body_len] = '\0';
            lua_pop(L, 1);
            cap += part_len;
            if (part_len == 0 || part_len < 16384) break;
            if (cap > 64 * 1024 * 1024) return luaL_error(L, "onsend body exceeds 64MiB");
        }
        curl_easy_setopt(r->easy, CURLOPT_POSTFIELDS, r->body_copy ? r->body_copy : "");
        curl_easy_setopt(r->easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)r->body_len);
        curl_easy_setopt(r->easy, CURLOPT_CUSTOMREQUEST, meth);
    } else if (lua_isnil(L, -1)) lua_pop(L, 1);
    else { lua_pop(L, 1); return luaL_error(L, "onsend must be a function"); }

    lua_getfield(L, 1, "onbodylength");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            return luaL_error(L, "onbodylength callback error: %s", msg ? msg : "unknown");
        }
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(L, "onbodylength must return a number");
        }
        r->upload_len = (curl_off_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    } else if (lua_isnil(L, -1)) lua_pop(L, 1);
    else { lua_pop(L, 1); return luaL_error(L, "onbodylength must be a function"); }
    if (r->upload_len >= 0) {
        curl_easy_setopt(r->easy, CURLOPT_POSTFIELDSIZE_LARGE, r->upload_len);
    }

    lua_getfield(L, 1, "oncomplete");
    if (lua_isfunction(L, -1)) r->oncomplete_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else if (lua_isnil(L, -1)) lua_pop(L, 1);
    else { lua_pop(L, 1); return luaL_error(L, "oncomplete must be a function"); }

    lua_getfield(L, 1, "resolve");
    if (lua_isstring(L, -1)) {
        r->resolve = curl_slist_append(NULL, lua_tostring(L, -1));
        curl_easy_setopt(r->easy, CURLOPT_RESOLVE, r->resolve);
    }
    lua_pop(L, 1);

    /* Attach easy to the multi. We DON'T park the coroutine yet: if the
     * transfer completes synchronously inside curl_multi_socket_action
     * below (e.g. immediate connect refused), check_multi_info() would
     * try to fan_coro_resume() the very thread we're still running on
     * and blow up with "cannot resume non-suspended coroutine".
     * Instead: set r->co so check_multi_info knows where to push return
     * values, leave r->co_ref = LUA_NOREF as a sentinel meaning "still
     * in l_request, don't resume", drain, and resume-or-yield below. */
    r->co = L;
    r->co_ref = LUA_NOREF;

    CURLMcode mc = curl_multi_add_handle(g_multi, r->easy);
    if (mc != CURLM_OK) {
        r->co = NULL;
        CURL_EASY_CLEANUP(r->easy, "l_request_add_handle_fail"); r->easy = NULL;
        lua_pushnil(L);
        lua_pushfstring(L, "curl_multi_add_handle: %s", curl_multi_strerror(mc));
        return 2;
    }
    g_ncurrent++;
    /* Kick libcurl once so it schedules its first socket_action. */
    int still = 0;
    curl_multi_socket_action(g_multi, CURL_SOCKET_TIMEOUT, 0, &still);
    /* We might already be done if the URL was invalid enough (or the
     * peer refused synchronously). Drain now. check_multi_info sees
     * co_ref == LUA_NOREF and just pushes values without resuming. */
    check_multi_info();
    if (r->done) {
        /* Values already on our stack; return the exact callback-aware count. */
        if (r->suppress_return) return 0;
        return r->curl_result == CURLE_OK ? 1 : 2;
    }
    /* Not done yet: park + yield. The wake path will push return values
     * onto our coroutine's stack and lua_resume us out of the yield. */
    r->co_ref = fan_coro_park(L);
    return lua_yield(L, 0);
}

/* ---- escape / unescape via libcurl (v1 byte-for-byte parity) ------------ */
static int l_escape(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING) { lua_pushnil(L); return 1; }
    size_t n; const char *s = lua_tolstring(L, 1, &n);
    /* We need one easy handle for curl_easy_escape; keep a thread-local
     * lazy one on the module's upvalue-free static. */
    static CURL *e = NULL;
    if (!e) e = curl_easy_init();
    if (!e) { lua_pushnil(L); return 1; }
    char *out = curl_easy_escape(e, s, (int)n);
    if (!out) { lua_pushnil(L); return 1; }
    lua_pushstring(L, out);
    curl_free(out);
    return 1;
}

static int l_unescape(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING) { lua_pushnil(L); return 1; }
    size_t n; const char *s = lua_tolstring(L, 1, &n);
    static CURL *e = NULL;
    if (!e) e = curl_easy_init();
    if (!e) { lua_pushnil(L); return 1; }
    int out_len = 0;
    char *out = curl_easy_unescape(e, s, (int)n, &out_len);
    if (!out) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, out, out_len);
    curl_free(out);
    return 1;
}

/* ---- register ----------------------------------------------------------- */
static const luaL_Reg http_c_funcs[] = {
    {"request",   l_request},
    {"escape",    l_escape},
    {"unescape",  l_unescape},
    {NULL, NULL},
};

void fan_http_register(lua_State *L) {
    g_main_L = fan_coro_main(L);  /* stable main thread, not the require() coroutine */
    /* One-time global init. Idempotent; curl handles refcounting. */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* module table 'fan' is at -1 on entry per luafan.c convention. */
    lua_newtable(L);
#if LUA_VERSION_NUM == 501
    luaL_register(L, NULL, http_c_funcs);
#else
    luaL_setfuncs(L, http_c_funcs, 0);
#endif
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "available");
    lua_setfield(L, -2, "http_c");
}

void fan_http_clear_lua_state(void) {
    /* Companion to fan_http_register — clear the cached main-thread pointer
     * before lua_close so late libcurl multi socket callbacks that resume
     * parked coroutines via fan_coro_wake see NULL and drop the wake. See
     * runtime/coro.h for the runtime-wide teardown contract. */
    g_main_L = NULL;
}

#else   /* !FAN_WITH_CURL */

void fan_http_register(lua_State *L) {
    /* Stub table: the Lua shim will inspect `available` and fall back to
     * the pure-Lua backend. */
    lua_newtable(L);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "available");
    lua_setfield(L, -2, "http_c");
}

void fan_http_clear_lua_state(void) {
    /* No-op stub: the FAN_WITH_CURL=0 build has no cached state to clear.
     * Provided so the aggregate teardown in luafan.c compiles the same
     * regardless of the curl feature switch. */
}

#endif  /* FAN_WITH_CURL */
