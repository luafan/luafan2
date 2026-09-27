/*
 * httpd_metrics.c — LuaFan v2 fan.httpd_c metrics (M14.C-j).
 * See httpd_metrics.h for the contract and rationale.
 */
#include "httpd_metrics.h"

#include <event2/buffer.h>
#include <lua.h>
#include <lauxlib.h>

#include <string.h>
#include <time.h>

/* All counters are process-global. v2 runs one event loop on the main
 * thread, so plain non-atomic counters are safe. v1 used _Atomic because
 * it supported multi-worker builds; v2's multi-worker story is fork()
 * (separate processes with independent counter arrays), not threads. */
typedef struct {
    unsigned long requests_total;
    unsigned long requests_active;
    unsigned long bytes_sent;
    unsigned long bytes_received;
    unsigned long errors_total;
    unsigned long connections_total;
    unsigned long keepalive_reused;
    unsigned long requests_get;
    unsigned long requests_post;
    unsigned long requests_put;
    unsigned long requests_delete;
    unsigned long requests_other;
    unsigned long responses_2xx;
    unsigned long responses_3xx;
    unsigned long responses_4xx;
    unsigned long responses_5xx;
    time_t start_time;
    int initialised;
} httpd_metrics_t;

static httpd_metrics_t g_m = {0};

void fan_httpd_metrics_init(void) {
    if (g_m.initialised) return;
    g_m.start_time = time(NULL);
    g_m.initialised = 1;
}

void fan_httpd_metrics_request_start(const char *method) {
    fan_httpd_metrics_init();
    g_m.requests_total++;
    g_m.requests_active++;

    if (method == NULL) {
        g_m.requests_other++;
    } else if (strcmp(method, "GET") == 0) {
        g_m.requests_get++;
    } else if (strcmp(method, "POST") == 0) {
        g_m.requests_post++;
    } else if (strcmp(method, "PUT") == 0) {
        g_m.requests_put++;
    } else if (strcmp(method, "DELETE") == 0) {
        g_m.requests_delete++;
    } else {
        g_m.requests_other++;
    }
}

void fan_httpd_metrics_request_end(int status, size_t bytes_sent) {
    if (g_m.requests_active > 0) g_m.requests_active--;
    g_m.bytes_sent += (unsigned long)bytes_sent;

    if (status >= 200 && status < 300)      g_m.responses_2xx++;
    else if (status >= 300 && status < 400) g_m.responses_3xx++;
    else if (status >= 400 && status < 500) { g_m.responses_4xx++; g_m.errors_total++; }
    else if (status >= 500)                 { g_m.responses_5xx++; g_m.errors_total++; }
    /* status < 200 (1xx, or negative sentinels) don't bump anything. */
}

void fan_httpd_metrics_connection(void) {
    fan_httpd_metrics_init();
    g_m.connections_total++;
}

void fan_httpd_metrics_add_recv(size_t n) {
    g_m.bytes_received += (unsigned long)n;
}

void fan_httpd_metrics_keepalive_reused(void) {
    g_m.keepalive_reused++;
}

size_t fan_httpd_metrics_render(struct evbuffer *buf) {
    fan_httpd_metrics_init();
    time_t uptime = time(NULL) - g_m.start_time;
    /* Prometheus exposition format 0.0.4: each metric prefixed by a
     * # HELP line and a # TYPE line, then `name value`. Simpler v1
     * shape (bare `name value` lines) is a subset of what Prometheus
     * will accept so scrapers just ignore the missing HELP/TYPE — but
     * a proper `# HELP` line is what curl-based ad-hoc debugging
     * expects. All counters are strictly monotonic (never decrement)
     * except `requests_active` which is a gauge. */
    size_t before = evbuffer_get_length(buf);
    evbuffer_add_printf(buf,
        "# HELP fan_httpd_uptime_seconds Seconds since first bind() / metrics init.\n"
        "# TYPE fan_httpd_uptime_seconds counter\n"
        "fan_httpd_uptime_seconds %ld\n"
        "# HELP fan_httpd_requests_total HTTP requests received.\n"
        "# TYPE fan_httpd_requests_total counter\n"
        "fan_httpd_requests_total %lu\n"
        "# HELP fan_httpd_requests_active Requests currently in flight.\n"
        "# TYPE fan_httpd_requests_active gauge\n"
        "fan_httpd_requests_active %lu\n"
        "# HELP fan_httpd_bytes_sent_total Response bytes written.\n"
        "# TYPE fan_httpd_bytes_sent_total counter\n"
        "fan_httpd_bytes_sent_total %lu\n"
        "# HELP fan_httpd_bytes_received_total Request-body bytes read.\n"
        "# TYPE fan_httpd_bytes_received_total counter\n"
        "fan_httpd_bytes_received_total %lu\n"
        "# HELP fan_httpd_errors_total Requests that returned 4xx or 5xx.\n"
        "# TYPE fan_httpd_errors_total counter\n"
        "fan_httpd_errors_total %lu\n"
        "# HELP fan_httpd_connections_total TCP connections accepted.\n"
        "# TYPE fan_httpd_connections_total counter\n"
        "fan_httpd_connections_total %lu\n"
        "# HELP fan_httpd_keepalive_reused_total Requests served on an existing keep-alive connection.\n"
        "# TYPE fan_httpd_keepalive_reused_total counter\n"
        "fan_httpd_keepalive_reused_total %lu\n"
        "# HELP fan_httpd_requests_by_method_total Requests broken down by HTTP method.\n"
        "# TYPE fan_httpd_requests_by_method_total counter\n"
        "fan_httpd_requests_by_method_total{method=\"GET\"} %lu\n"
        "fan_httpd_requests_by_method_total{method=\"POST\"} %lu\n"
        "fan_httpd_requests_by_method_total{method=\"PUT\"} %lu\n"
        "fan_httpd_requests_by_method_total{method=\"DELETE\"} %lu\n"
        "fan_httpd_requests_by_method_total{method=\"OTHER\"} %lu\n"
        "# HELP fan_httpd_responses_by_class_total Responses broken down by 2xx/3xx/4xx/5xx.\n"
        "# TYPE fan_httpd_responses_by_class_total counter\n"
        "fan_httpd_responses_by_class_total{class=\"2xx\"} %lu\n"
        "fan_httpd_responses_by_class_total{class=\"3xx\"} %lu\n"
        "fan_httpd_responses_by_class_total{class=\"4xx\"} %lu\n"
        "fan_httpd_responses_by_class_total{class=\"5xx\"} %lu\n",
        (long)uptime,
        g_m.requests_total,
        g_m.requests_active,
        g_m.bytes_sent,
        g_m.bytes_received,
        g_m.errors_total,
        g_m.connections_total,
        g_m.keepalive_reused,
        g_m.requests_get, g_m.requests_post, g_m.requests_put,
        g_m.requests_delete, g_m.requests_other,
        g_m.responses_2xx, g_m.responses_3xx,
        g_m.responses_4xx, g_m.responses_5xx);
    return evbuffer_get_length(buf) - before;
}

/* Push a flat Lua table onto L's stack. Same fields as the /metrics
 * output but keyed by short names (uptime_seconds, requests_total, ...)
 * so callers don't need to parse text. */
void fan_httpd_metrics_push_table(struct lua_State *L) {
    fan_httpd_metrics_init();
    lua_createtable(L, 0, 20);
#define SET_INT(name, val) do {                     \
        lua_pushinteger(L, (lua_Integer)(val));     \
        lua_setfield(L, -2, (name));                \
    } while (0)
    SET_INT("uptime_seconds",   (long)(time(NULL) - g_m.start_time));
    SET_INT("requests_total",   g_m.requests_total);
    SET_INT("requests_active",  g_m.requests_active);
    SET_INT("bytes_sent",       g_m.bytes_sent);
    SET_INT("bytes_received",   g_m.bytes_received);
    SET_INT("errors_total",     g_m.errors_total);
    SET_INT("connections_total",g_m.connections_total);
    SET_INT("keepalive_reused", g_m.keepalive_reused);
    SET_INT("requests_get",     g_m.requests_get);
    SET_INT("requests_post",    g_m.requests_post);
    SET_INT("requests_put",     g_m.requests_put);
    SET_INT("requests_delete",  g_m.requests_delete);
    SET_INT("requests_other",   g_m.requests_other);
    SET_INT("responses_2xx",    g_m.responses_2xx);
    SET_INT("responses_3xx",    g_m.responses_3xx);
    SET_INT("responses_4xx",    g_m.responses_4xx);
    SET_INT("responses_5xx",    g_m.responses_5xx);
#undef SET_INT
}
