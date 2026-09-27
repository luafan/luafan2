/*
 * zlib_wrap.c — LuaFan v2 zlib deflate/inflate primitives.
 * See zlib_wrap.h for API.
 *
 * The Lua bindings (l_deflate_raw / l_inflate_raw) are thin wrappers
 * over fan_zlib_deflate_raw_c / fan_zlib_inflate_raw_c so websocket.c
 * (permessage-deflate) and any other C caller reach the same code path
 * without going through the Lua stack.
 */
#include "zlib_wrap.h"
#include "../platform.h"

#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>

#if FAN_WITH_ZLIB

#include <zlib.h>

int fan_zlib_available(void) { return 1; }

int fan_zlib_deflate_raw_c(const char *in, size_t inlen,
                           int level, int sync,
                           char **out, size_t *outlen,
                           const char **errmsg) {
    if (out) *out = NULL;
    if (outlen) *outlen = 0;
    if (level != Z_DEFAULT_COMPRESSION && (level < 0 || level > 9)) {
        if (errmsg) *errmsg = "level must be -1 or in 0..9";
        return -1;
    }

    z_stream s;
    memset(&s, 0, sizeof(s));
    /* windowBits = -MAX_WBITS -> raw deflate (no zlib header/trailer) */
    int rc = deflateInit2(&s, level, Z_DEFLATED, -MAX_WBITS, 8,
                          Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) {
        if (errmsg) *errmsg = "deflateInit2 failed";
        return -1;
    }

    /* upper bound: deflateBound plus a little slack for the sync marker. */
    uLong bound = deflateBound(&s, (uLong)inlen) + 64;
    unsigned char *buf = (unsigned char *)malloc(bound);
    if (!buf) {
        deflateEnd(&s);
        if (errmsg) *errmsg = "out of memory";
        return -1;
    }

    s.next_in = (Bytef *)(uintptr_t)in;
    s.avail_in = (uInt)inlen;
    s.next_out = buf;
    s.avail_out = (uInt)bound;

    int flush = sync ? Z_SYNC_FLUSH : Z_FINISH;
    int expected = sync ? Z_OK : Z_STREAM_END;
    /* For Z_SYNC_FLUSH with empty input zlib emits a single empty stored
     * block ending in 00 00 FF FF; the peer needs it to detect the flush. */
    rc = deflate(&s, flush);
    if (rc != expected && !(sync && rc == Z_BUF_ERROR)) {
        deflateEnd(&s);
        free(buf);
        if (errmsg) *errmsg = "deflate did not finish";
        return -1;
    }
    if (out)    *out = (char *)buf;
    if (outlen) *outlen = (size_t)s.total_out;
    deflateEnd(&s);
    return 0;
}

int fan_zlib_inflate_raw_c(const char *in, size_t inlen,
                           char **out, size_t *outlen,
                           const char **errmsg) {
    if (out) *out = NULL;
    if (outlen) *outlen = 0;

    z_stream s;
    memset(&s, 0, sizeof(s));
    int rc = inflateInit2(&s, -MAX_WBITS);   /* raw inflate */
    if (rc != Z_OK) {
        if (errmsg) *errmsg = "inflateInit2 failed";
        return -1;
    }

    /* incremental output buffer, doubles on demand up to a hard ceiling */
    size_t cap = inlen * 4 + 64;
    const size_t HARD_MAX = 64 * 1024 * 1024;  /* 64 MiB decompressed cap */
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) {
        inflateEnd(&s);
        if (errmsg) *errmsg = "out of memory";
        return -1;
    }
    size_t used = 0;

    s.next_in = (Bytef *)(uintptr_t)in;
    s.avail_in = (uInt)inlen;

    while (1) {
        if (used == cap) {
            size_t newcap = cap * 2;
            if (newcap > HARD_MAX) {
                inflateEnd(&s); free(buf);
                if (errmsg) *errmsg = "decompressed size exceeds cap";
                return -1;
            }
            unsigned char *nb = (unsigned char *)realloc(buf, newcap);
            if (!nb) {
                inflateEnd(&s); free(buf);
                if (errmsg) *errmsg = "out of memory";
                return -1;
            }
            buf = nb; cap = newcap;
        }
        s.next_out = buf + used;
        s.avail_out = (uInt)(cap - used);
        rc = inflate(&s, Z_NO_FLUSH);
        used = cap - s.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR && s.avail_in == 0) {
            /* need more input but caller supplied all it had */
            break;
        }
        if (rc != Z_OK) {
            inflateEnd(&s); free(buf);
            if (errmsg) *errmsg = "inflate failed";
            return -1;
        }
    }

    if (out)    *out = (char *)buf;
    if (outlen) *outlen = used;
    inflateEnd(&s);
    return 0;
}

/* fan.zlib.deflate_raw(data [, level [, sync]]) -> string or nil,err */
static int l_deflate_raw(lua_State *L) {
    size_t inlen = 0;
    const char *in = luaL_checklstring(L, 1, &inlen);
    int level = (int)luaL_optinteger(L, 2, Z_DEFAULT_COMPRESSION);
    int sync = lua_toboolean(L, 3);

    char *out = NULL; size_t outlen = 0; const char *err = NULL;
    if (fan_zlib_deflate_raw_c(in, inlen, level, sync, &out, &outlen, &err) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "deflate failed");
        return 2;
    }
    lua_pushlstring(L, out, outlen);
    free(out);
    return 1;
}

/* fan.zlib.inflate_raw(data) -> string or nil,err */
static int l_inflate_raw(lua_State *L) {
    size_t inlen = 0;
    const char *in = luaL_checklstring(L, 1, &inlen);
    char *out = NULL; size_t outlen = 0; const char *err = NULL;
    if (fan_zlib_inflate_raw_c(in, inlen, &out, &outlen, &err) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "inflate failed");
        return 2;
    }
    lua_pushlstring(L, out, outlen);
    free(out);
    return 1;
}

#else /* !FAN_WITH_ZLIB */

int fan_zlib_available(void) { return 0; }

int fan_zlib_deflate_raw_c(const char *in, size_t inlen,
                           int level, int sync,
                           char **out, size_t *outlen,
                           const char **errmsg) {
    (void)in; (void)inlen; (void)level; (void)sync;
    if (out) *out = NULL;
    if (outlen) *outlen = 0;
    if (errmsg) *errmsg = "zlib not compiled in (build with -DFAN_WITH_ZLIB=ON)";
    return -1;
}
int fan_zlib_inflate_raw_c(const char *in, size_t inlen,
                           char **out, size_t *outlen,
                           const char **errmsg) {
    (void)in; (void)inlen;
    if (out) *out = NULL;
    if (outlen) *outlen = 0;
    if (errmsg) *errmsg = "zlib not compiled in (build with -DFAN_WITH_ZLIB=ON)";
    return -1;
}

static int l_deflate_raw(lua_State *L) {
    lua_pushnil(L);
    lua_pushliteral(L, "zlib not compiled in (build with -DFAN_WITH_ZLIB=ON)");
    return 2;
}
static int l_inflate_raw(lua_State *L) {
    lua_pushnil(L);
    lua_pushliteral(L, "zlib not compiled in (build with -DFAN_WITH_ZLIB=ON)");
    return 2;
}

#endif /* FAN_WITH_ZLIB */

static int l_available(lua_State *L) {
    lua_pushboolean(L, fan_zlib_available());
    return 1;
}

static const luaL_Reg zlib_funcs[] = {
    {"available",    l_available},
    {"deflate_raw",  l_deflate_raw},
    {"inflate_raw",  l_inflate_raw},
    {NULL, NULL},
};

void fan_zlib_register(lua_State *L) {
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, zlib_funcs, 0);
#else
    luaL_register(L, NULL, zlib_funcs);
#endif
    lua_pushboolean(L, fan_zlib_available());
    lua_setfield(L, -2, "enabled");
    lua_setfield(L, -2, "zlib");
}
