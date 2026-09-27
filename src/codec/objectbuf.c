/*
 * codec/objectbuf.c — LuaFan v2 objectbuf codec (compact binary serialiser).
 *
 * Same wire format as the Lua reference implementation that briefly lived at
 * lua/fan/objectbuf.lua (commit 2a91ba9), rewritten in C for performance.
 * See lua/fan/objectbuf.lua in git history for the annotated design.
 *
 * Tags (single byte):
 *   0x00 NIL   0x01 FALSE   0x02 TRUE
 *   0x03 INT8  0x04 INT16   0x05 INT32   0x06 INT64
 *   0x07 FLOAT (8B little-endian double)
 *   0x08 STR8   1B u8 len + bytes
 *   0x09 STR16  2B u16 len (LE) + bytes
 *   0x0a STR32  4B u32 len (LE) + bytes
 *   0x0b TABLE  4B u32 arr_n (LE) + 4B u32 map_n (LE)
 *                + arr_n values + map_n (key, value) pairs
 *   0x0c REF    4B u32 refid (LE)  -> back-reference to prior table
 *
 * Cycle handling: encoder assigns a fresh refid to every table BEFORE
 * recursing; decoder registers the fresh table BEFORE descending. So
 * self-references and mutual cycles resolve to reference-identical tables.
 */
#include "objectbuf.h"
#include "../platform.h"
#include "../util/bytearray.h"

#include <lauxlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

enum {
    T_NIL = 0x00, T_FALSE = 0x01, T_TRUE = 0x02,
    T_INT8 = 0x03, T_INT16 = 0x04, T_INT32 = 0x05, T_INT64 = 0x06,
    T_FLOAT = 0x07,
    T_STR8 = 0x08, T_STR16 = 0x09, T_STR32 = 0x0a,
    T_TABLE = 0x0b, T_REF = 0x0c,
};

/* ---- encode -------------------------------------------------------------- */
/* enc_state: dynamic byte buffer + table ref map ("addr" -> id) stored in a
 * Lua table pinned at absolute index REF_TABLE on the encoder's stack. */
typedef struct {
    BYTEARRAY *out;      /* owned externally (GC-guarded userdata) */
    int      ref_idx;    /* absolute stack index of the ref-map table on L */
    uint32_t next_id;    /* next fresh refid (starts at 1) */
} enc_state_t;

static void append_byte(enc_state_t *e, unsigned char b) {
    bytearray_writebuffer(e->out, &b, 1);
}
static void append_le(enc_state_t *e, uint64_t v, int n) {
    unsigned char buf[8];
    for (int i = 0; i < n; i++) buf[i] = (v >> (i * 8)) & 0xff;
    bytearray_writebuffer(e->out, buf, (size_t)n);
}

static void encode_value(lua_State *L, int val_idx, enc_state_t *e);

static void encode_int(enc_state_t *e, lua_Integer v) {
    if (v >= -0x80 && v <= 0x7f) {
        append_byte(e, T_INT8);
        append_le(e, (uint64_t)(uint8_t)(int8_t)v, 1);
    } else if (v >= -0x8000 && v <= 0x7fff) {
        append_byte(e, T_INT16);
        append_le(e, (uint64_t)(uint16_t)(int16_t)v, 2);
    } else if (v >= -0x80000000LL && v <= 0x7fffffffLL) {
        append_byte(e, T_INT32);
        append_le(e, (uint64_t)(uint32_t)(int32_t)v, 4);
    } else {
        append_byte(e, T_INT64);
        append_le(e, (uint64_t)v, 8);
    }
}

static void encode_string(enc_state_t *e, const char *s, size_t n) {
    if (n <= 0xff) {
        append_byte(e, T_STR8);  append_le(e, n, 1);
    } else if (n <= 0xffff) {
        append_byte(e, T_STR16); append_le(e, n, 2);
    } else {
        append_byte(e, T_STR32); append_le(e, n, 4);
    }
    bytearray_writebuffer(e->out, s, n);
}

/* Assign or look up the refid for the table at val_idx.
 * Returns id (>0) and *existed=1 if it was already seen. */
static uint32_t table_ref_id(lua_State *L, int val_idx, enc_state_t *e,
                             int *existed) {
    /* key = lightuserdata of the table pointer (const void*) */
    lua_pushlightuserdata(L, (void *)lua_topointer(L, val_idx));
    lua_rawget(L, e->ref_idx);
    if (!lua_isnil(L, -1)) {
        uint32_t id = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        *existed = 1;
        return id;
    }
    lua_pop(L, 1);  /* pop nil */
    uint32_t id = e->next_id++;
    lua_pushlightuserdata(L, (void *)lua_topointer(L, val_idx));
    lua_pushinteger(L, (lua_Integer)id);
    lua_rawset(L, e->ref_idx);
    *existed = 0;
    return id;
}

static void encode_table(lua_State *L, int val_idx, enc_state_t *e) {
    int existed = 0;
    uint32_t id = table_ref_id(L, val_idx, e, &existed);
    if (existed) {
        append_byte(e, T_REF);
        append_le(e, id, 4);
        return;
    }
    /* count contiguous 1..arr_n and collect map keys */
    uint32_t arr_n = 0;
    while (1) {
        lua_rawgeti(L, val_idx, (lua_Integer)(arr_n + 1));
        int is_nil = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (is_nil) break;
        arr_n++;
    }
    /* first pass to count map entries */
    uint32_t map_n = 0;
    lua_pushnil(L);
    while (lua_next(L, val_idx) != 0) {
        int is_arr_slot = 0;
        if (lua_type(L, -2) == LUA_TNUMBER) {
            /* integer key in 1..arr_n counts as array */
            lua_Number kn = lua_tonumber(L, -2);
            lua_Integer ki = (lua_Integer)kn;
            if ((lua_Number)ki == kn && ki >= 1 && (uint32_t)ki <= arr_n) {
                is_arr_slot = 1;
            }
        }
        if (!is_arr_slot) map_n++;
        lua_pop(L, 1);
    }
    append_byte(e, T_TABLE);
    append_le(e, arr_n, 4);
    append_le(e, map_n, 4);
    /* array part */
    for (uint32_t i = 1; i <= arr_n; i++) {
        lua_rawgeti(L, val_idx, (lua_Integer)i);
        encode_value(L, lua_gettop(L), e);
        lua_pop(L, 1);
    }
    /* map part: reiterate and skip array slots */
    lua_pushnil(L);
    while (lua_next(L, val_idx) != 0) {
        int is_arr_slot = 0;
        if (lua_type(L, -2) == LUA_TNUMBER) {
            lua_Number kn = lua_tonumber(L, -2);
            lua_Integer ki = (lua_Integer)kn;
            if ((lua_Number)ki == kn && ki >= 1 && (uint32_t)ki <= arr_n) {
                is_arr_slot = 1;
            }
        }
        if (is_arr_slot) { lua_pop(L, 1); continue; }
        /* key at -2, value at -1 */
        encode_value(L, lua_gettop(L) - 1, e);
        encode_value(L, lua_gettop(L),     e);
        lua_pop(L, 1);
    }
}

static void encode_value(lua_State *L, int val_idx, enc_state_t *e) {
    int t = lua_type(L, val_idx);
    if (t == LUA_TNIL) {
        append_byte(e, T_NIL);
    } else if (t == LUA_TBOOLEAN) {
        append_byte(e, lua_toboolean(L, val_idx) ? T_TRUE : T_FALSE);
    } else if (t == LUA_TNUMBER) {
#if LUA_VERSION_NUM >= 503
        if (lua_isinteger(L, val_idx)) {
            encode_int(e, lua_tointeger(L, val_idx));
        } else {
            append_byte(e, T_FLOAT);
            union { double d; uint64_t u; } cv;
            cv.d = lua_tonumber(L, val_idx);
            append_le(e, cv.u, 8);
        }
#else
        double d = lua_tonumber(L, val_idx);
        lua_Integer i = (lua_Integer)d;
        if ((double)i == d) {
            encode_int(e, i);
        } else {
            append_byte(e, T_FLOAT);
            union { double d; uint64_t u; } cv;
            cv.d = d;
            append_le(e, cv.u, 8);
        }
#endif
    } else if (t == LUA_TSTRING) {
        size_t n = 0;
        const char *s = lua_tolstring(L, val_idx, &n);
        encode_string(e, s, n);
    } else if (t == LUA_TTABLE) {
        encode_table(L, val_idx, e);
    } else {
        luaL_error(L, "objectbuf: cannot serialise Lua %s",
                   lua_typename(L, t));
    }
}

/* GC helper: a userdata that owns the encode bytearray. If encode_value
 * raises via luaL_error, the ownership survives on the stack until Lua GC
 * runs __gc and reclaims the buffer. */
static int enc_gc(lua_State *L) {
    BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, "fan.objectbuf.enc");
    if (ba->buffer) bytearray_dealloc(ba);
    return 0;
}

static int l_encode(lua_State *L) {
    luaL_checkany(L, 1);
    lua_settop(L, 1);            /* value at slot 1 */
    lua_newtable(L);             /* ref map at slot 2 */

    /* stash the bytearray in a GC-guarded userdata at slot 3 */
    BYTEARRAY *guard = (BYTEARRAY *)lua_newuserdata(L, sizeof(BYTEARRAY));
    memset(guard, 0, sizeof(*guard));
    if (luaL_newmetatable(L, "fan.objectbuf.enc")) {
        lua_pushcfunction(L, enc_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    if (!bytearray_alloc(guard, 64)) {
        return luaL_error(L, "objectbuf.encode: OOM");
    }

    enc_state_t e;
    e.out = guard;                  /* share the guarded bytearray directly */
    e.ref_idx = 2;
    e.next_id = 1;
    encode_value(L, 1, &e);
    lua_pushlstring(L, (const char *)guard->buffer, guard->length);
    bytearray_dealloc(guard);       /* free now; enc_gc becomes a no-op */
    memset(guard, 0, sizeof(*guard));
    return 1;
}

/* ---- decode -------------------------------------------------------------- */
/* dec_state: source bytes + read cursor + refs table pinned at ref_idx.
 * refs[i] holds table objects registered at decode time (1-based). */
typedef struct {
    const unsigned char *s;
    size_t   n;
    size_t   pos;
    int      ref_idx;   /* absolute Lua stack index of refs table */
    uint32_t refs_len;  /* current count of registered tables */
} dec_state_t;

static const unsigned char *dec_read(dec_state_t *d, size_t n) {
    if (d->pos + n > d->n) return NULL;
    const unsigned char *p = d->s + d->pos;
    d->pos += n;
    return p;
}

static uint64_t dec_le(const unsigned char *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static int decode_value(lua_State *L, dec_state_t *d);

static int decode_table(lua_State *L, dec_state_t *d) {
    const unsigned char *ha = dec_read(d, 8);
    if (!ha) return luaL_error(L, "objectbuf.decode: eof in table header");
    uint32_t arr_n = (uint32_t)dec_le(ha,     4);
    uint32_t map_n = (uint32_t)dec_le(ha + 4, 4);
    lua_newtable(L);
    /* register BEFORE descending so self-references resolve to same table */
    d->refs_len++;
    lua_pushvalue(L, -1);
    lua_rawseti(L, d->ref_idx, (lua_Integer)d->refs_len);

    for (uint32_t i = 1; i <= arr_n; i++) {
        decode_value(L, d);
        lua_rawseti(L, -2, (lua_Integer)i);
    }
    for (uint32_t i = 0; i < map_n; i++) {
        decode_value(L, d);    /* key */
        decode_value(L, d);    /* value */
        lua_rawset(L, -3);
    }
    return 1;
}

static int decode_value(lua_State *L, dec_state_t *d) {
    const unsigned char *p = dec_read(d, 1);
    if (!p) return luaL_error(L, "objectbuf.decode: unexpected eof");
    unsigned char tag = p[0];
    switch (tag) {
    case T_NIL:   lua_pushnil(L); return 1;
    case T_FALSE: lua_pushboolean(L, 0); return 1;
    case T_TRUE:  lua_pushboolean(L, 1); return 1;
    case T_INT8: {
        p = dec_read(d, 1);
        if (!p) return luaL_error(L, "objectbuf.decode: eof INT8");
        lua_pushinteger(L, (lua_Integer)(int8_t)p[0]);
        return 1;
    }
    case T_INT16: {
        p = dec_read(d, 2);
        if (!p) return luaL_error(L, "objectbuf.decode: eof INT16");
        lua_pushinteger(L, (lua_Integer)(int16_t)dec_le(p, 2));
        return 1;
    }
    case T_INT32: {
        p = dec_read(d, 4);
        if (!p) return luaL_error(L, "objectbuf.decode: eof INT32");
        lua_pushinteger(L, (lua_Integer)(int32_t)dec_le(p, 4));
        return 1;
    }
    case T_INT64: {
        p = dec_read(d, 8);
        if (!p) return luaL_error(L, "objectbuf.decode: eof INT64");
        lua_pushinteger(L, (lua_Integer)(int64_t)dec_le(p, 8));
        return 1;
    }
    case T_FLOAT: {
        p = dec_read(d, 8);
        if (!p) return luaL_error(L, "objectbuf.decode: eof FLOAT");
        union { double d; uint64_t u; } cv;
        cv.u = dec_le(p, 8);
        lua_pushnumber(L, cv.d);
        return 1;
    }
    case T_STR8:
    case T_STR16:
    case T_STR32: {
        int nb = (tag == T_STR8) ? 1 : (tag == T_STR16) ? 2 : 4;
        const unsigned char *lp = dec_read(d, nb);
        if (!lp) return luaL_error(L, "objectbuf.decode: eof STR length");
        uint32_t len = (uint32_t)dec_le(lp, nb);
        const unsigned char *sp = dec_read(d, len);
        if (!sp && len > 0) return luaL_error(L, "objectbuf.decode: eof STR body");
        lua_pushlstring(L, (const char *)(sp ? sp : (const unsigned char *)""),
                        (size_t)len);
        return 1;
    }
    case T_TABLE:
        return decode_table(L, d);
    case T_REF: {
        p = dec_read(d, 4);
        if (!p) return luaL_error(L, "objectbuf.decode: eof REF id");
        uint32_t id = (uint32_t)dec_le(p, 4);
        if (id < 1 || id > d->refs_len)
            return luaL_error(L, "objectbuf.decode: bad back-reference %d", (int)id);
        lua_rawgeti(L, d->ref_idx, (lua_Integer)id);
        return 1;
    }
    default:
        return luaL_error(L, "objectbuf.decode: unknown tag %d", (int)tag);
    }
}

static int l_decode(lua_State *L) {
    size_t n = 0;
    const char *s = luaL_checklstring(L, 1, &n);
    lua_settop(L, 1);
    lua_newtable(L);            /* refs table at slot 2 */
    dec_state_t d;
    d.s = (const unsigned char *)s;
    d.n = n;
    d.pos = 0;
    d.ref_idx = 2;
    d.refs_len = 0;

    /* run under pcall so we can convert errors into (nil, err) return */
    lua_pushcfunction(L, (lua_CFunction)decode_value);
    lua_pushlightuserdata(L, &d);
    /* We cannot pcall a Lua-visible C function that takes a pointer;
     * do the pcall via a small closure. Instead: run inline, catching
     * errors via lua_pcall over an anonymous wrapper. */
    lua_pop(L, 2);

    /* run decode_value inside a protected call: wrap it in a helper. */
    /* Simpler: do it without pcall (any luaL_error will propagate as a raw
     * Lua error), then wrap with a Lua-side pcall. Since we want (nil,err)
     * on failure, we'll do pcall from Lua by exposing a raw variant and
     * wrapping in a Lua trampoline at register time. Here we just call
     * decode_value directly. */
    decode_value(L, &d);        /* pushes 1 result */
    if (d.pos != d.n) {
        return luaL_error(L, "objectbuf.decode: trailing bytes at %d",
                          (int)d.pos);
    }
    return 1;
}

/* Public wrapper that returns (value) on success or (nil, err) on failure.
 * We implement it as a Lua thin wrapper around raw_decode to keep pcall
 * semantics simple and portable. */
static const char *DECODE_WRAPPER =
    "local raw = ...\n"
    "return function(s)\n"
    "  local ok, v = pcall(raw, s)\n"
    "  if ok then return v end\n"
    "  return nil, v\n"
    "end";

void fan_objectbuf_register(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_encode);
    lua_setfield(L, -2, "encode");

    /* create decode = wrapper(raw_decode) */
    if (luaL_loadstring(L, DECODE_WRAPPER) != 0) {
        luaL_error(L, "objectbuf: wrapper load failed: %s", lua_tostring(L, -1));
    }
    lua_pushcfunction(L, l_decode);
    lua_call(L, 1, 1);
    lua_setfield(L, -2, "decode");

    lua_setfield(L, -2, "objectbuf");   /* fan.objectbuf = {...} */
}
