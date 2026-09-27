/*
 * coro.c — LuaFan v2 coroutine park/resume + fan.sleep.
 */
#include "coro.h"
#include "loop.h"
#include "../platform.h"

#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <event2/event.h>

/* ---- resume ABI shim (5.1 / 5.2 / 5.3 / 5.4) ------------------------------ */
static int coro_do_resume(lua_State *co, int nargs) {
#if LUA_VERSION_NUM >= 504
    int nres = 0;
    return lua_resume(co, NULL, nargs, &nres);
#elif LUA_VERSION_NUM >= 502
    return lua_resume(co, NULL, nargs);
#else
    return lua_resume(co, nargs);
#endif
}

int fan_coro_resume(lua_State *co, int nargs) {
    int status = coro_do_resume(co, nargs);
    if (status != LUA_OK && status != LUA_YIELD) {
        const char *err = lua_tostring(co, -1);
        fprintf(stderr, "[luafan2] coroutine error: %s\n", err ? err : "(non-string error)");
        /* pop the error so the dead coroutine's stack is clean */
        lua_pop(co, 1);
    }
    return status;
}

int fan_coro_park(lua_State *L) {
    int is_main = lua_pushthread(L);  /* pushes running thread; 1 if main */
    if (is_main) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    /* thread is on top of L's stack; pin it in the (global) registry */
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return ref;
}

void fan_coro_wake(lua_State *main_L, lua_State *co, int ref, int nargs) {
    fan_coro_resume(co, nargs);
    if (ref != LUA_NOREF) {
        luaL_unref(main_L, LUA_REGISTRYINDEX, ref);
    }
}
/* A parked sleep: holds the strong ref to the coroutine + its timer event. */
typedef struct {
    lua_State  *L;          /* main state (owns the registry) */
    lua_State  *co;         /* the parked coroutine */
    int         co_ref;     /* registry ref pinning `co` across the resume */
    struct event *timer;    /* one-shot timer event */
} sleep_ctx_t;

static void sleep_timeout_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    sleep_ctx_t *ctx = (sleep_ctx_t *)arg;
    lua_State *L = ctx->L;
    lua_State *co = ctx->co;
    int co_ref = ctx->co_ref;

    /* free the timer + ctx BEFORE resuming: the resumed coroutine may itself
     * sleep again and (re)allocate, and we must not leak this one-shot. */
    if (ctx->timer) event_free(ctx->timer);
    free(ctx);

    /* resume with zero return values; keep the ref alive ACROSS the resume,
     * then release it (R17/R20 invariant). */
    fan_coro_resume(co, 0);
    luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
    (void)co;
}

static void seconds_to_timeval(double sec, struct timeval *tv) {
    if (sec < 0) sec = 0;
    tv->tv_sec  = (long)sec;
    tv->tv_usec = (long)((sec - (double)tv->tv_sec) * 1e6);
    if (tv->tv_usec < 0) tv->tv_usec = 0;
    if (tv->tv_usec > 999999) tv->tv_usec = 999999;
}

static int l_sleep(lua_State *L) {
    double sec = luaL_checknumber(L, 1);

    /* must be called from within a coroutine (not the main thread) */
    int is_main = lua_pushthread(L);   /* pushes running thread; 1 if main */
    lua_pop(L, 1);
    if (is_main) {
        return luaL_error(L, "fan.sleep must be called from within a coroutine "
                             "(use fan.loop and a coroutine)");
    }

    struct event_base *base = fan_loop_current_base();
    if (!base) return luaL_error(L, "no event base available");

    sleep_ctx_t *ctx = (sleep_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return luaL_error(L, "out of memory");
    ctx->L = L;
    ctx->co = L;

    /* pin the coroutine in the registry so GC can't collect it while parked */
    lua_pushthread(L);
    ctx->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    ctx->timer = evtimer_new(base, sleep_timeout_cb, ctx);
    if (!ctx->timer) {
        luaL_unref(L, LUA_REGISTRYINDEX, ctx->co_ref);
        free(ctx);
        return luaL_error(L, "failed to create timer");
    }
    struct timeval tv;
    seconds_to_timeval(sec, &tv);
    if (evtimer_add(ctx->timer, &tv) != 0) {
        event_free(ctx->timer);
        luaL_unref(L, LUA_REGISTRYINDEX, ctx->co_ref);
        free(ctx);
        return luaL_error(L, "failed to arm timer");
    }

    return lua_yield(L, 0);
}

void fan_coro_register(lua_State *L) {
    /* module table is at top of stack */
    lua_pushcfunction(L, l_sleep);
    lua_setfield(L, -2, "sleep");
}
