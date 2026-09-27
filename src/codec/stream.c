/*
 * codec/stream.c — LuaFan v2 stream codec.
 * A stream userdata owns a dynamic write buffer AND a read cursor over the
 * same bytes. Fresh streams start empty; stream.new(str) starts pre-filled
 * with a payload to decode.
 *
 * Wire format details:
 *   u/i 8       1B  little-endian
 *   u/i 16      2B  little-endian
 *   u/i 24      3B  little-endian
 *   u30 (VLQ)   1/2/3/5 bytes; see stream.h
 *   u/i 32      4B  little-endian
 *   string      u30(len) + bytes
 *   bytes(n)    raw n bytes (caller-framed)
 */
#include "stream.h"
#include "../platform.h"

#include <lauxlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define STREAM_MT "fan.stream"

typedef struct {
    unsigned char *buf;   /* dynamic write buffer */
    size_t         len;   /* bytes written */
    size_t         cap;   /* allocated capacity */
    size_t         pos;   /* read cursor into buf[0..len] */
} stream_t;

static stream_t *check_stream(lua_State *L, int idx) {
    return (stream_t *)luaL_checkudata(L, idx, STREAM_MT);
}

static int stream_reserve(stream_t *s, size_t need) {
    if (s->len + need <= s->cap) return 0;
    size_t newcap = s->cap ? s->cap * 2 : 64;
    while (newcap < s->len + need) newcap *= 2;
    unsigned char *nb = (unsigned char *)realloc(s->buf, newcap);
    if (!nb) return -1;
    s->buf = nb; s->cap = newcap;
    return 0;
}

static int stream_write(stream_t *s, const void *data, size_t n) {
    if (stream_reserve(s, n) != 0) return -1;
    memcpy(s->buf + s->len, data, n);
    s->len += n;
    return 0;
}

/* Read n bytes at current cursor; returns pointer or NULL on short-read. */
static const unsigned char *stream_read(stream_t *s, size_t n) {
    if (s->pos + n > s->len) return NULL;
    const unsigned char *p = s->buf + s->pos;
    s->pos += n;
    return p;
}

/* ---- write helpers -------------------------------------------------------- */
static int put_bytes(lua_State *L, stream_t *s, const void *data, size_t n) {
    if (stream_write(s, data, n) != 0) return luaL_error(L, "stream: out of memory");
    return 0;
}

static int l_add_u8(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_Integer v = luaL_checkinteger(L, 2);
    if (v < 0 || v > 0xff) return luaL_error(L, "AddU8: value out of range");
    unsigned char b = (unsigned char)v; return put_bytes(L, s, &b, 1);
}
static int l_add_s8(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_Integer v = luaL_checkinteger(L, 2);
    if (v < -128 || v > 127) return luaL_error(L, "AddS8: value out of range");
    int8_t b = (int8_t)v; return put_bytes(L, s, &b, 1);
}

static int put_le(lua_State *L, stream_t *s, uint64_t v, int n) {
    unsigned char buf[8];
    for (int i = 0; i < n; i++) buf[i] = (v >> (i * 8)) & 0xff;
    return put_bytes(L, s, buf, n);
}

static int l_add_u16(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_Integer v = luaL_checkinteger(L, 2);
    if (v < 0 || v > 0xffff) return luaL_error(L, "AddU16: out of range");
    return put_le(L, s, (uint64_t)v, 2);
}
static int l_add_s16(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_Integer v = luaL_checkinteger(L, 2);
    if (v < -0x8000 || v > 0x7fff) return luaL_error(L, "AddS16: out of range");
    return put_le(L, s, (uint64_t)(uint16_t)(int16_t)v, 2);
}
static int l_add_u24(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_Integer v = luaL_checkinteger(L, 2);
    if (v < 0 || v > 0xffffff) return luaL_error(L, "AddU24: out of range");
    return put_le(L, s, (uint64_t)v, 3);
}
static int l_add_s24(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_Integer v = luaL_checkinteger(L, 2);
    if (v < -0x800000 || v > 0x7fffff) return luaL_error(L, "AddS24: out of range");
    return put_le(L, s, (uint64_t)(v & 0xffffff), 3);
}
static int l_add_u32(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_Integer v = luaL_checkinteger(L, 2);
    if (v < 0 || v > 0xffffffffLL) return luaL_error(L, "AddU32: out of range");
    return put_le(L, s, (uint64_t)v, 4);
}
static int l_add_s32(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_Integer v = luaL_checkinteger(L, 2);
    if (v < -0x80000000LL || v > 0x7fffffffLL) return luaL_error(L, "AddS32: out of range");
    return put_le(L, s, (uint64_t)(uint32_t)(int32_t)v, 4);
}

/* U30 VLQ: 1/2/3/5 bytes with 2-bit width tag in the header byte's MSBs. */
static int l_add_u30(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_Integer v = luaL_checkinteger(L, 2);
    if (v < 0 || v > 0xffffffffLL) return luaL_error(L, "AddU30: out of range");
    uint32_t x = (uint32_t)v;
    unsigned char b[5];
    if (x <= 0x3f) {
        b[0] = 0x00 | (unsigned char)x;
        return put_bytes(L, s, b, 1);
    } else if (x <= 0x3fff) {
        b[0] = 0x40 | (unsigned char)((x >> 8) & 0x3f);
        b[1] = (unsigned char)(x & 0xff);
        return put_bytes(L, s, b, 2);
    } else if (x <= 0x3fffff) {
        b[0] = 0x80 | (unsigned char)((x >> 16) & 0x3f);
        b[1] = (unsigned char)((x >> 8) & 0xff);
        b[2] = (unsigned char)(x & 0xff);
        return put_bytes(L, s, b, 3);
    } else {
        b[0] = 0xc0;   /* low 6 bits ignored on read for the 5-byte form */
        b[1] = (unsigned char)((x >> 24) & 0xff);
        b[2] = (unsigned char)((x >> 16) & 0xff);
        b[3] = (unsigned char)((x >>  8) & 0xff);
        b[4] = (unsigned char)( x        & 0xff);
        return put_bytes(L, s, b, 5);
    }
}

static int l_add_bytes(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    size_t n = 0;
    const char *p = luaL_checklstring(L, 2, &n);
    return put_bytes(L, s, p, n);
}

static int l_add_string(lua_State *L) {
    /* length-prefixed string: u30(#s) + bytes */
    stream_t *s = check_stream(L, 1);
    size_t n = 0;
    const char *p = luaL_checklstring(L, 2, &n);
    /* re-use l_add_u30 by pushing args and calling directly */
    lua_pushcfunction(L, l_add_u30);
    lua_pushvalue(L, 1); lua_pushinteger(L, (lua_Integer)n);
    lua_call(L, 2, 0);
    return put_bytes(L, s, p, n);
}

/* ---- read helpers --------------------------------------------------------- */
static int l_get_u8(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    const unsigned char *p = stream_read(s, 1);
    if (!p) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    lua_pushinteger(L, p[0]); return 1;
}
static int l_get_s8(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    const unsigned char *p = stream_read(s, 1);
    if (!p) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    lua_pushinteger(L, (int8_t)p[0]); return 1;
}
static uint64_t read_le(const unsigned char *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static int l_get_u16(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    const unsigned char *p = stream_read(s, 2);
    if (!p) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    lua_pushinteger(L, (lua_Integer)read_le(p, 2)); return 1;
}
static int l_get_s16(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    const unsigned char *p = stream_read(s, 2);
    if (!p) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    int16_t v = (int16_t)read_le(p, 2);
    lua_pushinteger(L, v); return 1;
}
static int l_get_u24(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    const unsigned char *p = stream_read(s, 3);
    if (!p) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    lua_pushinteger(L, (lua_Integer)read_le(p, 3)); return 1;
}
static int l_get_s24(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    const unsigned char *p = stream_read(s, 3);
    if (!p) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    int32_t v = (int32_t)read_le(p, 3);
    if (v & 0x800000) v |= 0xff000000;   /* sign-extend from 24 bits */
    lua_pushinteger(L, v); return 1;
}
static int l_get_u32(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    const unsigned char *p = stream_read(s, 4);
    if (!p) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    lua_pushinteger(L, (lua_Integer)read_le(p, 4)); return 1;
}
static int l_get_s32(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    const unsigned char *p = stream_read(s, 4);
    if (!p) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    int32_t v = (int32_t)read_le(p, 4);
    lua_pushinteger(L, v); return 1;
}

static int l_get_u30(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    const unsigned char *p = stream_read(s, 1);
    if (!p) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    unsigned char h = p[0];
    unsigned tag = (h >> 6) & 0x3;
    uint32_t v;
    if (tag == 0) {
        v = h & 0x3f;
    } else if (tag == 1) {
        const unsigned char *p2 = stream_read(s, 1);
        if (!p2) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
        v = ((uint32_t)(h & 0x3f) << 8) | p2[0];
    } else if (tag == 2) {
        const unsigned char *p2 = stream_read(s, 2);
        if (!p2) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
        v = ((uint32_t)(h & 0x3f) << 16) | ((uint32_t)p2[0] << 8) | p2[1];
    } else {
        const unsigned char *p2 = stream_read(s, 4);
        if (!p2) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
        v = ((uint32_t)p2[0] << 24) | ((uint32_t)p2[1] << 16)
          | ((uint32_t)p2[2] <<  8) |  (uint32_t)p2[3];
    }
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

static int l_get_bytes(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_Integer n = luaL_checkinteger(L, 2);
    if (n < 0) return luaL_error(L, "GetBytes: negative length");
    const unsigned char *p = stream_read(s, (size_t)n);
    if (!p && n > 0) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    lua_pushlstring(L, (const char *)(p ? p : (const unsigned char *)""), (size_t)n);
    return 1;
}

static int l_get_string(lua_State *L) {
    /* u30(len) + bytes */
    stream_t *s = check_stream(L, 1);
    /* inline read of u30 without recursion cost */
    lua_pushcfunction(L, l_get_u30);
    lua_pushvalue(L, 1);
    lua_call(L, 1, 2);
    if (lua_isnil(L, -2)) return 2;  /* propagate nil,"eof" */
    lua_Integer n = lua_tointeger(L, -2);
    lua_pop(L, 2);
    const unsigned char *p = stream_read(s, (size_t)n);
    if (!p && n > 0) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    lua_pushlstring(L, (const char *)(p ? p : (const unsigned char *)""), (size_t)n);
    return 1;
}

/* ---- introspection + framing --------------------------------------------- */
static int l_package(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_pushlstring(L, (const char *)s->buf, s->len);
    return 1;
}
static int l_available(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_pushinteger(L, (lua_Integer)(s->len - s->pos));
    return 1;
}
static int l_len(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_pushinteger(L, (lua_Integer)s->len);
    return 1;
}
static int l_pos(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        lua_Integer p = luaL_checkinteger(L, 2);
        if (p < 0 || (size_t)p > s->len) return luaL_error(L, "pos out of range");
        s->pos = (size_t)p;
        return 0;
    }
    lua_pushinteger(L, (lua_Integer)s->pos);
    return 1;
}
static int l_reset(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    s->pos = 0;
    return 0;
}

/* Luafan v1 compatibility: switch the stream to append mode. Unread bytes are
 * preserved at the front of the buffer and the read cursor is rewound, so a
 * following prepare_get() re-decodes the whole (unread + appended) payload. */
static int l_prepare_add(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    if (s->pos > 0) {
        if (s->pos < s->len) {
            memmove(s->buf, s->buf + s->pos, s->len - s->pos);
            s->len -= s->pos;
        } else {
            s->len = 0;
        }
        s->pos = 0;
    }
    return 0;
}

/* Luafan v1 compatibility: switch to read mode, rewinding to the start of the
 * buffered payload so decoding sees all pending bytes. */
static int l_prepare_get(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    s->pos = 0;
    return 0;
}

/* ---- new / gc ------------------------------------------------------------- */
static int l_new(lua_State *L) {
    size_t init_len = 0;
    const char *init = NULL;
    if (lua_isstring(L, 1)) init = luaL_checklstring(L, 1, &init_len);
    stream_t *s = (stream_t *)lua_newuserdata(L, sizeof(*s));
    memset(s, 0, sizeof(*s));
    if (init_len > 0) {
        s->buf = (unsigned char *)malloc(init_len);
        if (!s->buf) return luaL_error(L, "stream.new: out of memory");
        memcpy(s->buf, init, init_len);
        s->len = init_len; s->cap = init_len;
    }
    luaL_getmetatable(L, STREAM_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int stream_gc(lua_State *L) {
    stream_t *s = (stream_t *)luaL_checkudata(L, 1, STREAM_MT);
    free(s->buf); s->buf = NULL;
    return 0;
}

static const luaL_Reg methods[] = {
    {"AddU8",  l_add_u8},  {"AddS8",  l_add_s8},
    {"AddU16", l_add_u16}, {"AddS16", l_add_s16},
    {"AddU24", l_add_u24}, {"AddS24", l_add_s24},
    {"AddU30", l_add_u30},
    {"AddU32", l_add_u32}, {"AddS32", l_add_s32},
    {"AddBytes",  l_add_bytes},
    {"AddString", l_add_string},

    {"GetU8",  l_get_u8},  {"GetS8",  l_get_s8},
    {"GetU16", l_get_u16}, {"GetS16", l_get_s16},
    {"GetU24", l_get_u24}, {"GetS24", l_get_s24},
    {"GetU30", l_get_u30},
    {"GetU32", l_get_u32}, {"GetS32", l_get_s32},
    {"GetBytes",  l_get_bytes},
    {"GetString", l_get_string},

    {"package",   l_package},
    {"available", l_available},
    {"len",       l_len},
    {"pos",       l_pos},
    {"reset",     l_reset},
    {"prepare_add", l_prepare_add},
    {"prepare_get", l_prepare_get},
    {NULL, NULL},
};

void fan_stream_register(lua_State *L) {
    luaL_newmetatable(L, STREAM_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, stream_gc);
    lua_setfield(L, -2, "__gc");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, methods, 0);
#else
    luaL_register(L, NULL, methods);
#endif
    lua_pop(L, 1);  /* metatable */

    /* module table with new() */
    lua_newtable(L);
    lua_pushcfunction(L, l_new);
    lua_setfield(L, -2, "new");
    lua_setfield(L, -2, "stream");   /* fan.stream = { new = ... } */
}
