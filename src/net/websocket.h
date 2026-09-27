/*
 * websocket.h — LuaFan v2 WebSocket (RFC 6455) — C data plane.
 *
 * Method: sink WebSocket frame encode/decode + read loop + send API into
 * C so the hot path does not cross Lua VM boundary per byte. Server-side
 * only in this iteration (client connect can be layered on fan.tcp+client
 * handshake later); driven from fan.httpd_c after the RFC 6455 handshake
 * has been performed.
 *
 * Ownership model (contrast with M14.C-d / M14.C-m):
 *   The fan_ws_conn_t owns the evhttp_connection outright. On the accept
 *   path we do NOT hand the bufferevent to fan.tcp — we install our own
 *   readcb/writecb/eventcb directly on evcon's bufferevent and hold the
 *   evcon pointer for teardown. evhttp is told about our closecb so a
 *   server:close() → evhttp_free path can invalidate us via callback
 *   before it frees the evcon; that mirrors libevent's own contract and
 *   removes the need for httpd_server_t.detached_conns bookkeeping.
 *
 * Frame codec API is exposed to the rest of the runtime (currently
 * consumed only by websocket.c itself, but split out so a future client
 * handshake reuse is trivial).
 *
 * D1 scope (this file):
 *   - Frame parse from an evbuffer (returns 0 need-more, 1 frame, -1 err)
 *   - Frame encode into an evbuffer
 *   - RFC 6455 mask application (4-byte aligned XOR)
 *   - RFC 6455 handshake accept-key computation (SHA1+base64 via openssl)
 *
 * Later D-stages layer the fan_ws_conn_t userdata + read loop + send API +
 * evcon-close plumbing on top of these primitives.
 */
#ifndef FAN2_NET_WEBSOCKET_H
#define FAN2_NET_WEBSOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <lua.h>

struct evbuffer;

/* ---- opcodes ------------------------------------------------------------- */
#define FAN_WS_OP_CONT   0x0
#define FAN_WS_OP_TEXT   0x1
#define FAN_WS_OP_BIN    0x2
#define FAN_WS_OP_CLOSE  0x8
#define FAN_WS_OP_PING   0x9
#define FAN_WS_OP_PONG   0xA

/* Owned frame view returned by fan_ws_parse_frame. `payload` is malloc'd
 * (may be NULL when plen == 0); caller must free it. Fields fin/rsv1/
 * opcode/mask carry the header semantics; masked frames are already
 * unmasked in-place by the parser so downstream code sees plaintext. */
typedef struct {
    int      fin;      /* 1 if FIN set */
    int      rsv1;     /* 1 if RSV1 set (permessage-deflate marker) */
    int      opcode;   /* FAN_WS_OP_* */
    char    *payload;  /* malloc'd payload (unmasked), or NULL if plen==0 */
    size_t   plen;     /* payload byte length */
} fan_ws_frame_t;

/* Free a frame's payload buffer (idempotent). Zero the struct after. */
void fan_ws_frame_dispose(fan_ws_frame_t *f);

/* Try to parse one frame from `in`. Returns:
 *    0  need more bytes (in unchanged; call again after readcb fires)
 *    1  full frame parsed and drained from `in`; `*out` populated
 *   -1  protocol error; `*err` set to a static string
 *
 * Enforces RFC 6455 rules that fan.websocket.lua also enforced:
 *   - control frames must be FIN and payload <= 125
 *   - RSV2/RSV3 must be zero (no extension in v2 defines them)
 *   - RSV1 on control frames is illegal
 *   - 7+16 / 7+64 length extensions
 *   - 4-byte mask when MASK bit set (server accept mode requires this)
 *
 * Server accept mode: pass `require_mask=1` so client→server frames
 * without a MASK bit are rejected (RFC 6455 §5.1). Client mode is
 * `require_mask=0`. */
int fan_ws_parse_frame(struct evbuffer *in, fan_ws_frame_t *out,
                       int require_mask, const char **err);

/* Encode one server→client frame (unmasked) into `out`. `payload` may be
 * NULL when `plen == 0`. Returns 0 on success, -1 on evbuffer error. */
int fan_ws_encode_frame(struct evbuffer *out,
                        int fin, int opcode, int rsv1,
                        const char *payload, size_t plen);

/* RFC 6455 accept-key digest: base64(SHA1(client_key || GUID)).
 * `out` must be at least 32 bytes. Returns 0 on success, -1 on failure
 * (OpenSSL not compiled in, or an unlikely runtime BIO failure). */
int fan_ws_compute_accept(const char *client_key, char *out, size_t outsz);

/* Push a fresh ws userdata onto `L` and take over the WebSocket data
 * plane on `bev`. `evcon` is the evhttp_connection that owned the bev
 * (passed as void* to keep libevent's http.h out of the include chain);
 * `ev_req` is the evhttp_request that was `own`ed away from evhttp at
 * accept time (evhttp will not free it, so the ws userdata does). Both
 * may be NULL for a bare-bev adopt (currently only the httpd handshake
 * path is a caller, and it always passes them).
 *
 * `deflate_enabled` toggles permessage-deflate (RFC 7692) on send/recv.
 *
 * On return the top of the stack holds the new userdata. The caller is
 * expected to return 1 to Lua right after so the userdata's uservalue
 * gets pinned by the returning coroutine's registry slot.
 *
 * Ownership transfer contract (contrast with M14.C-d fan.tcp adopt):
 *   - The ws userdata installs its own readcb / writecb / eventcb on
 *     bev and enables EV_READ|EV_WRITE.
 *   - Its __gc / :close will evhttp_request_free(ev_req) + then
 *     evhttp_connection_free(evcon), which cascades to bufferevent_free
 *     in the libevent-safe order.
 *   - Its evcon-closecb hook fires from evhttp_free (server:close) and
 *     just marks the ws as detached; the ws userdata's future :send /
 *     :recv return nil,"closed" and __gc becomes a no-op (evcon has
 *     already been freed by libevent).
 * Returns the userdata pointer (opaque; do not dereference from outside
 * websocket.c) so callers can stash it if they ever need to. */
void *fan_ws_conn_push(lua_State *L, void *evcon, void *bev, void *ev_req,
                       int deflate_enabled);

/* Register nothing at the moment — the ws userdata + Lua bindings land
 * in a later D-stage. Kept here so the register-time entry point is
 * predictable when we add it. */
void fan_ws_register(lua_State *L);

/* Clears callback state before the owning Lua state is closed. */
void fan_ws_clear_lua_state(void);

#endif
