/*
 * loop.h — LuaFan v2 event loop (libevent) wrapper.
 *
 * M1 scope: a process-level main event_base singleton driving fan.loop() /
 * fan.loopbreak(). Per-thread worker bases arrive in M6; the API is written so
 * that "the current base" is a lookup, not a global constant, to ease that.
 */
#ifndef FAN2_RUNTIME_LOOP_H
#define FAN2_RUNTIME_LOOP_H

#include <event2/event.h>
#include <event2/dns.h>

/* Lazily create (once) and return the main event base. NULL on failure. */
struct event_base *fan_loop_base(void);

/* Lazily create (once) and return the DNS base bound to the main event base. */
struct evdns_base *fan_loop_dnsbase(void);

/* The base the calling context should schedule on. In M1 this is always the
 * main base; M6 will return the current worker's base when on a worker thread. */
struct event_base *fan_loop_current_base(void);

/* Run the loop until loopbreak / no more events. Returns 0 normally. */
int fan_loop_run(void);

/* Request the running loop to stop after the current iteration. */
void fan_loop_break(void);

/* Whether fan_loop_run() is currently inside event_base_dispatch. */
int fan_loop_is_running(void);

/* Free the main base (process teardown / tests). Safe to call when never made. */
void fan_loop_cleanup(void);

/* Re-initialise the main event base after fork() in the child. Must be called
 * in the child branch of fork() before using any event-driven API. Returns 0
 * on success, -1 on failure. */
int fan_loop_reinit_after_fork(void);

#endif /* FAN2_RUNTIME_LOOP_H */
