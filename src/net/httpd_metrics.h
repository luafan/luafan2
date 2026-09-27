/*
 * httpd_metrics.h — LuaFan v2 fan.httpd_c metrics (M14.C-j).
 *
 * Process-global request/response counters, byte-for-byte the same shape
 * as v1's httpd_metrics.c so `/metrics` scrapes look identical. v2 is
 * single-threaded (one libevent loop, no worker threads), so unlike v1
 * we use plain `unsigned long` counters — no atomics needed.
 *
 * fan.httpd_c is opt-in: the /metrics endpoint only registers when the
 * user passes `opts.metrics = "/some/path"` to bind. This differs from
 * v1 which hard-coded /metrics on every bind and could clash with a
 * user's own /metrics handler. Counters themselves are always live so
 * `fan.httpd.metrics()` (Lua side) returns the same numbers regardless
 * of whether the HTTP endpoint is exposed.
 */
#ifndef FAN2_HTTPD_METRICS_H
#define FAN2_HTTPD_METRICS_H

#include <stddef.h>

/* Bump the request-total + active + per-method counters. `method` is
 * the wire method string (upper-case). NULL falls back to "other". */
void fan_httpd_metrics_request_start(const char *method);

/* Decrement active, add bytes_sent, bump the status-class counter for
 * `status`, and (for 4xx/5xx) bump errors_total. Safe to call at most
 * once per request; a mid-crash call at status=500 works fine. */
void fan_httpd_metrics_request_end(int status, size_t bytes_sent);

/* Bump connections_total once per new evhttp connection. Kept separate
 * from request_start because keep-alive lets one connection carry many
 * requests. */
void fan_httpd_metrics_connection(void);

/* Add bytes to bytes_received (called from body read paths). */
void fan_httpd_metrics_add_recv(size_t n);

/* Bump keepalive_reused when we detect an in-flight request over an
 * already-counted connection (v1 had this but never actually
 * incremented — we keep the field so scrapes don't lose columns). */
void fan_httpd_metrics_keepalive_reused(void);

/* Fill an evbuffer with Prometheus 0.0.4 exposition text. `buf` must be
 * a valid, empty evbuffer. Never fails; returns the number of bytes
 * written for logging. */
struct evbuffer;
size_t fan_httpd_metrics_render(struct evbuffer *buf);

/* Push a Lua table {name = value, ...} onto the given state's stack.
 * Same fields as the /metrics output. Used by fan.httpd.metrics()
 * (Lua-facing helper). */
struct lua_State;
void fan_httpd_metrics_push_table(struct lua_State *L);

/* Initialise the module. Idempotent; sets start_time on first call. */
void fan_httpd_metrics_init(void);

#endif /* FAN2_HTTPD_METRICS_H */
