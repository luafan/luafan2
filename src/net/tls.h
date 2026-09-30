/*
 * tls.h — LuaFan v2 TLS support (OpenSSL) for client (and later server) sockets.
 *
 * Compiled unconditionally; the OpenSSL-dependent body is guarded by
 * FAN_WITH_OPENSSL. When OpenSSL is disabled, fan_tls_client_bev returns NULL
 * with an explanatory error and fan.tls advertises available=false.
 *
 * The TLS connection reuses tcp.c's bufferevent connection model: we only
 * differ in how the bufferevent is constructed (bufferevent_openssl_socket_new
 * with a per-connection SSL over an SSL_CTX). Read/write/drain/close all flow
 * through the same tcp_conn_t machinery.
 */
#ifndef FAN2_NET_TLS_H
#define FAN2_NET_TLS_H

#include <stddef.h>            /* size_t */
#include <lua.h>
#include <event2/bufferevent.h>

/* One-time process init (OpenSSL algorithms, default verify paths). Safe to
 * call repeatedly. No-op when OpenSSL is disabled. */
void fan_tls_init(void);

/* Whether TLS is compiled in. */
int fan_tls_available(void);

/* Build a client-side TLS bufferevent (fd = -1, to be connected via
 * bufferevent_socket_connect_hostname). Applies SNI + optional peer/host
 * verification for `host`. On failure returns NULL and, if err is non-NULL,
 * sets *err to a static/borrowed message. Returns NULL when OpenSSL is off. */
struct bufferevent *fan_tls_client_bev(struct event_base *base,
                                       const char *host,
                                       int verify_peer,
                                       int verify_host,
                                       const char **err);

/* M17-2: extended client builder — same as fan_tls_client_bev but with
 * per-request TLS options (ssl_host / cainfo / capath / pkcs12).  The
 * SSL_CTX is drawn from a process-wide fingerprint-keyed cache so two
 * connections with identical TLS params share a single ctx (see the cache
 * design comment above the implementation in tls.c).  Passing all option
 * pointers as NULL is equivalent to fan_tls_client_bev.
 *
 * `ssl_host` overrides `host` for SNI and hostname verification when set
 * to a non-empty string; NULL / "" falls back to `host`.
 * `cainfo` / `capath` follow OpenSSL SSL_CTX_load_verify_locations semantics.
 * `pkcs12_path` / `pkcs12_password` load a client-cert bundle. */
struct bufferevent *fan_tls_client_bev_ex(struct event_base *base,
                                          const char *host,
                                          const char *ssl_host,
                                          int verify_peer,
                                          int verify_host,
                                          const char *cainfo,
                                          const char *capath,
                                          const char *pkcs12_path,
                                          const char *pkcs12_password,
                                          const char **err);

/* M21.1 — Fill `buf` with the OpenSSL peer-verify reason for `ssl_ptr`
 * (opaque SSL *).  Returns 1 if buf populated (verify failed with a
 * known reason), 0 if the handshake succeeded or ssl is not a TLS
 * bufferevent.  Used by tcp.c to append the specific X509 reason to a
 * "connect failed" error so callers see e.g.
 * "connection error: unable to get local issuer certificate" instead
 * of a bare "connection error".  Safe to call with ssl_ptr = NULL
 * (returns 0). */
int fan_tls_client_verify_reason(void *ssl_ptr, char *buf, size_t buflen);

/* ---- server side --------------------------------------------------------- */

/* Build a server SSL context from a PEM cert + key file pair. Returns an
 * opaque handle (owned by the caller; must be freed with
 * fan_tls_server_ctx_free) or NULL with *err set. NULL when OpenSSL is off. */
void *fan_tls_server_ctx_new(const char *cert_path, const char *key_path,
                             const char **err);

/* Free a server SSL context returned by fan_tls_server_ctx_new. NULL is a
 * no-op. Safe to call from any thread that owns the ctx. */
void  fan_tls_server_ctx_free(void *ctx);

/* Wrap an accepted socket fd in a server-side TLS bufferevent using ctx.
 * On failure returns NULL and, if err is non-NULL, sets *err. Returns NULL
 * when OpenSSL is off. */
struct bufferevent *fan_tls_server_bev(struct event_base *base, int fd,
                                       void *ctx, const char **err);

/* Register the fan.tls table on the module at stack top (-1). */
void fan_tls_register(lua_State *L);

#endif /* FAN2_NET_TLS_H */
