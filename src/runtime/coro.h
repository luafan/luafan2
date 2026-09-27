/*
 * coro.h — LuaFan v2 coroutine park/resume primitives.
 *
 * The programming model: Lua code runs inside coroutines; a blocking-looking
 * call (fan.sleep, later socket recv, etc.) yields the coroutine, C arms a
 * libevent event, and the event callback resumes the coroutine when ready.
 *
 * Lifetime invariant (learned from v1 R17/R20): the coroutine MUST stay
 * referenced across lua_resume. We luaL_ref the thread when parking and only
 * luaL_unref AFTER the resume returns — never before — so a GC triggered by
 * another allocation cannot collect a coroutine that is about to run / running.
 */
#ifndef FAN2_RUNTIME_CORO_H
#define FAN2_RUNTIME_CORO_H

#include <lua.h>

/* Resume coroutine `co` (which was previously yielded) with `nargs` values
 * already pushed on co's stack. Handles the Lua 5.1/5.2-4 lua_resume ABI, logs
 * uncaught errors, and returns the raw lua_resume status. This is the single
 * entry point every event callback resumes through. */
int fan_coro_resume(lua_State *co, int nargs);

/* Park helper: pin the currently-running coroutine `L` in the registry so GC
 * cannot collect it while it is suspended, and return the registry ref. The
 * CALLER must immediately `return lua_yield(L, 0)` after calling this (yielding
 * cannot be wrapped in a helper). Returns LUA_NOREF and does nothing if called
 * on the main thread (caller should luaL_error in that case). */
int fan_coro_park(lua_State *L);

/* Wake helper: resume coroutine `co` with `nargs` values already on its stack,
 * then release the registry pin `ref` (the R17/R20 order: unref AFTER resume).
 * `main_L` owns the registry. */
void fan_coro_wake(lua_State *main_L, lua_State *co, int ref, int nargs);

/* Register fan.sleep and expose it on the module table at index -1.
 * Requires the loop + a valid main lua_State. */
void fan_coro_register(lua_State *L);

#endif /* FAN2_RUNTIME_CORO_H */
