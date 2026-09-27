/*
 * luafan.c — LuaFan v2 top-level module entry (luaopen_fan2).
 *
 * M0 scope: pure, event-loop-independent utilities so the build/test harness
 * can be validated end-to-end under ASan on arm1 before the runtime core lands.
 *   fan.version()         -> "2.0.0-dev"
 *   fan.gettime()         -> monotonic seconds (double)
 *   fan.data2hex(s)       -> upper-case hex string (v1 byte-for-byte); nil on non-string
 *   fan.hex2data(hex)     -> binary string (v1 permissive: len>>1 pairs; strtol semantics
 *                            silently map invalid hex digits to 0); nil on non-string
 *   fan.is_valid_utf8(s)  -> boolean (v1: nil/absent -> true)
 *   fan.sanitize_utf8(s)  -> string with invalid bytes replaced by U+FFFD (v1: EF BF BD).
 *                            Returns the input unchanged on the all-valid fast path.
 */
#include "platform.h"
#include "runtime/loop.h"
#include "runtime/coro.h"
#include "net/tcp.h"
#include "net/fifo.h"
#include "net/udp.h"
#include "net/dns.h"
#include "net/evdns.h"
#include "net/tls.h"
#include "net/zlib_wrap.h"
#include "net/httpd.h"
#include "net/websocket.h"
#include "net/http.h"
#include "codec/stream.h"
#include "codec/objectbuf.h"
#include "codec/json.h"
#if FAN_WITH_SQLITE3
#include "db/db_sqlite3.h"
#endif
#if FAN_WITH_MARIADB
#include "db/db_mariadb.h"
#endif

#include "sys/posix.h"
#include "sys/popen.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define FAN2_VERSION "2.0.0-dev"

/* Lua 5.1 compat: luaL_setfuncs / registration */
#if (LUA_VERSION_NUM >= 502)
#  define FAN_REGISTER(L, name, funcs)  do { luaL_newlib((L), (funcs)); } while (0)
#else
#  define FAN_REGISTER(L, name, funcs)  do { luaL_register((L), (name), (funcs)); } while (0)
#endif

static double fan_monotonic_seconds(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    }
#endif
    return (double)time(NULL);
}

static int l_version(lua_State *L) {
    lua_pushliteral(L, FAN2_VERSION);
    return 1;
}

static int l_gettime(lua_State *L) {
    lua_pushnumber(L, fan_monotonic_seconds());
    return 1;
}

static int l_data2hex(lua_State *L) {
    /* v1: non-string -> return 0 (nil, no error). Matches v1 luafan.c:206-208. */
    if (!lua_isstring(L, 1)) return 0;
    size_t len = 0;
    const char *s = lua_tolstring(L, 1, &len);
    /* v1 uses upper-case hex ("0123456789ABCDEF"). */
    static const char hexd[] = "0123456789ABCDEF";
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        luaL_addchar(&b, hexd[(c >> 4) & 0xf]);
        luaL_addchar(&b, hexd[c & 0xf]);
    }
    luaL_pushresult(&b);
    return 1;
}

static int hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* v2 hex2data: string-coercible input (v1's lua_isstring test, incl. numbers),
 * but STRICT parsing — v1's odd-length truncation and strtol "invalid -> 0"
 * fallbacks were silent-data-loss footguns and are rejected here. Empty input
 * returns "". Errors surface as (nil, message) so callers can branch. */
static int l_hex2data(lua_State *L) {
    if (!lua_isstring(L, 1)) return 0;   /* non-coercible -> nil */
    size_t len = 0;
    const char *s = lua_tolstring(L, 1, &len);
    if (len == 0) { lua_pushliteral(L, ""); return 1; }
    if (len % 2 != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "hex string length must be even");
        return 2;
    }
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (size_t i = 0; i < len; i += 2) {
        int hi = hexval((unsigned char)s[i]);
        int lo = hexval((unsigned char)s[i + 1]);
        if (hi < 0 || lo < 0) {
            lua_pushnil(L);
            lua_pushfstring(L, "invalid hex digit at byte %d", (int)(i + (hi < 0 ? 1 : 2)));
            return 2;
        }
        luaL_addchar(&b, (char)((hi << 4) | lo));
    }
    luaL_pushresult(&b);
    return 1;
}

/* Validate one UTF-8 sequence starting at s[i], len total. Returns the number
 * of bytes consumed (1..4), or 0 if invalid at this position. */
static size_t utf8_seq_len(const unsigned char *s, size_t i, size_t len) {
    unsigned char c = s[i];
    if (c < 0x80) return 1;
    size_t n;
    if ((c & 0xE0) == 0xC0) { n = 2; if (c < 0xC2) return 0; }
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) { n = 4; if (c > 0xF4) return 0; }
    else return 0;
    if (i + n > len) return 0;
    for (size_t k = 1; k < n; k++) {
        if ((s[i + k] & 0xC0) != 0x80) return 0;
    }
    /* reject overlong / surrogate / >U+10FFFF */
    if (n == 3 && c == 0xE0 && s[i + 1] < 0xA0) return 0;
    if (n == 3 && c == 0xED && s[i + 1] > 0x9F) return 0; /* surrogates */
    if (n == 4 && c == 0xF0 && s[i + 1] < 0x90) return 0;
    if (n == 4 && c == 0xF4 && s[i + 1] > 0x8F) return 0;
    return n;
}

static int l_is_valid_utf8(lua_State *L) {
    /* v1 accepts nil / absent as "" (always valid). */
    size_t len = 0;
    const unsigned char *s = (const unsigned char *)luaL_optlstring(L, 1, "", &len);
    size_t i = 0;
    while (i < len) {
        size_t n = utf8_seq_len(s, i, len);
        if (n == 0) { lua_pushboolean(L, 0); return 1; }
        i += n;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_sanitize_utf8(lua_State *L) {
    /* v1 accepts nil / absent as "" (luaL_optlstring). */
    size_t len = 0;
    const unsigned char *s = (const unsigned char *)luaL_optlstring(L, 1, "", &len);

    /* Fast path: single validating pass; return input unchanged when valid.
     * Lua strings are interned/immutable so pushvalue is zero-copy. */
    size_t i = 0;
    while (i < len) {
        size_t n = utf8_seq_len(s, i, len);
        if (n == 0) break;
        i += n;
    }
    if (i == len) {
        lua_pushvalue(L, 1);
        return 1;
    }

    /* Repair path: keep valid prefix, then emit U+FFFD (EF BF BD) for every
     * bad byte and advance by 1 (v1's `q += 1` fallback exactly). */
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addlstring(&b, (const char *)s, i);
    while (i < len) {
        size_t n = utf8_seq_len(s, i, len);
        if (n == 0) {
            luaL_addchar(&b, (char)0xEF);
            luaL_addchar(&b, (char)0xBF);
            luaL_addchar(&b, (char)0xBD);
            i += 1;
        } else {
            for (size_t k = 0; k < n; k++) luaL_addchar(&b, (char)s[i + k]);
            i += n;
        }
    }
    luaL_pushresult(&b);
    return 1;
}

/* ---- runtime: loop / loopbreak / spawn ------------------------------------ */

static int l_loop(lua_State *L) {
    (void)L;
    int rc = fan_loop_run();
    lua_pushinteger(L, rc);
    return 1;
}

static int l_loopbreak(lua_State *L) {
    (void)L;
    fan_loop_break();
    return 0;
}

/* fan.spawn(fn, ...) — create a coroutine running fn(...) and start it.
 * The coroutine is pinned in the registry across its first resume, then
 * released (the R17/R20 invariant): if it parks (yields) it re-pins itself
 * via the park primitive, so releasing here after resume is always safe. */
static int l_spawn(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int nargs = lua_gettop(L) - 1;

    lua_State *co = lua_newthread(L);   /* pushes the new thread onto L */
    /* pin the coroutine so GC cannot collect it across the resume */
    lua_pushvalue(L, -1);
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);                      /* pop the thread copy left by newthread push */

    /* move fn + args from L to co: copy them over, keeping originals for now */
    lua_pushvalue(L, 1);                /* fn */
    for (int i = 0; i < nargs; i++) lua_pushvalue(L, 2 + i);
    lua_xmove(L, co, nargs + 1);        /* fn + args -> co */

    fan_coro_resume(co, nargs);
    luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
    return 0;
}

/* ---- v1 top-level fan.* misc helpers -------------------------------------
 *
 * These live on the fan module for byte-for-byte v1 compat. Their v1 sources:
 *   fan.gettop         -> luafan.c:376 (returns lua_gettop on MAIN thread)
 *   fan.const(name)    -> luafan.c:484 (unique userdata sentinel with
 *                        __tostring "const: NAME" and __metatable=false)
 *   fan.open/close/getdtablesize -> luafan_posix.c:34/44/49 (thin syscall
 *                        wrappers; v1 pushes (ret, errno) on failure via
 *                        luafan_push_result). We match the (ret[, nil, err])
 *                        shape used by the rest of fan.posix: on success
 *                        return the integer result; on failure return
 *                        nil + a string error message + the errno number.
 */

/* fan.gettop() — v1 returns the depth of the process's MAIN Lua state's
 * stack (utlua_mainthread). v2 has no separate main-thread pointer at the
 * fan.* module level: fan.spawn creates coroutines, but coroutines share
 * this state's global registry, and there is no VM lock to acquire. We
 * simply report the current state's stack top, which is what v1 code
 * expects to inspect in a debugger REPL / hook and equals main-thread
 * gettop when called from the main state (the common case). */
static int l_gettop(lua_State *L) {
    lua_pushinteger(L, lua_gettop(L));
    return 1;
}

/* fan.const(name) — allocate a fresh zero-sized userdata and give it a
 * private metatable so `tostring(c) == "const: NAME"` and
 * `getmetatable(c) == false` (v1 __metatable is boolean false). Two calls
 * with the same name still return *distinct* userdata (identity is the
 * pointer): v1 sentinels are compared by identity, not by name, so this
 * matches the v1 semantics used to build unique end-of-stream / EOF-style
 * markers. */
static int l_const_tostring(lua_State *L) {
    /* upvalue 1 is the copied name string */
    lua_pushfstring(L, "const: %s", lua_tostring(L, lua_upvalueindex(1)));
    return 1;
}
static int l_const(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_newuserdata(L, 0);              /* the sentinel itself */
    lua_newtable(L);                    /* its private metatable */
    lua_pushstring(L, name);            /* copy name into VM (upvalue) */
    lua_pushcclosure(L, l_const_tostring, 1);
    lua_setfield(L, -2, "__tostring");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable"); /* getmetatable() -> false */
    lua_setmetatable(L, -2);
    return 1;
}

/* fan.open / fan.close / fan.getdtablesize — thin POSIX wrappers.
 * Contract: success -> integer return (fd for open, 0 for close, table
 * size for getdtablesize); failure -> nil, error string, errno number.
 * This is a *superset* of v1's push_result (which pushed (ret, errno)):
 * callers that only look at the first return see the same integer or nil,
 * so v1 code keeps working; callers that want the message get one for
 * free. */
static int push_syscall_err(lua_State *L, int saved_errno) {
    lua_pushnil(L);
    lua_pushstring(L, strerror(saved_errno));
    lua_pushinteger(L, saved_errno);
    return 3;
}
static int l_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int flags = (int)luaL_optinteger(L, 2, O_RDWR);
    mode_t mode = (mode_t)luaL_optinteger(L, 3, 0);
    int fd = open(path, flags, mode);
    if (fd < 0) return push_syscall_err(L, errno);
    lua_pushinteger(L, fd);
    return 1;
}
static int l_close(lua_State *L) {
    int fd = (int)luaL_checkinteger(L, 1);
    if (close(fd) < 0) return push_syscall_err(L, errno);
    lua_pushinteger(L, 0);
    return 1;
}
static int l_getdtablesize(lua_State *L) {
    /* Portable across glibc/musl/macOS: sysconf(_SC_OPEN_MAX). getdtablesize(2)
     * exists on BSD/glibc but not on musl. v1 uses FAN_GETDTABLESIZE() macro
     * that switches per platform; sysconf is universal and returns the same
     * value in all cases we care about. */
    long n = sysconf(_SC_OPEN_MAX);
    if (n < 0) return push_syscall_err(L, errno);
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static const luaL_Reg fan2lib[] = {
    {"version",        l_version},
    {"gettime",        l_gettime},
    {"data2hex",       l_data2hex},
    {"hex2data",       l_hex2data},
    {"is_valid_utf8",  l_is_valid_utf8},
    {"sanitize_utf8",  l_sanitize_utf8},
    {"loop",           l_loop},
    {"loopbreak",      l_loopbreak},
    {"spawn",          l_spawn},
    /* v1 top-level parity (M14.C-h) */
    {"gettop",         l_gettop},
    {"const",          l_const},
    {"open",           l_open},
    {"close",          l_close},
    {"getdtablesize",  l_getdtablesize},
    {NULL, NULL},
};

int luaopen_fan(lua_State *L) {
    FAN_REGISTER(L, "fan", fan2lib);
    lua_pushliteral(L, FAN2_VERSION);
    lua_setfield(L, -2, "_VERSION");
    fan_coro_register(L);   /* adds fan.sleep to the module table at -1 */
    fan_tcp_register(L);    /* adds fan.tcp to the module table at -1 */
    fan_fifo_register(L);   /* adds fan.fifo to the module table at -1 */
    fan_udp_register(L);    /* adds fan.udp to the module table at -1 */
    fan_dns_register(L);    /* adds fan.dns to the module table at -1 */
    fan_evdns_register(L);  /* adds fan.evdns (M11) */
    fan_tls_register(L);    /* adds fan.tls to the module table at -1 */
    fan_zlib_register(L);   /* adds fan.zlib to the module table at -1 */
    fan_httpd_register(L);  /* adds fan.httpd_c (M14.C) */
    fan_ws_register(L);     /* installs fan.ws.conn metatable (M14.D) */
    fan_http_register(L);   /* adds fan.http_c (M13.C) */
    fan_stream_register(L); /* adds fan.stream to the module table at -1 */
    fan_objectbuf_register(L); /* adds fan.objectbuf */
    fan_json_register(L);   /* adds fan.json */
#if FAN_WITH_SQLITE3
    fan_sqlite3_register(L); /* adds fan.sqlite3 */
#endif
#if FAN_WITH_MARIADB
    fan_mariadb_register(L); /* adds fan.mariadb */
#endif
    fan_posix_register(L);  /* adds fan.posix (POSIX process/CPU/net ifaces) */
    fan_popen_register(L);  /* adds fan.popen (M12) */
    return 1;
}

/* fan_clear_lua_states — aggregate teardown hook. Call this immediately
 * before `lua_close(L)` when embedding luafan2 so every module that caches
 * a main-thread pointer (tcp/httpd/http/udp/dns/fifo/websocket/popen) NULLs
 * it out. Paired with the `if (!main_L) return;` guard in
 * fan_coro_wake (runtime/coro.c) and the `if (!L)` early-returns in
 * server_accept_cb / httpd gencb / evdns callbacks, this closes the
 * lua_close → fan_loop_cleanup window against use-after-free from stray
 * libevent callbacks. Safe to call multiple times and safe before any
 * register() has run (each per-module clear is a plain assignment to
 * NULL, no allocation freed). */
void fan_clear_lua_states(void) {
    fan_tcp_clear_lua_state();
    fan_fifo_clear_lua_state();
    fan_udp_clear_lua_state();
    fan_dns_clear_lua_state();
    fan_httpd_clear_lua_state();
    fan_ws_clear_lua_state();
    fan_http_clear_lua_state();
    fan_popen_clear_lua_state();
    /* Modules without cached main state (evdns, tls, zlib, stream,
     * objectbuf, json, sqlite3, mariadb, posix) intentionally omitted —
     * they never resume coroutines from libevent callbacks. */
}
