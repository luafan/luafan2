/*
 * websocket.c — LuaFan v2 WebSocket C data plane (D1 slice).
 *
 * See websocket.h for the ownership model and roadmap. This file
 * currently ships only the primitives:
 *   - fan_ws_parse_frame    RFC 6455 frame decoder from an evbuffer
 *   - fan_ws_encode_frame   RFC 6455 frame encoder into an evbuffer
 *   - fan_ws_frame_dispose  free the malloc'd payload
 *   - fan_ws_compute_accept RFC 6455 handshake digest (SHA1+base64)
 *
 * Later D-stages register the fan_ws_conn_t userdata and its
 * receive/send/close plumbing driven by the bufferevent it holds.
 *
 * Design notes:
 *   Payload buffer strategy: parse_frame malloc's a fresh buffer for
 *   the payload rather than referencing evbuffer's internals — this
 *   lets us drain the frame from the input buffer immediately (so the
 *   readcb loop can advance past it) and hand the caller a stable
 *   pointer they can push as a Lua string. The 4-byte mask is applied
 *   in-place inside that malloc'd buffer using a word-aligned loop
 *   when possible (Lua-side apply_mask was the top hot spot in v2's
 *   pure-Lua design; the aligned XOR here is where the perf recovery
 *   actually lands).
 */
#include "websocket.h"
#include "zlib_wrap.h"
#include "../runtime/coro.h"
#include "../runtime/loop.h"

#include <stdlib.h>
#include <string.h>
#include <lauxlib.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/http.h>

#ifdef FAN_WITH_OPENSSL
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#endif

#ifndef FAN_UNLIKELY
#define FAN_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/* ---- utilities ----------------------------------------------------------- */

/* Apply a 4-byte mask key to `buf` in place. `off` is the caller's
 * running frame offset (allows unmasking a payload that was assembled
 * from multiple chunks — currently unused because we always malloc a
 * contiguous payload, but kept for symmetry and future streaming). */
static void ws_apply_mask(char *buf, size_t len, const unsigned char key[4],
                          size_t off) {
    if (len == 0) return;
    size_t i = 0;
    /* Head: align to 4-byte boundary relative to the frame origin so
     * the aligned word loop indexes the correct mask byte. */
    while (i < len && ((off + i) & 3u) != 0) {
        buf[i] = (char)((unsigned char)buf[i] ^ key[(off + i) & 3u]);
        i++;
    }
    if (i < len) {
        /* Build the 32-bit key rotated to match (off+i) alignment. Now
         * (off+i) is 4-aligned so k0 is key[0], k1=key[1], etc. */
        uint32_t k = ((uint32_t)key[0]) |
                     ((uint32_t)key[1] << 8) |
                     ((uint32_t)key[2] << 16) |
                     ((uint32_t)key[3] << 24);
        while (i + 4 <= len) {
            uint32_t w;
            memcpy(&w, buf + i, 4);
            w ^= k;
            memcpy(buf + i, &w, 4);
            i += 4;
        }
        while (i < len) {
            buf[i] = (char)((unsigned char)buf[i] ^ key[(off + i) & 3u]);
            i++;
        }
    }
}

/* ---- frame decoder ------------------------------------------------------- */

void fan_ws_frame_dispose(fan_ws_frame_t *f) {
    if (!f) return;
    free(f->payload);
    f->payload = NULL;
    f->plen = 0;
}

int fan_ws_parse_frame(struct evbuffer *in, fan_ws_frame_t *out,
                       int require_mask, const char **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof(*out));

    size_t avail = evbuffer_get_length(in);
    if (avail < 2) return 0;

    /* Peek the first 14 bytes worst-case (2 base + 8 extlen + 4 mask). */
    unsigned char hdr[14];
    size_t peek_want = avail < sizeof(hdr) ? avail : sizeof(hdr);
    ev_ssize_t peeked = evbuffer_copyout(in, hdr, peek_want);
    if (peeked < 2) return 0;

    unsigned char b1 = hdr[0];
    unsigned char b2 = hdr[1];
    int fin       = (b1 & 0x80) != 0;
    int rsv1      = (b1 & 0x40) != 0;
    int rsv23     = b1 & 0x30;
    int opcode    = b1 & 0x0f;
    int masked    = (b2 & 0x80) != 0;
    uint64_t plen = (uint64_t)(b2 & 0x7f);

    if (rsv23 != 0) {
        if (err) *err = "protocol error: RSV2/RSV3 set";
        return -1;
    }
    if (require_mask && !masked) {
        if (err) *err = "protocol error: unmasked client frame";
        return -1;
    }

    size_t hdrlen = 2;
    if (plen == 126) {
        if ((size_t)peeked < 4) return 0;
        plen = ((uint64_t)hdr[2] << 8) | hdr[3];
        hdrlen = 4;
    } else if (plen == 127) {
        if ((size_t)peeked < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) {
            plen = (plen << 8) | hdr[2 + i];
        }
        hdrlen = 10;
        /* RFC 6455 §5.2: the most significant bit MUST be 0. */
        if (plen >> 63) {
            if (err) *err = "protocol error: 64-bit length msb set";
            return -1;
        }
    }

    /* Control-frame rules. */
    int is_control = opcode >= 0x8;
    if (is_control) {
        if (!fin) {
            if (err) *err = "protocol error: fragmented control frame";
            return -1;
        }
        if (plen > 125) {
            if (err) *err = "protocol error: oversized control frame";
            return -1;
        }
        if (rsv1) {
            if (err) *err = "protocol error: RSV1 set on control frame";
            return -1;
        }
    }

    unsigned char mask_key[4] = {0};
    if (masked) {
        if ((size_t)peeked < hdrlen + 4) return 0;
        memcpy(mask_key, hdr + hdrlen, 4);
        hdrlen += 4;
    }

    /* Hard cap on payload size — protects against a hostile 2^63 declared
     * length. 64 MiB matches the highest single-frame size we ever need
     * for tests + is generous vs a real WS message (which should be
     * fragmented at that point anyway). */
    if (plen > (uint64_t)(64 * 1024 * 1024)) {
        if (err) *err = "frame too large";
        return -1;
    }

    if (avail < hdrlen + (size_t)plen) return 0;

    /* We have a full frame. Drain the header first, then the payload. */
    evbuffer_drain(in, hdrlen);

    char *payload = NULL;
    if (plen > 0) {
        payload = (char *)malloc((size_t)plen);
        if (FAN_UNLIKELY(!payload)) {
            if (err) *err = "oom";
            return -1;
        }
        if (evbuffer_remove(in, payload, (size_t)plen) != (ev_ssize_t)plen) {
            free(payload);
            if (err) *err = "evbuffer_remove short read";
            return -1;
        }
        if (masked) {
            ws_apply_mask(payload, (size_t)plen, mask_key, 0);
        }
    }

    out->fin     = fin;
    out->rsv1    = rsv1;
    out->opcode  = opcode;
    out->payload = payload;
    out->plen    = (size_t)plen;
    return 1;
}

/* ---- frame encoder ------------------------------------------------------- */

int fan_ws_encode_frame(struct evbuffer *out,
                        int fin, int opcode, int rsv1,
                        const char *payload, size_t plen) {
    unsigned char hdr[10];
    size_t hlen;
    hdr[0] = (unsigned char)((fin ? 0x80 : 0) |
                             (rsv1 ? 0x40 : 0) |
                             (opcode & 0x0f));
    if (plen < 126) {
        hdr[1] = (unsigned char)plen;
        hlen = 2;
    } else if (plen < 65536) {
        hdr[1] = 126;
        hdr[2] = (unsigned char)((plen >> 8) & 0xff);
        hdr[3] = (unsigned char)(plen & 0xff);
        hlen = 4;
    } else {
        hdr[1] = 127;
        /* 64-bit big-endian */
        for (int i = 0; i < 8; i++) {
            hdr[2 + i] = (unsigned char)((plen >> (56 - 8 * i)) & 0xff);
        }
        hlen = 10;
    }
    if (evbuffer_add(out, hdr, hlen) != 0) return -1;
    if (plen > 0 && evbuffer_add(out, payload, plen) != 0) return -1;
    return 0;
}

/* ---- handshake digest ---------------------------------------------------- */

int fan_ws_compute_accept(const char *key, char *out, size_t outsz) {
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
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    b64 = BIO_push(b64, mem);
    if (BIO_write(b64, sha, SHA_DIGEST_LENGTH) != SHA_DIGEST_LENGTH) {
        BIO_free_all(b64);
        return -1;
    }
    (void)BIO_flush(b64);
    BUF_MEM *bptr = NULL;
    BIO_get_mem_ptr(b64, &bptr);
    if (!bptr || bptr->length >= outsz) {
        BIO_free_all(b64);
        return -1;
    }
    memcpy(out, bptr->data, bptr->length);
    out[bptr->length] = '\0';
    BIO_free_all(b64);
    return 0;
#else
    (void)key;
    (void)out;
    (void)outsz;
    return -1;
#endif
}

/* ============================================================
 * D2 — ws_conn userdata + read loop + send API
 * ============================================================
 *
 * The userdata owns the evhttp_connection + evhttp_request (both were
 * detached from the evhttp server's book-keeping at accept time), and
 * installs its own bufferevent callbacks so incoming WebSocket frames
 * drive a per-conn read state machine and outgoing frames go straight
 * into the bev output buffer. No fan.tcp adopt path, no cross-module
 * lifecycle bookkeeping.
 *
 * Life states
 *   flags & WS_ST_EVCON_LIVE  1 while evcon/ev_req/bev are safe to touch.
 *                             Cleared by evcon_closecb (server:close) OR
 *                             by our own :close (explicit or __gc).
 *   flags & WS_ST_CLOSE_SENT  1 once we wrote a CLOSE frame to the peer.
 *   flags & WS_ST_EOF         1 once we've seen the peer's CLOSE frame OR
 *                             an EOF/error on the bev (peer went away).
 *
 * Coroutine parking
 *   :recv parks when no complete message is queued; the readcb wakes it
 *   with (msg, "text"|"binary") or (nil, err). Ping/Pong control frames
 *   are answered inline in the readcb (they never wake recv). CLOSE
 *   frames trigger an echo close + wake with (nil, "closed").
 */
#define FAN_WS_CONN_MT "fan.ws.conn"

/* Shared control block bridging fan_ws_conn_t and a pending deferred
 * teardown timer. Both sides hold a refcount (2 at schedule time). Either
 * side may "cancel" the teardown by nulling out evcon/ev_req when it knows
 * they've already been released (evcon closecb — evhttp_free is freeing
 * them for us). The last releaser frees the control block itself.
 *
 * `owner` is a weak back-pointer to the fan_ws_conn_t; it is nulled out
 * by __gc so that a late closecb (or any callback that also runs against
 * this ctl) can tell whether the userdata is still live. The ctl outlives
 * the userdata whenever a teardown is pending at GC time.
 *
 * This design avoids two failure modes that plagued the earlier
 * `event_base_once(evcon)` design:
 *   1. server:close() -> evhttp_free -> evhttp_connection_free frees the
 *      evcon; our stale pending timer would then double-free it (UAF).
 *   2. Lua GC of fan_ws_conn_t before the timer fires would either force
 *      a synchronous free (defeats the deferred pattern) or leave a
 *      dangling back-pointer.
 * With the ref-counted ctl block the timer always fires safely: if
 * cancelled, it becomes a no-op teardown that just releases its refcount.
 */
typedef struct ws_teardown_ctl_s {
    int refcnt;                        /* 2 while both sides live */
    int cancelled;                     /* 1: evcon already gone via closecb */
    int retry;                         /* outbuf-drain retry count (0..DRAIN_MAX) */
    struct evhttp_connection *evcon;   /* NULL when cancelled or fired */
    struct evhttp_request    *ev_req;
    struct bufferevent       *bev;     /* borrowed; used for outbuf drain check.
                                        * NULL when cancelled (libevent freed it). */
    struct fan_ws_conn_s     *owner;   /* weak: nulled by __gc */
} ws_teardown_ctl_t;

/* Max re-arm attempts for the outbuf-drain loop. 100 * 1ms = 100 ms upper
 * bound before we free evcon regardless of pending output. Justification:
 *   - Normal case: outbuf drains in 1-2 ticks (one EV_WRITE dispatch).
 *   - Slow / lossy peer: TCP might be flow-controlled; capping at 100 ms
 *     prevents a permanently stalled peer from leaking evcon forever.
 *   - Client already closed: EPIPE/ECONNRESET fires bev's eventcb which
 *     is silenced at teardown time; the writecb attempt returns 0 and
 *     the outbuf never drains — the cap ensures we still make progress. */
#define WS_TEARDOWN_DRAIN_MAX 100

typedef struct fan_ws_conn_s {
    struct evhttp_connection *evcon;
    struct evhttp_request    *ev_req;
    struct bufferevent       *bev;

    unsigned int flags;
#define WS_ST_EVCON_LIVE  (1u << 0)
#define WS_ST_CLOSE_SENT  (1u << 1)
#define WS_ST_EOF         (1u << 2)
#define WS_ST_DEFLATE     (1u << 3)
#define WS_ST_SEND_BUSY   (1u << 4)  /* reserved: single-threaded model
                                       * makes this a debug guard only */

    /* Parked coroutine slot: at most one recv() is parked at a time.
     * `co_ref` is the registry ref pinning `co` across resume. */
    lua_State *co;
    int        co_ref;

    /* Fragment reassembly (across CONT frames).
     * `msg_op` is 0 when idle, otherwise FAN_WS_OP_TEXT or FAN_WS_OP_BIN.
     * `msg_deflated` is 1 when the initial data frame had RSV1 set. */
    char  *msg_buf;
    size_t msg_len;
    size_t msg_cap;
    int    msg_op;
    int    msg_deflated;

    /* Completed messages queue (rare: only when readcb delivers before
     * any recv() runs). Single slot suffices — fan.websocket is one-recv-
     * at-a-time; the queue absorbs a burst that arrived while user code
     * was still processing the previous message. Grow to a small ring if
     * this ever becomes contended. */
    char        *ready_body;
    size_t       ready_len;
    const char  *ready_op;    /* "text" | "binary" | NULL */

    /* Sticky error for the next recv() (set by eventcb / protocol err). */
    const char *err;

    /* Shared control block for a pending deferred teardown (NULL when
     * none is armed). See ws_teardown_ctl_t docs above. */
    ws_teardown_ctl_t *teardown;
} fan_ws_conn_t;

static lua_State *g_ws_main_L = NULL;

/* ---- forwards ----------------------------------------------------------- */
static void ws_readcb(struct bufferevent *bev, void *arg);
static void ws_eventcb(struct bufferevent *bev, short what, void *arg);
static void ws_evcon_closecb(struct evhttp_connection *evcon, void *arg);
static void ws_teardown_cancel_closecb(struct evhttp_connection *evcon, void *arg);
static void ws_deferred_teardown_cb(evutil_socket_t fd, short what, void *arg);
static void ws_schedule_request_only_free(struct evhttp_request *req);

/* Release one reference on the shared teardown control block. When the
 * refcount drops to 0 the block itself is freed. */
static void ws_teardown_ctl_release(ws_teardown_ctl_t *ctl) {
    if (!ctl) return;
    if (--ctl->refcnt > 0) return;
    free(ctl);
}

/* Timer fires on the next event-loop tick (or after a 1ms re-arm if the
 * bev outbuf is not yet drained — see the drain loop below). Perform the
 * actual teardown, unless closecb has already cancelled us (evhttp_free
 * path). Order matters because the request is EVHTTP_USER_OWNED (via
 * evhttp_request_own at accept time):
 *   1. evhttp_connection_free — libevent's internal loop TAILQ_REMOVEs
 *      the request out of evcon->requests but SKIPS freeing it because
 *      it's user-owned (evhttp_request_free_auto -> USER_OWNED check).
 *   2. evhttp_request_free — now that the request is detached from the
 *      evcon's TAILQ, we can free it without risking libevent walking
 *      back into freed memory.
 * If we did it in the opposite order the evcon's TAILQ_REMOVE would UAF
 * on the already-freed req->next chain link.
 *
 * Note: we no longer clear the back-pointer on the fan_ws_conn_t here.
 * The w->teardown pointer is only meaningful before this fires; once
 * fired, w has already forgotten about us on the write side (close/gc
 * both null it out immediately after arming). */
static void ws_deferred_teardown_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    ws_teardown_ctl_t *ctl = (ws_teardown_ctl_t *)arg;

    /* Outbuf-drain loop: on the non-cancelled path, libevent's
     * bufferevent_free does NOT flush pending output (see
     * be_socket_destruct in bufferevent_sock.c — it just close(fd)s
     * without pumping evbuffer_write). Any bytes still in the outbuf
     * when we free would be dropped, and for handlers that upgrade
     * and immediately close (accept + close in one gencb frame) the
     * handshake head + CLOSE frame are entirely in-outbuf at this
     * point. The write callback (be_socket_writecb) is what actually
     * pumps outbuf -> fd, and it only runs when the event loop
     * dispatches EV_WRITE for our bev. So: if outbuf is non-empty,
     * yield 1ms and try again. In practice this returns after one or
     * two ticks (< 1 ms wall clock); the DRAIN_MAX cap only kicks in
     * when the peer is flow-controlled or already dead, in which case
     * further waiting would not help. */
    if (!ctl->cancelled && ctl->bev && ctl->retry < WS_TEARDOWN_DRAIN_MAX) {
        struct evbuffer *out = bufferevent_get_output(ctl->bev);
        if (evbuffer_get_length(out) > 0) {
            ctl->retry++;
            struct timeval tv = {0, 1000};    /* 1 ms */
            if (event_base_once(fan_loop_base(), -1, EV_TIMEOUT,
                                ws_deferred_teardown_cb, ctl, &tv) == 0) {
                /* Re-armed successfully; keep both refs alive. */
                return;
            }
            /* Re-arm failed (rare): fall through and free now — leaking
             * a few bytes of outbuf is better than leaking evcon. */
        }
    }

    struct evhttp_connection *evcon = ctl->evcon;
    struct evhttp_request    *req   = ctl->ev_req;
    ctl->evcon  = NULL;
    ctl->ev_req = NULL;
    ctl->bev    = NULL;
    if (!ctl->cancelled) {
        /* Normal teardown path: evcon still live. Free it first (its
         * internal tailq walk TAILQ_REMOVEs but does NOT free the
         * user-owned request), then free the detached request. */
        if (evcon) evhttp_connection_free(evcon);
        if (req)   evhttp_request_free(req);
    } else {
        /* Cancelled path: evcon has already been freed by
         * evhttp_free -> evhttp_connection_free (which nulled evcon and
         * ran the tailq walk that detached but skipped freeing our
         * user-owned request). Just free the leftover request. */
        if (req) evhttp_request_free(req);
    }
    ws_teardown_ctl_release(ctl);
}

/* Schedule an out-of-stack teardown so:
 *   (a) The free runs on a future tick, never on the httpd_gencb /
 *       readcb stack that triggered us (mandatory: freeing evcon from
 *       inside libevent's bev readcb path is a use-after-free).
 *   (b) Pending output (handshake head, CLOSE frame) actually reaches
 *       the wire. libevent's `bufferevent_free` does NOT flush pending
 *       output (see be_socket_destruct in bufferevent_sock.c — just
 *       close(fd) without a final evbuffer_write). Bytes still in the
 *       outbuf when we free are dropped. `be_socket_writecb` is what
 *       pumps outbuf -> fd, and it only runs when the event loop
 *       dispatches EV_WRITE for the bev. We schedule teardown on the
 *       next tick and, if the outbuf is still non-empty when the
 *       timer fires, re-arm at 1ms intervals until it drains (or the
 *       DRAIN_MAX cap kicks in for a flow-controlled / dead peer).
 *       See ws_deferred_teardown_cb for the drain loop.
 *
 * The caller (close / gc) has already nulled out its own pointers to
 * evcon/ev_req/bev — we take a fresh copy so the drain loop can query
 * evbuffer_get_length even after w is GC'd. We install the ctl block
 * back on w so a subsequent closecb (server:close -> evhttp_free) can
 * cancel us. */
static void ws_schedule_teardown(fan_ws_conn_t *w,
                                 struct evhttp_connection *evcon,
                                 struct evhttp_request    *ev_req,
                                 struct bufferevent       *bev) {
    ws_teardown_ctl_t *ctl = (ws_teardown_ctl_t *)calloc(1, sizeof(*ctl));
    if (!ctl) {
        /* Alloc failure: best-effort synchronous free. Only reachable
         * under OOM, where there is nothing better to do. Note:
         * evhttp_connection_free will cascade to freeing ev_req via its
         * request-queue loop — do NOT free ev_req separately (double-free). */
        if (evcon) evhttp_connection_free(evcon);
        return;
    }
    ctl->refcnt = 2;                        /* w + timer */
    ctl->evcon  = evcon;
    ctl->ev_req = ev_req;
    ctl->bev    = bev;                      /* borrowed; nulled when consumed */
    ctl->owner  = w;

    struct timeval tv = {0, 0};             /* next tick; drain loop re-arms */
    if (event_base_once(fan_loop_base(), -1, EV_TIMEOUT,
                        ws_deferred_teardown_cb, ctl, &tv) != 0) {
        /* Cannot schedule — fall back to synchronous free (rare). */
        free(ctl);
        if (evcon) evhttp_connection_free(evcon);
        return;
    }
    w->teardown = ctl;
    /* Rebind evcon closecb from ws_evcon_closecb (arg=w) to the
     * cancel-only closecb (arg=ctl). After this point evhttp may free
     * the evcon (via server:close/evhttp_free) and it will call back
     * against `ctl` only, so it's safe even after w is GC'd. */
    if (evcon) {
        evhttp_connection_set_closecb(evcon, ws_teardown_cancel_closecb, ctl);
    }
}

/* --- Request-only next-tick free (separate from the evcon+req teardown
 *     control block).
 *
 * ws_evcon_closecb runs when libevent is freeing the evcon on us (server
 * shutdown / client half-close). At that point evhttp_connection_free's
 * internal tailq walk TAILQ_REMOVEs our USER_OWNED request but skips
 * freeing it — we own it via evhttp_request_own (see l_req_ws_accept).
 * Freeing it synchronously from inside closecb is illegal (we are on
 * libevent's own stack, walking that same tailq). Defer to the next tick.
 *
 * This is intentionally NOT the ws_teardown_ctl_t path: that block
 * models a two-sided teardown ticket (both close/gc and the timer hold
 * a ref, and closecb may race to cancel). Here there is exactly one
 * pending action — free one request — so a tiny dedicated wrapper is
 * clearer than reusing ctl with refcnt=1 cancelled=1. */
typedef struct ws_pending_req_free_s {
    struct evhttp_request *req;
} ws_pending_req_free_t;

static void ws_deferred_request_free_cb(evutil_socket_t fd, short what,
                                        void *arg) {
    (void)fd; (void)what;
    ws_pending_req_free_t *p = (ws_pending_req_free_t *)arg;
    if (p->req) evhttp_request_free(p->req);
    free(p);
}

static void ws_schedule_request_only_free(struct evhttp_request *req) {
    if (!req) return;
    ws_pending_req_free_t *p =
        (ws_pending_req_free_t *)calloc(1, sizeof(*p));
    if (!p) {
        /* OOM: leak the request (mirrors v1 OOM behavior). Freeing
         * synchronously from inside evcon_free's tailq walk is unsafe. */
        return;
    }
    p->req = req;
    struct timeval tv = {0, 0};
    if (event_base_once(fan_loop_base(), -1, EV_TIMEOUT,
                        ws_deferred_request_free_cb, p, &tv) != 0) {
        /* Fallback: last-resort. Not ideal from inside evcon_free's
         * tailq walk, but leaking is worse. */
        evhttp_request_free(req);
        free(p);
    }
}

static fan_ws_conn_t *check_ws(lua_State *L, int idx) {
    return (fan_ws_conn_t *)luaL_checkudata(L, idx, FAN_WS_CONN_MT);
}

/* Ensure the caller's protocol invariants line up before we touch bev. */
static int ws_is_live(fan_ws_conn_t *w) {
    return (w->flags & WS_ST_EVCON_LIVE) && !(w->flags & WS_ST_EOF) && w->bev;
}

/* Append to the fragment buffer. Grows on demand. */
static int ws_msg_append(fan_ws_conn_t *w, const char *data, size_t n) {
    if (n == 0) return 0;
    size_t need = w->msg_len + n;
    if (need > w->msg_cap) {
        size_t cap = w->msg_cap ? w->msg_cap : 256;
        while (cap < need) cap *= 2;
        char *nb = (char *)realloc(w->msg_buf, cap);
        if (!nb) return -1;
        w->msg_buf = nb;
        w->msg_cap = cap;
    }
    memcpy(w->msg_buf + w->msg_len, data, n);
    w->msg_len = need;
    return 0;
}

/* Push a completed message onto the ready slot. Ownership of `buf` moves
 * to the ws userdata (or freed on drop). If the slot is already full
 * we protocol-fail the connection: a well-behaved peer never sends a
 * second full message while the previous is still queued because the
 * server is naturally paced by :recv(). */
static void ws_ready_set(fan_ws_conn_t *w, char *buf, size_t len,
                         const char *op) {
    if (w->ready_body) {
        /* backpressure violation — treat as protocol error */
        free(buf);
        w->err = "recv backpressure violated";
        w->flags |= WS_ST_EOF;
        return;
    }
    w->ready_body = buf;
    w->ready_len  = len;
    w->ready_op   = op;
}

/* Write a control / data frame straight to the bev. Returns 0/-1. */
static int ws_write_frame(fan_ws_conn_t *w, int fin, int opcode, int rsv1,
                          const char *payload, size_t plen) {
    if (!ws_is_live(w)) return -1;
    struct evbuffer *out = bufferevent_get_output(w->bev);
    return fan_ws_encode_frame(out, fin, opcode, rsv1, payload, plen);
}

/* Once a complete message has been assembled, either deliver it to a
 * parked recv or queue it for the next recv() call. */
static void ws_deliver_message(fan_ws_conn_t *w) {
    const char *op = (w->msg_op == FAN_WS_OP_TEXT) ? "text" : "binary";
    char  *body = w->msg_buf;
    size_t len  = w->msg_len;
    int   deflated = w->msg_deflated;

    /* Detach: ws_msg_reset would zero these; we hand ownership out. */
    w->msg_buf = NULL;
    w->msg_len = 0;
    w->msg_cap = 0;
    w->msg_op = 0;
    w->msg_deflated = 0;

    if (deflated) {
        /* RFC 7692 §7.2.2: append the sync-flush trailer and inflate. */
        static const char TRAILER[4] = { '\x00', '\x00', '\xff', '\xff' };
        size_t sealed_len = len + 4;
        char *sealed = (char *)malloc(sealed_len);
        if (!sealed) {
            free(body);
            w->err = "oom";
            w->flags |= WS_ST_EOF;
            return;
        }
        if (len > 0) memcpy(sealed, body, len);
        memcpy(sealed + len, TRAILER, 4);
        free(body);
        char *inflated = NULL; size_t ilen = 0; const char *ierr = NULL;
        int rc = fan_zlib_inflate_raw_c(sealed, sealed_len, &inflated, &ilen, &ierr);
        free(sealed);
        if (rc != 0) {
            w->err = ierr ? ierr : "inflate failed";
            w->flags |= WS_ST_EOF;
            return;
        }
        body = inflated;
        len  = ilen;
    }

    if (w->co && w->co_ref != LUA_NOREF) {
        lua_State *co = w->co;
        int ref = w->co_ref;
        w->co = NULL;
        w->co_ref = LUA_NOREF;
        if (body) {
            lua_pushlstring(co, body, len);
            free(body);
        } else {
            lua_pushliteral(co, "");
        }
        lua_pushstring(co, op);
        fan_coro_wake(g_ws_main_L, co, ref, 2);
    } else {
        /* Queue for the next :recv() call. If backpressure trips the
         * queue this call marks EOF+err so the next recv sees it. */
        ws_ready_set(w, body, len, op);
    }
}

/* Wake a parked recv with (nil, err) — used for CLOSE / eventcb. */
static void ws_fail_parked(fan_ws_conn_t *w, const char *err) {
    if (!w->co || w->co_ref == LUA_NOREF) return;
    lua_State *co = w->co;
    int ref = w->co_ref;
    w->co = NULL;
    w->co_ref = LUA_NOREF;
    lua_pushnil(co);
    lua_pushstring(co, err ? err : "closed");
    fan_coro_wake(g_ws_main_L, co, ref, 2);
}

/* ---- bufferevent callbacks --------------------------------------------- */

static void ws_readcb(struct bufferevent *bev, void *arg) {
    fan_ws_conn_t *w = (fan_ws_conn_t *)arg;
    if (!ws_is_live(w)) return;
    struct evbuffer *in = bufferevent_get_input(bev);

    for (;;) {
        fan_ws_frame_t f;
        const char *perr = NULL;
        int rc = fan_ws_parse_frame(in, &f, 1 /* require_mask (server) */, &perr);
        if (rc == 0) return;         /* need more bytes */
        if (rc == -1) {
            w->err = perr ? perr : "protocol error";
            w->flags |= WS_ST_EOF;
            ws_fail_parked(w, w->err);
            return;
        }

        /* Reject RSV1 on non-data / when we didn't negotiate deflate. */
        if (f.rsv1 &&
            (f.opcode == FAN_WS_OP_CONT || !(w->flags & WS_ST_DEFLATE))) {
            fan_ws_frame_dispose(&f);
            w->err = "protocol error: RSV1 without permessage-deflate";
            w->flags |= WS_ST_EOF;
            ws_fail_parked(w, w->err);
            return;
        }

        switch (f.opcode) {
        case FAN_WS_OP_PING: {
            /* auto-answer with the same payload */
            ws_write_frame(w, 1, FAN_WS_OP_PONG, 0,
                           f.payload, f.plen);
            fan_ws_frame_dispose(&f);
            break;
        }
        case FAN_WS_OP_PONG: {
            /* ignored — v2 has no ping-tracking hook yet */
            fan_ws_frame_dispose(&f);
            break;
        }
        case FAN_WS_OP_CLOSE: {
            /* echo close + tear down (send-side only; keep bev alive
             * until :close / __gc so the parked recv can wake first) */
            if (!(w->flags & WS_ST_CLOSE_SENT)) {
                ws_write_frame(w, 1, FAN_WS_OP_CLOSE, 0,
                               f.payload, f.plen);
                w->flags |= WS_ST_CLOSE_SENT;
            }
            fan_ws_frame_dispose(&f);
            w->flags |= WS_ST_EOF;
            ws_fail_parked(w, "closed");
            return;
        }
        case FAN_WS_OP_TEXT:
        case FAN_WS_OP_BIN: {
            if (w->msg_op != 0) {
                fan_ws_frame_dispose(&f);
                w->err = "protocol error: new data opcode inside fragment";
                w->flags |= WS_ST_EOF;
                ws_fail_parked(w, w->err);
                return;
            }
            w->msg_op = f.opcode;
            w->msg_deflated = f.rsv1 ? 1 : 0;
            if (f.plen > 0 && ws_msg_append(w, f.payload, f.plen) != 0) {
                fan_ws_frame_dispose(&f);
                w->err = "oom";
                w->flags |= WS_ST_EOF;
                ws_fail_parked(w, w->err);
                return;
            }
            int fin = f.fin;
            fan_ws_frame_dispose(&f);
            if (fin) {
                ws_deliver_message(w);
                /* deliver_message may have set an error (inflate fail). */
                if (w->flags & WS_ST_EOF) {
                    ws_fail_parked(w, w->err);
                    return;
                }
            }
            break;
        }
        case FAN_WS_OP_CONT: {
            if (w->msg_op == 0) {
                fan_ws_frame_dispose(&f);
                w->err = "protocol error: continuation without initial data";
                w->flags |= WS_ST_EOF;
                ws_fail_parked(w, w->err);
                return;
            }
            if (f.plen > 0 && ws_msg_append(w, f.payload, f.plen) != 0) {
                fan_ws_frame_dispose(&f);
                w->err = "oom";
                w->flags |= WS_ST_EOF;
                ws_fail_parked(w, w->err);
                return;
            }
            int fin = f.fin;
            fan_ws_frame_dispose(&f);
            if (fin) {
                ws_deliver_message(w);
                if (w->flags & WS_ST_EOF) {
                    ws_fail_parked(w, w->err);
                    return;
                }
            }
            break;
        }
        default: {
            fan_ws_frame_dispose(&f);
            w->err = "protocol error: unknown opcode";
            w->flags |= WS_ST_EOF;
            ws_fail_parked(w, w->err);
            return;
        }
        }
    }
}

static void ws_eventcb(struct bufferevent *bev, short what, void *arg) {
    (void)bev;
    fan_ws_conn_t *w = (fan_ws_conn_t *)arg;
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        w->flags |= WS_ST_EOF;
        if (!w->err) w->err = (what & BEV_EVENT_EOF) ? "eof" : "io error";
        ws_fail_parked(w, w->err);
    }
}

/* Cancel-only closecb installed on evcon after a teardown timer is armed.
 * At that point the fan_ws_conn_t may or may not still exist; this cb
 * only touches the ctl block so it is safe either way.
 *
 * If server:close (evhttp_free) beats our timer, this fires from inside
 * evhttp_connection_free — libevent is freeing evcon out from under us,
 * so we set cancelled=1 + null out ctl->evcon so the timer skips the
 * evhttp_connection_free call. However we KEEP ctl->ev_req: the request
 * was user-owned (see evhttp_request_own in httpd.c:l_req_ws_accept),
 * and evhttp_connection_free's request-list TAILQ loop will
 * TAILQ_REMOVE it but SKIP the actual free (evhttp_request_free_auto
 * checks EVHTTP_USER_OWNED). The timer will pick up the still-tail-
 * detached request and free it.
 *
 * This callback does NOT release any ctl refcount: it does not own a
 * ticket (only w and the timer do). */
static void ws_teardown_cancel_closecb(struct evhttp_connection *evcon, void *arg) {
    (void)evcon;
    ws_teardown_ctl_t *ctl = (ws_teardown_ctl_t *)arg;
    if (!ctl) return;
    ctl->cancelled = 1;
    ctl->evcon  = NULL;   /* libevent is freeing evcon itself */
    ctl->bev    = NULL;   /* bev is owned by evcon; goes away with it. Also
                           * suppresses the outbuf-drain loop in the timer
                           * callback (drain only runs on !cancelled paths). */
    /* ctl->ev_req intentionally retained — see docs above. */
    /* Wake up any still-parked coroutine on the owner (mirrors the
     * duty of the original ws_evcon_closecb); owner may have been GC'd,
     * in which case it's NULL. */
    if (ctl->owner) {
        fan_ws_conn_t *w = ctl->owner;
        w->flags &= ~WS_ST_EVCON_LIVE;
        w->flags |=  WS_ST_EOF;
        w->evcon  = NULL;
        w->ev_req = NULL;
        w->bev    = NULL;
        if (!w->err) w->err = "server closed";
        ws_fail_parked(w, w->err);
    }
}

/* evcon closecb (initial, installed at accept): libevent is about to free
 * (or is freeing) the evcon. This fires from two paths:
 *   (a) server:close() -> evhttp_free -> evhttp_connection_free
 *   (b) client half-close / IO error on the persistent path
 *
 * At this stage there is no teardown ctl yet (close/gc haven't run), so
 * we only need to notify the fan_ws_conn_t and fail any parked recv.
 *
 * The user-owned request tied to the evcon (via evhttp_request_own at
 * accept) is NOT freed by libevent here — evhttp_free's tailq walk
 * skips USER_OWNED. We must free it ourselves; the safe time to do so
 * is AFTER this cb returns (evcon_free continues to TAILQ_REMOVE + fd
 * shutdown), so defer via event_base_once. */
static void ws_evcon_closecb(struct evhttp_connection *evcon, void *arg) {
    (void)evcon;
    fan_ws_conn_t *w = (fan_ws_conn_t *)arg;

    /* Snapshot the user-owned request pointer before we forget it, so we
     * can free it out-of-line on the next tick. */
    struct evhttp_request *req = w->ev_req;

    /* Clear ownership pointers BEFORE evhttp_free runs its
     * evhttp_connection_free path. Our __gc / :close will now be no-ops. */
    w->flags &= ~WS_ST_EVCON_LIVE;
    w->flags |=  WS_ST_EOF;
    w->evcon  = NULL;
    w->ev_req = NULL;
    w->bev    = NULL;
    if (!w->err) w->err = "server closed";
    ws_fail_parked(w, w->err);

    if (req) {
        /* Free the user-owned request on the next tick, after evcon_free
         * has finished its tailq walk. See ws_schedule_request_only_free
         * for why this is a dedicated helper (not a refcnt=1 ctl). */
        ws_schedule_request_only_free(req);
    }
}

/* ---- Lua-facing methods ------------------------------------------------ */

/* WS:state() -> "open" | "closing" | "closed" */
static int l_ws_state(lua_State *L) {
    fan_ws_conn_t *w = check_ws(L, 1);
    if (w->flags & WS_ST_EOF) lua_pushliteral(L, "closed");
    else if (w->flags & WS_ST_CLOSE_SENT) lua_pushliteral(L, "closing");
    else lua_pushliteral(L, "open");
    return 1;
}

/* Internal send helper — encodes and writes one frame. */
static int ws_send_one(lua_State *L, fan_ws_conn_t *w,
                       int fin, int opcode, int rsv1,
                       const char *payload, size_t plen) {
    if (!ws_is_live(w)) {
        lua_pushnil(L);
        lua_pushliteral(L, "closed");
        return 2;
    }
    if (w->flags & WS_ST_SEND_BUSY) {
        /* single-threaded model: this can only be reached from within
         * a control-frame auto-send while we were already inside another
         * send. Currently unreachable via the public API, but return a
         * clean error rather than corrupt frame interleaving. */
        lua_pushnil(L);
        lua_pushliteral(L, "send busy");
        return 2;
    }
    w->flags |= WS_ST_SEND_BUSY;
    int rc = ws_write_frame(w, fin, opcode, rsv1, payload, plen);
    w->flags &= ~WS_ST_SEND_BUSY;
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "write failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* WS:send(msg [, opcode]) — text unless opcode == "binary". */
static int l_ws_send(lua_State *L) {
    fan_ws_conn_t *w = check_ws(L, 1);
    size_t inlen = 0;
    const char *in = luaL_optlstring(L, 2, "", &inlen);
    int op = FAN_WS_OP_TEXT;
    if (!lua_isnoneornil(L, 3)) {
        if (lua_isnumber(L, 3)) {
            op = (int)lua_tointeger(L, 3) & 0x0f;
        } else {
            const char *s = luaL_checkstring(L, 3);
            if (strcmp(s, "binary") == 0) op = FAN_WS_OP_BIN;
            else if (strcmp(s, "text") == 0) op = FAN_WS_OP_TEXT;
            else {
                lua_pushnil(L);
                lua_pushfstring(L, "unknown opcode: %s", s);
                return 2;
            }
        }
    }
    /* permessage-deflate: compress the payload with the sync-flush
     * trailer stripped, per RFC 7692 §7.2.1. Empty payload still emits
     * one byte so the peer has something to decompress. */
    if ((w->flags & WS_ST_DEFLATE) && inlen > 0) {
        char *comp = NULL; size_t clen = 0; const char *derr = NULL;
        int rc = fan_zlib_deflate_raw_c(in, inlen, -1 /*default*/, 1 /*sync*/,
                                        &comp, &clen, &derr);
        if (rc != 0) {
            lua_pushnil(L);
            lua_pushstring(L, derr ? derr : "deflate failed");
            return 2;
        }
        /* strip trailing 00 00 FF FF sync marker */
        if (clen >= 4 && memcmp(comp + clen - 4, "\x00\x00\xff\xff", 4) == 0) {
            clen -= 4;
        }
        if (clen == 0) {
            /* emit at least a single 0x00 empty block */
            free(comp);
            comp = (char *)malloc(1);
            if (!comp) {
                lua_pushnil(L);
                lua_pushliteral(L, "oom");
                return 2;
            }
            comp[0] = 0;
            clen = 1;
        }
        int r = ws_send_one(L, w, 1, op, 1 /*rsv1*/, comp, clen);
        free(comp);
        return r;
    }
    return ws_send_one(L, w, 1, op, 0, in, inlen);
}

/* WS:send_binary(msg) — shortcut. */
static int l_ws_send_binary(lua_State *L) {
    /* Force arg 3 to "binary" so l_ws_send takes the binary branch. Pad
     * the stack first — the caller passes (self, msg) with top=2, and
     * lua_replace(L, 3) on an empty slot is UB. */
    lua_settop(L, 3);           /* pushes nil at index 3 if missing */
    lua_pushliteral(L, "binary");
    lua_replace(L, 3);
    return l_ws_send(L);
}

/* WS:ping([data]) */
static int l_ws_ping(lua_State *L) {
    fan_ws_conn_t *w = check_ws(L, 1);
    size_t plen = 0;
    const char *p = luaL_optlstring(L, 2, "", &plen);
    return ws_send_one(L, w, 1, FAN_WS_OP_PING, 0, p, plen);
}

/* WS:pong([data]) — proactive unsolicited pong (RFC 6455 §5.5.3). */
static int l_ws_pong(lua_State *L) {
    fan_ws_conn_t *w = check_ws(L, 1);
    size_t plen = 0;
    const char *p = luaL_optlstring(L, 2, "", &plen);
    return ws_send_one(L, w, 1, FAN_WS_OP_PONG, 0, p, plen);
}

/* WS:close([code [, reason]]) — sends CLOSE frame + tears down. */
static int l_ws_close(lua_State *L) {
    fan_ws_conn_t *w = check_ws(L, 1);
    if ((w->flags & WS_ST_CLOSE_SENT) || !ws_is_live(w)) {
        /* already fully closed — final teardown */
        w->flags |= WS_ST_EOF;
    } else {
        int code = (int)luaL_optinteger(L, 2, 1000);
        size_t rlen = 0;
        const char *reason = luaL_optlstring(L, 3, "", &rlen);
        /* payload: 2-byte big-endian code + reason */
        size_t plen = 2 + rlen;
        char stack_buf[128];
        char *buf = (plen <= sizeof(stack_buf)) ? stack_buf : (char *)malloc(plen);
        if (!buf) {
            lua_pushnil(L);
            lua_pushliteral(L, "oom");
            return 2;
        }
        buf[0] = (char)((code >> 8) & 0xff);
        buf[1] = (char)(code & 0xff);
        if (rlen > 0) memcpy(buf + 2, reason, rlen);
        ws_write_frame(w, 1, FAN_WS_OP_CLOSE, 0, buf, plen);
        if (buf != stack_buf) free(buf);
        w->flags |= WS_ST_CLOSE_SENT;
    }

    /* Tear down evcon (if still ours). evhttp_free-driven closecb sets
     * WS_ST_EVCON_LIVE=0 first, so a double-close is safe. Defer the
     * actual free to the next event-loop tick — handlers commonly close
     * from inside httpd_gencb which is itself on the bev's readcb stack,
     * and freeing evcon synchronously there tips libevent into UAF when
     * it returns. Mirrors v1 ws_deferred_free_cb.
     *
     * Note: ws_schedule_teardown rebinds evcon's closecb to a cancel-only
     * hook (arg=ctl) before returning. During the 20 ms flush window a
     * `server:close()` (which triggers evhttp_free) can still race the
     * timer; the cancel-only hook is how we learn libevent is freeing
     * the evcon out from under us and marks the pending ctl cancelled.
     * Without it the timer would double-free the evcon (UAF). */
    if (w->flags & WS_ST_EVCON_LIVE) {
        struct evhttp_connection *evcon = w->evcon;
        struct evhttp_request    *req   = w->ev_req;
        struct bufferevent       *bev   = w->bev;
        w->evcon  = NULL;
        w->ev_req = NULL;
        w->bev    = NULL;
        w->flags &= ~WS_ST_EVCON_LIVE;
        /* From the local API's perspective the connection is done — the
         * TCP-level FIN is on the way. Mark EOF so :state() reports
         * "closed" immediately, matching v1 semantics (test_websocket
         * expects "closed" right after :close()). */
        w->flags |= WS_ST_EOF;
        ws_schedule_teardown(w, evcon, req, bev);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* WS:recv() -> msg, opcode  |  nil, err */
static int l_ws_recv(lua_State *L) {
    fan_ws_conn_t *w = check_ws(L, 1);

    /* Fast path: a completed message is queued. */
    if (w->ready_body || (w->ready_op && w->ready_len == 0)) {
        char *body = w->ready_body;
        size_t len = w->ready_len;
        const char *op = w->ready_op;
        w->ready_body = NULL;
        w->ready_len  = 0;
        w->ready_op   = NULL;
        if (body) {
            lua_pushlstring(L, body, len);
            free(body);
        } else {
            lua_pushliteral(L, "");
        }
        lua_pushstring(L, op);
        return 2;
    }

    if (w->flags & WS_ST_EOF) {
        lua_pushnil(L);
        lua_pushstring(L, w->err ? w->err : "closed");
        return 2;
    }
    if (!ws_is_live(w)) {
        lua_pushnil(L);
        lua_pushliteral(L, "closed");
        return 2;
    }
    if (w->co) {
        /* another recv already parked — v2 single-threaded contract says
         * this is illegal (both would deadlock). Return an error. */
        lua_pushnil(L);
        lua_pushliteral(L, "recv already in progress");
        return 2;
    }
    int ref = fan_coro_park(L);
    if (ref == LUA_NOREF) {
        return luaL_error(L, "ws:recv must be called from a coroutine");
    }
    w->co = L;
    w->co_ref = ref;
    return lua_yield(L, 0);
}

/* WS:receive — v1 alias. */
static int l_ws_receive(lua_State *L) { return l_ws_recv(L); }

/* __gc: mirror :close's teardown (idempotent). Uses the deferred
 * schedule for the same reasons :close does — __gc can plausibly run
 * from anywhere in the event loop and we do not want to fire an evcon
 * free on the very readcb stack that's about to return. */
static int l_ws_gc(lua_State *L) {
    fan_ws_conn_t *w = check_ws(L, 1);
    if (w->flags & WS_ST_EVCON_LIVE) {
        struct evhttp_connection *evcon = w->evcon;
        struct evhttp_request    *req   = w->ev_req;
        struct bufferevent       *bev   = w->bev;
        w->evcon  = NULL;
        w->ev_req = NULL;
        w->bev    = NULL;
        w->flags &= ~WS_ST_EVCON_LIVE;
        ws_schedule_teardown(w, evcon, req, bev);
    }
    /* Release our refcount on any pending teardown ctl (close/gc armed it,
     * the timer holds the other ref). Because w is about to disappear,
     * clear the weak owner back-pointer so the cancel-only closecb (which
     * remains armed on evcon) does not touch freed memory if server:close
     * races the timer. */
    if (w->teardown) {
        ws_teardown_ctl_t *ctl = w->teardown;
        w->teardown = NULL;
        ctl->owner = NULL;
        ws_teardown_ctl_release(ctl);
    }
    /* Free any leftover buffers (partial fragment, queued ready message). */
    if (w->msg_buf)    { free(w->msg_buf);    w->msg_buf = NULL; }
    if (w->ready_body) { free(w->ready_body); w->ready_body = NULL; }
    return 0;
}

/* ---- Lua registration -------------------------------------------------- */

static const luaL_Reg ws_methods[] = {
    {"send",         l_ws_send},
    {"send_binary",  l_ws_send_binary},
    {"ping",         l_ws_ping},
    {"pong",         l_ws_pong},
    {"close",        l_ws_close},
    {"recv",         l_ws_recv},
    {"receive",      l_ws_receive},
    {"state",        l_ws_state},
    {NULL, NULL},
};

/* fan.ws.conn is an opaque userdata (no per-instance fields; state is in
 * the fan_ws_conn_t struct). __index points straight at the methods table
 * — no user-value override slot, no __newindex hook. This matches how
 * fan.tcp conn objects are exposed and keeps the write-side hot path
 * (send/recv) free of a Lua-table lookup for every method dispatch. */
void fan_ws_register(lua_State *L) {
    g_ws_main_L = fan_coro_main(L);  /* stable main thread, not the require() coroutine */
    luaL_newmetatable(L, FAN_WS_CONN_MT);
    /* Build the methods table and install it directly as __index. */
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, ws_methods, 0);
#else
    luaL_register(L, NULL, ws_methods);
#endif
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_ws_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}

void *fan_ws_conn_push(lua_State *L, void *evcon, void *bev, void *ev_req,
                       int deflate_enabled) {
    fan_ws_conn_t *w = (fan_ws_conn_t *)lua_newuserdata(L, sizeof(*w));
    memset(w, 0, sizeof(*w));
    w->evcon  = (struct evhttp_connection *)evcon;
    w->ev_req = (struct evhttp_request *)   ev_req;
    w->bev    = (struct bufferevent *)      bev;
    w->co_ref = LUA_NOREF;
    w->flags  = WS_ST_EVCON_LIVE | (deflate_enabled ? WS_ST_DEFLATE : 0);

    luaL_getmetatable(L, FAN_WS_CONN_MT);
    lua_setmetatable(L, -2);

    /* Wire up bev callbacks + evcon closecb before enabling read. */
    if (w->bev) {
        bufferevent_setcb(w->bev, ws_readcb, NULL, ws_eventcb, w);
        bufferevent_enable(w->bev, EV_READ | EV_WRITE);
    }
    if (w->evcon) {
        evhttp_connection_set_closecb(w->evcon, ws_evcon_closecb, w);
    }
    return w;
}
