/*
 * codec/stream.c — LuaFan v2 stream codec.
 *
 * A stream userdata owns a dynamic write buffer AND a read cursor over the
 * same bytes. Fresh streams start empty; stream.new(str) starts pre-filled
 * with a payload ready to decode. Writes always append at `len`; reads
 * advance a separate `pos` cursor from 0..len.
 *
 * Wire format details (all little-endian; byte-for-byte compatible with
 * LuaFan v1):
 *   u/i 8       1B
 *   u/i 16      2B  little-endian
 *   u/i 24      3B  little-endian
 *   u30 (LEB128) 1..5 bytes; see stream.h for the septet layout
 *   u/i 32      4B  little-endian
 *   D64         8B  IEEE-754 binary64 little-endian (float64)
 *   string      u30(len) + bytes
 *   bytes(n)    raw n bytes (caller-framed)
 *
 * Modes: the v1 module distinguished read vs write "modes"; v2's storage
 * layout doesn't need that distinction because writes always target `len`
 * and reads target `pos`. `prepare_get()` rewinds the read cursor to 0
 * (equivalent to v1's `bytearray_read_ready`), and `prepare_add()` shifts
 * the unread tail to the front + resets pos (equivalent to v1's
 * `bytearray_write_ready`).
 *
 * mark / reset: `mark()` snapshots the read cursor; `reset()` restores it.
 * This matches v1 semantics exactly (v1's `reset()` rewinds to the last
 * mark; the initial mark is 0, so on a fresh unmarked stream `reset()` is
 * the same as `pos(0)`). NOTE: the initial mark is intentionally 0 so
 * that pre-mark `reset()` calls still rewind to the start of the payload;
 * `empty()` also clears it, mirroring v1's `bytearray_empty`.
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
    size_t         mark;  /* saved read cursor for reset() (v1 semantics) */
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

/* U30 LEB128: 7-bit septets, low-bit-first, MSB=1 => continuation byte.
 * v1-compatible: `128` encodes as `0x80 0x01`, boundary at 128/16384/... */
static int l_add_u30(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_Integer v = luaL_checkinteger(L, 2);
    if (v < 0 || v > 0xffffffffLL) return luaL_error(L, "AddU30: out of range");
    uint32_t x = (uint32_t)v;
    unsigned char b[5];
    int n = 0;
    do {
        unsigned char byte = (unsigned char)(x & 0x7f);
        x >>= 7;
        if (x != 0) byte |= 0x80;
        b[n++] = byte;
    } while (x != 0 && n < 5);
    return put_bytes(L, s, b, n);
}

/* D64: IEEE-754 binary64 stored as 8 little-endian bytes. We use memcpy
 * to grab the raw bits (avoids strict-aliasing UB) and then write them in
 * a fixed LE order regardless of host endianness — v1 depended on host LE
 * layout, but that's the only supported target (x86_64, arm64 both LE). */
static int l_add_d64(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    double v = luaL_checknumber(L, 2);
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return put_le(L, s, bits, 8);
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

/* U30 LEB128 reader — matches v1 (up to 5 bytes / 32-bit value). */
static int l_get_u30(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    uint32_t v = 0;
    unsigned shift = 0;
    size_t start = s->pos;
    for (int i = 0; i < 5; i++) {
        const unsigned char *p = stream_read(s, 1);
        if (!p) {
            s->pos = start;   /* rewind — reader is atomic on short reads */
            lua_pushnil(L); lua_pushliteral(L, "eof"); return 2;
        }
        v |= ((uint32_t)(p[0] & 0x7f)) << shift;
        if ((p[0] & 0x80) == 0) {
            lua_pushinteger(L, (lua_Integer)v);
            return 1;
        }
        shift += 7;
    }
    /* 5 bytes with the last one still having the continuation bit set is a
     * malformed stream: v1 accepted it silently (loop exited on shift>30),
     * we match that lenient behaviour so v1 streams keep decoding. */
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

static int l_get_d64(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    const unsigned char *p = stream_read(s, 8);
    if (!p) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    uint64_t bits = read_le(p, 8);
    double v;
    memcpy(&v, &bits, sizeof(v));
    lua_pushnumber(L, v);
    return 1;
}

/* GetBytes(n?) — n omitted or nil means "all available bytes". Matches v1
 * (`luaL_optinteger` with -1 = "unlimited" → clamped to available). */
static int l_get_bytes(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    size_t avail = s->len - s->pos;
    size_t n;
    if (lua_isnoneornil(L, 2)) {
        n = avail;
    } else {
        lua_Integer ni = luaL_checkinteger(L, 2);
        if (ni < 0) return luaL_error(L, "GetBytes: negative length");
        n = (size_t)ni;
    }
    if (n == 0) { lua_pushliteral(L, ""); return 1; }
    if (n > avail) { lua_pushnil(L); lua_pushliteral(L, "eof"); return 2; }
    const unsigned char *p = s->buf + s->pos;
    s->pos += n;
    lua_pushlstring(L, (const char *)p, n);
    return 1;
}

/* TestBytes(n?) — peek without moving the read cursor. `n` omitted / nil /
 * negative means "all available" (v1's `luaL_optinteger(..., -1)` path).
 * Short reads clamp to available (never nil for non-empty streams). */
static int l_test_bytes(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    size_t avail = s->len - s->pos;
    size_t n;
    if (lua_isnoneornil(L, 2)) {
        n = avail;
    } else {
        lua_Integer ni = luaL_checkinteger(L, 2);
        if (ni < 0) n = avail; else n = (size_t)ni;
    }
    if (n == 0 || avail == 0) return 0;   /* v1: returns 0 args when nothing */
    if (n > avail) n = avail;
    lua_pushlstring(L, (const char *)(s->buf + s->pos), n);
    return 1;
}

/* GetString — v1 signature: on short read, return (nil, expected_length).
 * expected_length includes the u30 header if the header itself couldn't
 * be read, matching v1's `*buflen = bytearray_read_available(ba) + 1`. */
static int l_get_string(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    size_t start = s->pos;
    /* peek at the u30 length */
    lua_pushcfunction(L, l_get_u30);
    lua_pushvalue(L, 1);
    lua_call(L, 1, 2);
    if (lua_isnil(L, -2)) {
        /* short header read: rewind and mimic v1 (nil, available+1). */
        lua_pop(L, 2);
        s->pos = start;
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)((s->len - s->pos) + 1));
        return 2;
    }
    lua_Integer n = lua_tointeger(L, -2);
    lua_pop(L, 2);
    size_t avail = s->len - s->pos;
    if ((size_t)n > avail) {
        /* payload short: rewind fully; expected_length = declared len + u30
         * header width (which v1 reports as `len + diff`). */
        size_t hdr = s->pos - start;
        s->pos = start;
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)(n + (lua_Integer)hdr));
        return 2;
    }
    const unsigned char *p = s->buf + s->pos;
    s->pos += (size_t)n;
    lua_pushlstring(L, (const char *)p, (size_t)n);
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

/* mark() — snapshot the read cursor so a later reset() rewinds here. v1
 * returned true on success (only fails in v1 when not in read mode); v2
 * always allows it. */
static int l_mark(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    s->mark = s->pos;
    lua_pushboolean(L, 1);
    return 1;
}

/* reset() — rewind the read cursor to the last mark (initial mark = 0).
 * Matches v1 semantics: a fresh stream has mark=0 so reset() ~= pos(0);
 * once mark() is called, reset() rewinds to that saved offset. */
static int l_reset(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    s->pos = s->mark;
    lua_pushboolean(L, 1);
    return 1;
}

/* empty() — clear the stream: buffer contents dropped, cursors reset.
 * v1 keeps the allocation and only resets offset/total; we do the same. */
static int l_empty(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    s->len = 0;
    s->pos = 0;
    s->mark = 0;
    lua_pushboolean(L, 1);
    return 1;
}

/* readline() — pull the next \r, \n, or \r\n terminated line. Returns
 * (line, breakflag) on success, or nothing when no complete line is
 * buffered (in which case the read cursor is restored). Implemented in
 * C for performance and to avoid the metatable shim from the v1 Lua
 * module. */
static int l_readline(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    size_t start = s->pos;
    size_t end = s->len;
    if (start >= end) return 0;

    /* scan for \r or \n */
    size_t i = start;
    while (i < end) {
        unsigned char c = s->buf[i];
        if (c == '\r' || c == '\n') break;
        i++;
    }
    if (i >= end) {
        /* no terminator seen — v1 rewinds and returns nothing */
        s->pos = start;
        return 0;
    }

    /* line body is buf[start..i); terminator starts at i */
    unsigned char t0 = s->buf[i];
    const char *brk;
    size_t after;
    if (t0 == '\r' && i + 1 < end && s->buf[i + 1] == '\n') {
        brk = "\r\n"; after = i + 2;
    } else if (t0 == '\r') {
        brk = "\r"; after = i + 1;
    } else {
        brk = "\n"; after = i + 1;
    }
    lua_pushlstring(L, (const char *)(s->buf + start), i - start);
    lua_pushstring(L, brk);
    s->pos = after;
    return 2;
}

/* Luafan v1 compatibility: switch the stream to append mode. Unread bytes
 * are preserved at the front of the buffer and the read cursor is rewound,
 * so a following prepare_get() re-decodes the whole (unread + appended)
 * payload. Any pending mark is cleared (v1's bytearray_write_ready). */
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
    s->mark = 0;
    lua_pushboolean(L, 1);
    return 1;
}

/* Luafan v1 compatibility: switch to read mode, rewinding to the start of
 * the buffered payload so decoding sees all pending bytes. */
static int l_prepare_get(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    s->pos = 0;
    lua_pushboolean(L, 1);
    return 1;
}

/* __tostring — v1 shape: "<fan.stream available=%d>". */
static int l_tostring(lua_State *L) {
    stream_t *s = check_stream(L, 1);
    lua_pushfstring(L, "<fan.stream available=%d>", (int)(s->len - s->pos));
    return 1;
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
    /* writers */
    {"AddU8",  l_add_u8},  {"AddS8",  l_add_s8},
    {"AddU16", l_add_u16}, {"AddS16", l_add_s16},
    {"AddU24", l_add_u24}, {"AddS24", l_add_s24},
    {"AddU30", l_add_u30},
    {"AddU32", l_add_u32}, {"AddS32", l_add_s32},
    {"AddD64", l_add_d64},
    {"AddBytes",  l_add_bytes},
    {"AddString", l_add_string},
    /* v1 aliases: ABC = ActionScript Bytecode legacy naming, both map to U30. */
    {"AddABCU32", l_add_u30},
    {"AddABCS32", l_add_u30},

    /* readers */
    {"GetU8",  l_get_u8},  {"GetS8",  l_get_s8},
    {"GetU16", l_get_u16}, {"GetS16", l_get_s16},
    {"GetU24", l_get_u24}, {"GetS24", l_get_s24},
    {"GetU30", l_get_u30},
    {"GetU32", l_get_u32}, {"GetS32", l_get_s32},
    {"GetD64", l_get_d64},
    {"GetBytes",  l_get_bytes},
    {"GetString", l_get_string},
    {"TestBytes", l_test_bytes},
    /* v1 aliases */
    {"GetABCU32", l_get_u30},
    {"GetABCS32", l_get_u30},

    /* introspection + framing */
    {"package",     l_package},
    {"available",   l_available},
    {"len",         l_len},
    {"pos",         l_pos},
    {"mark",        l_mark},
    {"reset",       l_reset},
    {"empty",       l_empty},
    {"readline",    l_readline},
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
    lua_pushcfunction(L, l_tostring);
    lua_setfield(L, -2, "__tostring");
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
