/*
 * loop.c — LuaFan v2 event loop implementation.
 */
#include "loop.h"
#include "../platform.h"

#include <stddef.h>

static struct event_base *g_main_base = NULL;
static struct evdns_base *g_dnsbase = NULL;
static int g_running = 0;
static int g_break_requested = 0;

struct event_base *fan_loop_base(void) {
    if (FAN_UNLIKELY(g_main_base == NULL)) {
        g_main_base = event_base_new();
    }
    return g_main_base;
}

struct evdns_base *fan_loop_dnsbase(void) {
    if (FAN_UNLIKELY(g_dnsbase == NULL)) {
        struct event_base *base = fan_loop_base();
        if (!base) return NULL;
        /* EVDNS_BASE_INITIALIZE_NAMESERVERS: read /etc/resolv.conf */
        g_dnsbase = evdns_base_new(base, EVDNS_BASE_INITIALIZE_NAMESERVERS);
        if (g_dnsbase) {
            /* Bound the worst case when a configured nameserver is unreachable
             * (e.g. a cloud metadata resolver that drops our packets): cap the
             * per-query timeout and retry count so a resolve fails fast with an
             * error instead of hanging the whole event loop. */
            evdns_base_set_option(g_dnsbase, "timeout:", "2");
            evdns_base_set_option(g_dnsbase, "attempts:", "2");
            evdns_base_set_option(g_dnsbase, "max-timeouts:", "2");
        }
    }
    return g_dnsbase;
}

struct event_base *fan_loop_current_base(void) {
    /* M1: single base. M6 will consult a thread-local worker id here. */
    return fan_loop_base();
}

int fan_loop_run(void) {
    struct event_base *base = fan_loop_base();
    if (!base) return -1;
    /* If a break was requested before we even started (e.g. a coroutine ran to
     * completion synchronously and called fan.loopbreak), honour it up front:
     * evdns and other infrastructure may hold long-lived events that would
     * otherwise keep event_base_dispatch blocked forever. */
    if (g_break_requested) {
        g_break_requested = 0;
        return 0;
    }
    g_running = 1;
    /* EVLOOP_NO_EXIT_ON_EMPTY is NOT used: in M1 the loop returns when there is
     * nothing left to do, which is the natural "script finished" semantics. */
    int rc = event_base_dispatch(base);
    g_running = 0;
    g_break_requested = 0;
    return rc;
}

void fan_loop_break(void) {
    g_break_requested = 1;
    if (g_main_base) event_base_loopbreak(g_main_base);
}

int fan_loop_is_running(void) {
    return g_running;
}

void fan_loop_cleanup(void) {
    if (g_dnsbase) {
        evdns_base_free(g_dnsbase, 0);
        g_dnsbase = NULL;
    }
    if (g_main_base) {
        event_base_free(g_main_base);
        g_main_base = NULL;
    }
    g_running = 0;
}

/* After fork(): re-initialise libevent state in the child so kqueue/epoll
 * fds inherited from the parent are dropped and events are re-registered.
 * Callers must invoke this in the child branch of fork() before using any
 * event-driven API. Returns 0 on success, -1 if there is no live base. */
int fan_loop_reinit_after_fork(void) {
    if (!g_main_base) return 0;   /* nothing to reinit; base will be created on first use */
    /* evdns holds sockets that must be recreated too. Drop it; the next
     * fan.dns.resolve will build a fresh one on the reinit'd base. */
    if (g_dnsbase) {
        evdns_base_free(g_dnsbase, 0);
        g_dnsbase = NULL;
    }
    return event_reinit(g_main_base);
}
