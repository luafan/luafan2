/*
 * main.c — LuaFan v2 executable entry point (the `fan` program).
 *
 * v2 is delivered as a self-contained executable, NOT a fan.so module:
 *   - it owns lua_State creation/teardown (and, later, per-thread States),
 *   - it statically links every v2 C module and preloads `fan` before the
 *     script runs, so scripts just `local fan = require("fan")`,
 *   - no dlopen symbol resolution, no host-interpreter coupling.
 *
 * Usage:
 *   fan script.lua [args...]
 *   fan -e "chunk"
 *   fan -v
 */
#include "platform.h"
#include "runtime/loop.h"

#include "luafan.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define FAN2_VERSION "2.0.0-dev"


/* Provided by luafan.c — clears every module's cached main-thread pointer.
 * MUST be called before lua_close(L) whenever fan_loop_run() may have armed
 * libevent events, so any late callback that fires between lua_close and
 * fan_loop_cleanup sees NULL and drops the wake instead of dereferencing a
 * dangling lua_State. See luafan.c for the full rationale. */
void fan_clear_lua_states(void);

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "LuaFan v2 (%s)\n"
        "usage: %s [options] [script [args...]]\n"
        "  -e chunk   execute a string chunk\n"
        "  -v         print version and exit\n"
        "  -h         this help\n",
        FAN2_VERSION, argv0);
}

/* message handler that adds a traceback to runtime errors */
static int msghandler(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (msg == NULL) msg = "(non-string error)";
    luaL_traceback(L, L, msg, 1);
    return 1;
}

/* pcall the function at the top of the stack with an installed msghandler */
static int docall(lua_State *L, int narg, int nres) {
    int base = lua_gettop(L) - narg;   /* function index */
    lua_pushcfunction(L, msghandler);
    lua_insert(L, base);
    int status = lua_pcall(L, narg, nres, base);
    lua_remove(L, base);
    return status;
}

static int report(lua_State *L, int status) {
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "fan: %s\n", msg ? msg : "(error with no message)");
        lua_pop(L, 1);
    }
    return status;
}

/* build the global `arg` table: arg[0]=script, arg[1..]=script args */
static void set_arg_table(lua_State *L, int argc, char **argv, int script_idx) {
    lua_newtable(L);
    if (script_idx >= 0 && script_idx < argc) {
        lua_pushstring(L, argv[script_idx]);
        lua_rawseti(L, -2, 0);
        int n = 1;
        for (int i = script_idx + 1; i < argc; i++) {
            lua_pushstring(L, argv[i]);
            lua_rawseti(L, -2, n++);
        }
    }
    lua_setglobal(L, "arg");
}

int main(int argc, char **argv) {
    /* Ignore SIGPIPE. A libevent bufferevent that writes to a socket whose
     * peer has closed will otherwise take the default SIGPIPE action and
     * terminate the process. Linux hides this in practice because our
     * write paths go through `send(..., MSG_NOSIGNAL)` / libevent's own
     * SIGPIPE guard; macOS has no MSG_NOSIGNAL and libevent does not set
     * SO_NOSIGPIPE per socket, so `fan` on macOS was dying with rc=141
     * during `test_tcp` (the "shutdown with pending output" scenario).
     * Ignoring SIGPIPE globally is the standard daemon idiom and lets
     * write() surface EPIPE to Lua instead of killing the process.
     *
     * Portable enough: POSIX signal() with SIG_IGN is defined on Linux,
     * macOS, and every BSD; on Windows this whole file is not built
     * (we do not ship a native Windows binary yet). */
    signal(SIGPIPE, SIG_IGN);

    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "fan: cannot create Lua state (out of memory)\n");
        return 1;
    }
    luaL_openlibs(L);

    /* preload the built-in `fan` library so `require("fan")` returns it and the
     * global `fan` is also available for convenience */
    luaL_requiref(L, "fan", luaopen_fan, 1); /* 1 = set global `fan` */
    lua_pop(L, 1);                           /* pop the module left by requiref */

    /* ---- argument parsing ---- */
    const char *estr = NULL;
    int script_idx = -1;
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') { script_idx = i; break; }
        if (strcmp(a, "-v") == 0) {
            printf("LuaFan %s  (%s)\n", FAN2_VERSION, LUA_RELEASE);
            lua_close(L); fan_loop_cleanup(); return 0;
        } else if (strcmp(a, "-h") == 0) {
            print_usage(argv[0]); lua_close(L); fan_loop_cleanup(); return 0;
        } else if (strcmp(a, "-e") == 0) {
            if (i + 1 >= argc) { print_usage(argv[0]); lua_close(L); return 1; }
            estr = argv[++i];
        } else if (strcmp(a, "--") == 0) {
            script_idx = (i + 1 < argc) ? i + 1 : -1;
            break;
        } else {
            fprintf(stderr, "fan: unknown option '%s'\n", a);
            print_usage(argv[0]); lua_close(L); return 1;
        }
    }

    set_arg_table(L, argc, argv, script_idx);

    int status = LUA_OK;

    if (estr) {
        status = luaL_loadstring(L, estr);
        if (status == LUA_OK) status = docall(L, 0, 0);
        report(L, status);
    }

    if (status == LUA_OK && script_idx >= 0) {
        status = luaL_loadfile(L, argv[script_idx]);
        if (status == LUA_OK) status = docall(L, 0, 0);
        report(L, status);
    }

    if (!estr && script_idx < 0) {
        /* no script and no -e: nothing to do (a REPL can come later) */
        print_usage(argv[0]);
    }

    fan_clear_lua_states();  /* NULL cached main-thread pointers */
    lua_close(L);            /* run finalizers */
    fan_loop_cleanup();      /* then free the event base */
    return status == LUA_OK ? 0 : 1;
}
