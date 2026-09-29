/*
 * main.c — LuaFan v2 executable entry point (the `fan` program).
 *
 * v2 is delivered as TWO artefacts from one build:
 *   1) build/fan.so  — a Lua-loadable MODULE library that carries every v2 C
 *                      module.  When any host Lua interpreter does
 *                      `require("fan")` it dlopen()s this file with
 *                      RTLD_LOCAL, keeping its heavy deps (libcurl / libssl /
 *                      libevent / ...) scoped to fan.so's own namespace.
 *   2) build/fan     — this executable.  It owns lua_State creation and
 *                      teardown (and, later, per-thread States) and argv
 *                      parsing, but delegates every v2 API to fan.so.  It
 *                      does NOT link fan.so at compile time — instead it
 *                      dlopen()s it at startup, exactly like the host Lua
 *                      interpreter would, so it inherits the same
 *                      RTLD_LOCAL isolation.  No host-interpreter coupling.
 *
 * Result: `fan`'s DT_NEEDED list ends up being essentially { liblua, libdl,
 * libc } on Linux — no direct curl/ssl/event dependency.  All the heavy
 * libraries live behind fan.so.
 *
 * Usage:
 *   fan script.lua [args...]
 *   fan -e "chunk"
 *   fan -v
 */
#include "platform.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <dlfcn.h>
#include <libgen.h>   /* dirname() */
#include <limits.h>   /* PATH_MAX */
#include <unistd.h>   /* readlink() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef __APPLE__
#  include <mach-o/dyld.h> /* _NSGetExecutablePath */
#endif

#define FAN2_VERSION "2.0.0-dev"

/* ---------------------------------------------------------------------------
 * fan.so runtime binding.
 *
 * Every v2 API this file calls into lives in fan.so, resolved via dlopen +
 * dlsym at startup.  Keeping this table small makes it obvious what the
 * executable-side ABI contract with fan.so is:
 *
 *   luaopen_fan            — the module entry point Lua calls on require().
 *   fan_clear_lua_states   — see luafan.c: NULLs every module's cached
 *                            main-thread pointer BEFORE lua_close(L) so any
 *                            late libevent callback that fires between
 *                            lua_close() and fan_loop_cleanup() drops the
 *                            wake instead of dereferencing a dangling
 *                            lua_State.
 *   fan_loop_cleanup       — frees the shared event base after lua_close().
 * ------------------------------------------------------------------------- */
static struct {
    void *handle;
    int  (*luaopen_fan)(lua_State *);
    void (*fan_clear_lua_states)(void);
    void (*fan_loop_cleanup)(void);
} fan_so;

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

/* ---------------------------------------------------------------------------
 * fan.so loader.
 *
 * We look for fan.so next to the executable itself, not on the system
 * library search path — the layout `build/fan` + `build/fan.so` is the
 * intended install shape, and a stray fan.so somewhere else on LD_LIBRARY_PATH
 * would be surprising.  On Linux we read the true binary path from
 * /proc/self/exe; on macOS we ask the dynamic linker via _NSGetExecutablePath;
 * on any other Unix we fall back to argv[0] plus PATH-like resolution.
 *
 * Once the path is known we dlopen() it with RTLD_LAZY | RTLD_LOCAL, which is
 * the crucial part: RTLD_LOCAL keeps every symbol dragged in by fan.so
 * (libcurl / libssl / libevent / ...) scoped to that dlopen handle rather
 * than promoted into the executable's global namespace.  That is exactly the
 * isolation Lua's own package loader gives a `require("fan")` on any host
 * interpreter, and mirroring it here means the ./fan executable inherits
 * v1's dependency-isolation model too.
 * ------------------------------------------------------------------------- */

/* Fill `out` (size cap) with the absolute path of the running executable.
 * Returns 0 on success, -1 on failure (with an errno-ish message on stderr).
 * Uses OS-specific APIs first, then falls back to argv0 (which is enough
 * when the launcher was invoked with a full/relative path). */
static int resolve_self_path(const char *argv0, char *out, size_t cap) {
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n > 0) {
        out[n] = '\0';
        return 0;
    }
    /* readlink may fail on unusual mounts; fall through to argv0. */
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &sz) == 0) {
        /* _NSGetExecutablePath can return a symlinked path; realpath()
         * normalises it so dirname() yields the true build dir. */
        char resolved[PATH_MAX];
        if (realpath(out, resolved)) {
            strncpy(out, resolved, cap - 1);
            out[cap - 1] = '\0';
        }
        return 0;
    }
#endif
    if (argv0 && argv0[0]) {
        strncpy(out, argv0, cap - 1);
        out[cap - 1] = '\0';
        return 0;
    }
    fprintf(stderr, "fan: cannot determine executable path\n");
    return -1;
}

/* dlopen fan.so from the executable's directory and dlsym the 3 ABI symbols.
 * Returns 0 on success (fan_so.* populated), -1 on any failure. */
static int load_fan_so(const char *argv0) {
    char self[PATH_MAX];
    if (resolve_self_path(argv0, self, sizeof self) != 0) return -1;

    /* dirname() may modify its argument on some libcs (glibc's dirname(3) is
     * documented as such); work on a copy. */
    char dircopy[PATH_MAX];
    strncpy(dircopy, self, sizeof dircopy - 1);
    dircopy[sizeof dircopy - 1] = '\0';
    const char *dir = dirname(dircopy);

    char sopath[PATH_MAX];
    int n = snprintf(sopath, sizeof sopath, "%s/fan.so", dir ? dir : ".");
    if (n <= 0 || (size_t)n >= sizeof sopath) {
        fprintf(stderr, "fan: fan.so path too long\n");
        return -1;
    }

    fan_so.handle = dlopen(sopath, RTLD_LAZY | RTLD_LOCAL);
    if (!fan_so.handle) {
        fprintf(stderr, "fan: cannot load %s: %s\n", sopath, dlerror());
        return -1;
    }

    /* dlsym returns void*; cast through uintptr_t-equivalent to silence
     * ISO C's object-vs-function-pointer warning.  POSIX guarantees the
     * conversion is well-defined on any platform that has dlsym. */
    #define BIND(fld, name) do {                                            \
        void *sym = dlsym(fan_so.handle, name);                             \
        if (!sym) {                                                         \
            fprintf(stderr, "fan: %s missing in fan.so: %s\n",              \
                    name, dlerror());                                       \
            dlclose(fan_so.handle);                                         \
            fan_so.handle = NULL;                                           \
            return -1;                                                      \
        }                                                                   \
        *(void **)(&fan_so.fld) = sym;                                      \
    } while (0)

    BIND(luaopen_fan,          "luaopen_fan");
    BIND(fan_clear_lua_states, "fan_clear_lua_states");
    BIND(fan_loop_cleanup,     "fan_loop_cleanup");
    #undef BIND

    return 0;
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

    /* Resolve fan.so + dlsym its 3 ABI symbols (see fan_so table above).
     * Doing this AFTER luaL_openlibs (so package.* is initialised in case a
     * future extension wants to fall back to Lua's own C-module loader) but
     * BEFORE any luaL_requiref call that needs fan_so.luaopen_fan. */
    if (load_fan_so(argv[0]) != 0) {
        lua_close(L);
        return 1;
    }

    /* preload the built-in `fan` library so `require("fan")` returns it and the
     * global `fan` is also available for convenience */
    luaL_requiref(L, "fan", fan_so.luaopen_fan, 1); /* 1 = set global `fan` */
    lua_pop(L, 1);                                  /* pop the module left by requiref */

    /* ---- argument parsing ---- */
    const char *estr = NULL;
    int script_idx = -1;
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') { script_idx = i; break; }
        if (strcmp(a, "-v") == 0) {
            printf("LuaFan %s  (%s)\n", FAN2_VERSION, LUA_RELEASE);
            lua_close(L); fan_so.fan_loop_cleanup(); return 0;
        } else if (strcmp(a, "-h") == 0) {
            print_usage(argv[0]); lua_close(L); fan_so.fan_loop_cleanup(); return 0;
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

    fan_so.fan_clear_lua_states();  /* NULL cached main-thread pointers */
    lua_close(L);                   /* run finalizers */
    fan_so.fan_loop_cleanup();      /* then free the event base */
    return status == LUA_OK ? 0 : 1;
}
