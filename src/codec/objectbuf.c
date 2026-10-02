/*
 * codec/objectbuf.c — LuaFan objectbuf: native, v1-compatible wire codec.
 *
 * This is the public fan.objectbuf implementation. It follows the LuaFan v1
 * section/index protocol exactly (see tmp/luafan/src/objectbuf.c and the
 * fan/objectbuf/init.lua wrapper) so that v1 and v2 can exchange data in both
 * directions and share symbol tables.
 *
 * Wire format:
 *   flag byte = OR of:
 *     0x80 HAS_NUMBER   0x40 HAS_U30   0x20 HAS_STRING   0x08 HAS_TABLE
 *     if none of those bits is set the flag is a boolean (0 = false, 1 = true)
 *   each present section, in order number -> u30 -> string -> table:
 *     u30(count) followed by the encoded values
 *       number : 8-byte little-endian IEEE-754 double
 *       u30    : u30 varint (7-bit septets, LEB128 style)
 *       string : u30(length) + raw bytes
 *       table  : u30(total body length) + u30(array_n) + array_n * u30(ref)
 *                + (u30(key_ref) u30(value_ref)) repeated
 *   A builtin index dictionary starts at 2: 1 = false, 2 = true. Values are
 *   assigned increasing indices as their section is emitted; tables are
 *   assigned after strings. The root object is the first item of the LAST
 *   section that carries data (selection order table -> string -> u30 -> number).
 *
 * Symbol table (from objectbuf.symbol):
 *     { [1] = map (value -> index),
 *       [2] = map_vk (index -> value),
 *       [3] = index (first free index) }
 *   Indices are assigned in the order: sorted strings, sorted numbers,
 *   sorted u30 integers.
 */
#include "objectbuf.h"

#include <lauxlib.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OBJ_HAS_NUMBER 0x80
#define OBJ_HAS_U30    0x40
#define OBJ_HAS_STRING 0x20
#define OBJ_HAS_TABLE  0x08

#define OBJ_MAX_U30    4294967296.0   /* exclusive upper bound (2^32) */
#define OBJ_MAX_DEPTH  1000

/* ------------------------------------------------------------------ */
/* growable byte writer                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} wbuf;

static void wb_grow(wbuf *w, size_t extra)
{
    if (w->len + extra <= w->cap) return;
    if (w->cap == 0) w->cap = 64;
    while (w->cap < w->len + extra) {
        if (w->cap > (size_t)-1 / 2) { w->cap = w->len + extra; break; }
        w->cap *= 2;
    }
    uint8_t *nb = (uint8_t *)realloc(w->buf, w->cap);
    if (!nb) { w->cap = 0; return; }
    w->buf = nb;
}

static void wb_byte(wbuf *w, uint8_t b)
{
    wb_grow(w, 1);
    if (w->cap < w->len + 1) return;
    w->buf[w->len++] = b;
}

static void wb_raw(wbuf *w, const void *p, size_t n)
{
    if (n == 0) return;
    wb_grow(w, n);
    if (w->cap < w->len + n) return;
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void wb_u30(wbuf *w, uint32_t v)
{
    while (v >= 0x80) { wb_byte(w, (uint8_t)((v & 0x7f) | 0x80)); v >>= 7; }
    wb_byte(w, (uint8_t)v);
}

static void wb_d64(wbuf *w, double d)
{
    union { double d; uint64_t u; } c;
    c.d = d;
    for (int i = 0; i < 8; ++i) wb_byte(w, (uint8_t)(c.u >> (i * 8)));
}

static void wb_string(wbuf *w, const char *s, size_t n)
{
    wb_u30(w, (uint32_t)n);
    wb_raw(w, s, n);
}

/* ------------------------------------------------------------------ */
/* reader                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         pos;
} rbuf;

static int rb_byte(rbuf *r, uint8_t *v)
{
    if (r->pos >= r->len) return 0;
    *v = r->p[r->pos++];
    return 1;
}

static int rb_u30(rbuf *r, uint32_t *v)
{
    uint32_t x = 0;
    int shift = 0;
    uint8_t b;
    do {
        if (shift > 28 || !rb_byte(r, &b)) return 0;
        x |= (uint32_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    *v = x;
    return 1;
}

static int rb_d64(rbuf *r, double *v)
{
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) {
        uint8_t b;
        if (!rb_byte(r, &b)) return 0;
        x |= (uint64_t)b << (i * 8);
    }
    union { double d; uint64_t u; } c;
    c.u = x;
    *v = c.d;
    return 1;
}

static size_t rb_rem(const rbuf *r) { return r->len - r->pos; }

/* ------------------------------------------------------------------ */
/* collection pass (mirrors v1 packer)                                */
/* ------------------------------------------------------------------ */
typedef struct {
    int tables, numbers, u30s, strings;          /* lists */
    int table_map, number_map, u30_map, string_map;
    int n_tables, n_numbers, n_u30s, n_strings;
    int depth;
} pack_ctx;

static void pack_ctx_init(lua_State *L, pack_ctx *c)
{
    memset(c, 0, sizeof(*c));
    lua_newtable(L); c->tables     = lua_absindex(L, -1);
    lua_newtable(L); c->numbers    = lua_absindex(L, -1);
    lua_newtable(L); c->u30s       = lua_absindex(L, -1);
    lua_newtable(L); c->strings    = lua_absindex(L, -1);
    lua_newtable(L); c->number_map = lua_absindex(L, -1);
    lua_newtable(L); c->u30_map    = lua_absindex(L, -1);
    lua_newtable(L); c->string_map = lua_absindex(L, -1);
    lua_newtable(L); c->table_map  = lua_absindex(L, -1);
}

static int obj_is_u30(lua_Number v)
{
    return isfinite((double)v) && floor((double)v) == (double)v
        && v >= 0 && v < OBJ_MAX_U30;
}

static void pack_value(lua_State *L, pack_ctx *c, int idx);

static void pack_add(lua_State *L, int map, int list, int *count, int idx)
{
    ++*count;
    lua_pushvalue(L, idx);
    lua_rawseti(L, list, *count);
    lua_pushvalue(L, idx);
    lua_pushinteger(L, *count);
    lua_rawset(L, map);
}

static void pack_table(lua_State *L, pack_ctx *c, int idx)
{
    if (++c->depth > OBJ_MAX_DEPTH) {
        luaL_error(L, "objectbuf: table nesting too deep (> %d)", OBJ_MAX_DEPTH);
    }
    if (!lua_checkstack(L, 8)) {
        luaL_error(L, "objectbuf: cannot grow stack for nested table");
    }
    lua_pushvalue(L, idx);
    lua_rawget(L, c->table_map);
    if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        c->depth--;
        return;
    }
    lua_pop(L, 1);

    pack_add(L, c->table_map, c->tables, &c->n_tables, idx);

    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        pack_value(L, c, lua_gettop(L) - 1);
        pack_value(L, c, lua_gettop(L));
        lua_pop(L, 1);
    }
    c->depth--;
}

static void pack_number(lua_State *L, pack_ctx *c, int idx)
{
    lua_Number v = lua_tonumber(L, idx);
    int u30 = obj_is_u30(v);
    int map = u30 ? c->u30_map : c->number_map;
    lua_pushvalue(L, idx);
    lua_rawget(L, map);
    if (!lua_isnil(L, -1)) { lua_pop(L, 1); return; }
    lua_pop(L, 1);
    if (u30) pack_add(L, c->u30_map, c->u30s, &c->n_u30s, idx);
    else     pack_add(L, c->number_map, c->numbers, &c->n_numbers, idx);
}

static void pack_string(lua_State *L, pack_ctx *c, int idx)
{
    lua_pushvalue(L, idx);
    lua_rawget(L, c->string_map);
    if (!lua_isnil(L, -1)) { lua_pop(L, 1); return; }
    lua_pop(L, 1);
    pack_add(L, c->string_map, c->strings, &c->n_strings, idx);
}

static void pack_value(lua_State *L, pack_ctx *c, int idx)
{
    switch (lua_type(L, idx)) {
    case LUA_TTABLE:  pack_table(L, c, idx);  break;
    case LUA_TNUMBER: pack_number(L, c, idx); break;
    case LUA_TSTRING: pack_string(L, c, idx); break;
    case LUA_TBOOLEAN:
    case LUA_TNIL:
        break;
    default:
        luaL_error(L, "objectbuf: cannot serialise Lua %s",
                   lua_typename(L, lua_type(L, idx)));
    }
}

/* ------------------------------------------------------------------ */
/* encode                                                             */
/* ------------------------------------------------------------------ */
static int resolve_ref(lua_State *L, int sym_map, int index_map, int value_idx)
{
    if (sym_map) {
        lua_pushvalue(L, value_idx);
        lua_rawget(L, sym_map);
        if (!lua_isnil(L, -1)) {
            int r = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
            return r;
        }
        lua_pop(L, 1);
    }
    lua_pushvalue(L, value_idx);
    lua_rawget(L, index_map);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return -1; }
    int r = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return r;
}

static int l_encode(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) return luaL_error(L, "no argument.");
    int base = lua_gettop(L);

    if (lua_isboolean(L, 1)) {
        lua_pushlstring(L, lua_toboolean(L, 1) ? "\x01" : "\x00", 1);
        return 1;
    }

    int sym = lua_istable(L, 2) ? 2 : 0;

    pack_ctx c;
    pack_ctx_init(L, &c);
    pack_value(L, &c, 1);

    int index = 2;
    int sym_map = 0;
    if (sym) {
        lua_rawgeti(L, sym, 3);
        if (!lua_isnumber(L, -1)) {
            return luaL_error(L, "objectbuf: invalid symbol table (missing index).");
        }
        index = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (index < 2) {
            return luaL_error(L, "objectbuf: invalid symbol table (index too small).");
        }
        lua_rawgeti(L, sym, 1);
        if (!lua_istable(L, -1)) {
            return luaL_error(L, "objectbuf: invalid symbol table (missing map).");
        }
        sym_map = lua_absindex(L, -1);
    }

    lua_newtable(L);
    int index_map = lua_absindex(L, -1);
    lua_pushboolean(L, 0); lua_pushinteger(L, 1); lua_rawset(L, index_map);
    lua_pushboolean(L, 1); lua_pushinteger(L, 2); lua_rawset(L, index_map);

    wbuf out;
    memset(&out, 0, sizeof(out));
    wb_byte(&out, 0);                 /* flag placeholder */
    int flag = 0;

    /* numbers (non-u30) */
    if (c.n_numbers > 0) {
        wbuf sec; memset(&sec, 0, sizeof(sec));
        uint32_t real = 0;
        for (int i = 1; i <= c.n_numbers; ++i) {
            lua_rawgeti(L, c.numbers, i);
            int vtop = lua_gettop(L);
            int skip = 0;
            if (sym_map) {
                lua_pushvalue(L, vtop);
                lua_rawget(L, sym_map);
                skip = !lua_isnil(L, -1);
                lua_pop(L, 1);
            }
            if (!skip) {
                wb_d64(&sec, lua_tonumber(L, vtop));
                ++real;
                lua_pushvalue(L, vtop);
                lua_pushinteger(L, index + (int)real);
                lua_rawset(L, index_map);
            }
            lua_pop(L, 1);
        }
        if (real > 0) {
            flag |= OBJ_HAS_NUMBER;
            wb_u30(&out, real);
            wb_raw(&out, sec.buf, sec.len);
            index += (int)real;
        }
        free(sec.buf);
    }

    /* u30 integers */
    if (c.n_u30s > 0) {
        wbuf sec; memset(&sec, 0, sizeof(sec));
        uint32_t real = 0;
        for (int i = 1; i <= c.n_u30s; ++i) {
            lua_rawgeti(L, c.u30s, i);
            int vtop = lua_gettop(L);
            int skip = 0;
            if (sym_map) {
                lua_pushvalue(L, vtop);
                lua_rawget(L, sym_map);
                skip = !lua_isnil(L, -1);
                lua_pop(L, 1);
            }
            if (!skip) {
                wb_u30(&sec, (uint32_t)lua_tointeger(L, vtop));
                ++real;
                lua_pushvalue(L, vtop);
                lua_pushinteger(L, index + (int)real);
                lua_rawset(L, index_map);
            }
            lua_pop(L, 1);
        }
        if (real > 0) {
            flag |= OBJ_HAS_U30;
            wb_u30(&out, real);
            wb_raw(&out, sec.buf, sec.len);
            index += (int)real;
        }
        free(sec.buf);
    }

    /* strings */
    if (c.n_strings > 0) {
        wbuf sec; memset(&sec, 0, sizeof(sec));
        uint32_t real = 0;
        for (int i = 1; i <= c.n_strings; ++i) {
            lua_rawgeti(L, c.strings, i);
            int vtop = lua_gettop(L);
            int skip = 0;
            if (sym_map) {
                lua_pushvalue(L, vtop);
                lua_rawget(L, sym_map);
                skip = !lua_isnil(L, -1);
                lua_pop(L, 1);
            }
            if (!skip) {
                size_t slen = 0;
                const char *s = lua_tolstring(L, vtop, &slen);
                wb_string(&sec, s, slen);
                ++real;
                lua_pushvalue(L, vtop);
                lua_pushinteger(L, index + (int)real);
                lua_rawset(L, index_map);
            }
            lua_pop(L, 1);
        }
        if (real > 0) {
            flag |= OBJ_HAS_STRING;
            wb_u30(&out, real);
            wb_raw(&out, sec.buf, sec.len);
            index += (int)real;
        }
        free(sec.buf);
    }

    /* tables */
    if (c.n_tables > 0) {
        flag |= OBJ_HAS_TABLE;
        wb_u30(&out, (uint32_t)c.n_tables);
        for (int i = 1; i <= c.n_tables; ++i) {
            lua_rawgeti(L, c.tables, i);
            lua_pushinteger(L, index + i);
            lua_rawset(L, index_map);
        }
        for (int i = 1; i <= c.n_tables; ++i) {
            lua_rawgeti(L, c.tables, i);
            int tb = lua_gettop(L);

            wbuf body; memset(&body, 0, sizeof(body));
            uint32_t arr_n = 0;
            for (;;) {
                lua_rawgeti(L, tb, (lua_Integer)arr_n + 1);
                if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
                int r = resolve_ref(L, sym_map, index_map, lua_gettop(L));
                lua_pop(L, 1);
                wb_u30(&body, (uint32_t)r);
                ++arr_n;
            }
            lua_pushnil(L);
            while (lua_next(L, tb) != 0) {
                int vidx = lua_gettop(L);
                int kidx = vidx - 1;
                int is_arr = 0;
                if (lua_type(L, kidx) == LUA_TNUMBER) {
                    lua_Number nk = lua_tonumber(L, kidx);
                    lua_Integer ik = (lua_Integer)nk;
                    if ((lua_Number)ik == nk && ik >= 1 && (uint32_t)ik <= arr_n) {
                        is_arr = 1;
                    }
                }
                if (!is_arr) {
                    int kr = resolve_ref(L, sym_map, index_map, kidx);
                    int vr = resolve_ref(L, sym_map, index_map, vidx);
                    wb_u30(&body, (uint32_t)kr);
                    wb_u30(&body, (uint32_t)vr);
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);

            wbuf d2; memset(&d2, 0, sizeof(d2));
            wb_u30(&d2, arr_n);
            wb_u30(&out, (uint32_t)(d2.len + body.len));
            wb_raw(&out, d2.buf, d2.len);
            wb_raw(&out, body.buf, body.len);
            free(d2.buf);
            free(body.buf);
        }
    }

    /* scalar root fully resolved into the symbol table leaves flag at 0 */
    if (flag == 0) {
        int t = lua_type(L, 1);
        if (t == LUA_TNUMBER) {
            lua_Number v = lua_tonumber(L, 1);
            if (obj_is_u30(v)) {
                flag |= OBJ_HAS_U30;
                wb_u30(&out, 1);
                wb_u30(&out, (uint32_t)lua_tointeger(L, 1));
            } else {
                flag |= OBJ_HAS_NUMBER;
                wb_u30(&out, 1);
                wb_d64(&out, (double)v);
            }
        } else if (t == LUA_TSTRING) {
            size_t slen = 0;
            const char *s = lua_tolstring(L, 1, &slen);
            flag |= OBJ_HAS_STRING;
            wb_u30(&out, 1);
            wb_string(&out, s, slen);
        }
    }

    if (out.buf == NULL) {
        lua_settop(L, base);
        return luaL_error(L, "objectbuf: out of memory");
    }
    out.buf[0] = (uint8_t)flag;
    lua_pushlstring(L, (const char *)out.buf, out.len);
    free(out.buf);

    lua_replace(L, base + 1);
    lua_settop(L, base + 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* decode                                                             */
/* ------------------------------------------------------------------ */
static int push_ref(lua_State *L, int vk, int rev, uint32_t idx)
{
    if (vk) {
        lua_rawgeti(L, vk, (lua_Integer)idx);
        if (!lua_isnil(L, -1)) return 1;
        lua_pop(L, 1);
    }
    lua_rawgeti(L, rev, (lua_Integer)idx);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return 0; }
    return 1;
}

static int l_decode(lua_State *L)
{
    size_t n = 0;
    const char *data = luaL_checklstring(L, 1, &n);
    int sym = lua_istable(L, 2) ? 2 : 0;

    rbuf r = { (const uint8_t *)data, n, 0 };
    uint8_t flag;
    if (!rb_byte(&r, &flag)) {
        lua_pushnil(L);
        lua_pushliteral(L, "decode failed, empty input.");
        return 2;
    }
    if (flag == 0) { lua_pushboolean(L, 0); return 1; }
    if (flag == 1) { lua_pushboolean(L, 1); return 1; }

    lua_newtable(L);
    int rev = lua_absindex(L, -1);
    uint32_t index = 2;
    int vk = 0;

    if (sym) {
        lua_rawgeti(L, sym, 3);
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            lua_pushnil(L);
            lua_pushliteral(L, "decode failed, invalid symbol table (missing index).");
            return 2;
        }
        index = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_rawgeti(L, sym, 2);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_pushnil(L);
            lua_pushliteral(L, "decode failed, invalid symbol table (missing map).");
            return 2;
        }
        vk = lua_absindex(L, -1);
    }

    lua_pushboolean(L, 0); lua_rawseti(L, rev, 1);
    lua_pushboolean(L, 1); lua_rawseti(L, rev, 2);

    uint32_t last_top = index + 1;

    if (flag & OBJ_HAS_NUMBER) {
        uint32_t count;
        if (!rb_u30(&r, &count)) goto malformed;
        last_top = index + 1;
        for (uint32_t i = 0; i < count; ++i) {
            double v;
            if (!rb_d64(&r, &v)) goto malformed;
            lua_pushnumber(L, v);
            lua_rawseti(L, rev, ++index);
        }
    }

    if (flag & OBJ_HAS_U30) {
        uint32_t count;
        if (!rb_u30(&r, &count)) goto malformed;
        last_top = index + 1;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t v;
            if (!rb_u30(&r, &v)) goto malformed;
            lua_pushinteger(L, (lua_Integer)v);
            lua_rawseti(L, rev, ++index);
        }
    }

    if (flag & OBJ_HAS_STRING) {
        uint32_t count;
        if (!rb_u30(&r, &count)) goto malformed;
        last_top = index + 1;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t slen;
            if (!rb_u30(&r, &slen) || slen > rb_rem(&r)) goto malformed;
            lua_pushlstring(L, (const char *)(r.p + r.pos), slen);
            r.pos += slen;
            lua_rawseti(L, rev, ++index);
        }
    }

    if (flag & OBJ_HAS_TABLE) {
        uint32_t count;
        if (!rb_u30(&r, &count) || count > rb_rem(&r)) goto malformed;
        last_top = index + 1;
        for (uint32_t i = 1; i <= count; ++i) {
            lua_newtable(L);
            lua_rawseti(L, rev, index + i);
        }
        index += count;
        for (uint32_t i = 1; i <= count; ++i) {
            uint32_t blen;
            if (!rb_u30(&r, &blen) || blen > rb_rem(&r)) goto malformed;
            rbuf b = { r.p + r.pos, blen, 0 };
            r.pos += blen;

            lua_rawgeti(L, rev, index - count + i);
            uint32_t arr_n;
            if (!rb_u30(&b, &arr_n) || arr_n > rb_rem(&b)) goto malformed;
            for (uint32_t j = 1; j <= arr_n; ++j) {
                uint32_t vi;
                if (!rb_u30(&b, &vi)) goto malformed;
                if (!push_ref(L, vk, rev, vi)) goto malformed;
                lua_rawseti(L, -2, j);
            }
            while (b.pos < b.len) {
                uint32_t ki, vi;
                if (!rb_u30(&b, &ki) || !rb_u30(&b, &vi)) goto malformed;
                if (!push_ref(L, vk, rev, ki)) goto malformed;
                if (!push_ref(L, vk, rev, vi)) goto malformed;
                lua_rawset(L, -3);
            }
            lua_pop(L, 1);
        }
    }

    if (!push_ref(L, vk, rev, last_top)) {
        lua_pushnil(L);
        lua_pushliteral(L, "decode failed, root missing.");
        return 2;
    }
    lua_replace(L, 1);
    lua_settop(L, 1);
    return 1;

malformed:
    lua_settop(L, 0);
    lua_pushnil(L);
    lua_pushliteral(L, "decode failed, malformed objectbuf stream.");
    return 2;
}

/* ------------------------------------------------------------------ */
/* symbol                                                             */
/* ------------------------------------------------------------------ */
static void sort_list(lua_State *L, int list)
{
    lua_getglobal(L, "table");
    lua_getfield(L, -1, "sort");
    lua_pushvalue(L, list);
    lua_call(L, 1, 0);
    lua_pop(L, 1);
}

static int l_symbol(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) return luaL_error(L, "no argument.");
    int base = lua_gettop(L);

    pack_ctx c;
    pack_ctx_init(L, &c);
    pack_value(L, &c, 1);

    sort_list(L, c.strings);
    sort_list(L, c.numbers);
    sort_list(L, c.u30s);

    lua_newtable(L); int map = lua_absindex(L, -1);
    lua_newtable(L); int vk  = lua_absindex(L, -1);
    lua_pushboolean(L, 0); lua_pushinteger(L, 1); lua_rawset(L, map);
    lua_pushboolean(L, 1); lua_pushinteger(L, 2); lua_rawset(L, map);

    int index = 2;
    int lists[3] = { c.strings, c.numbers, c.u30s };
    for (int k = 0; k < 3; ++k) {
        size_t cnt = lua_rawlen(L, lists[k]);
        for (size_t i = 1; i <= cnt; ++i) {
            int ni = index + (int)i;
            lua_rawgeti(L, lists[k], (lua_Integer)i);
            lua_pushvalue(L, -1);
            lua_pushinteger(L, ni);
            lua_rawset(L, map);          /* map[value] = ni */
            lua_pushvalue(L, -1);
            lua_rawseti(L, vk, ni);      /* vk[ni] = value */
            lua_pop(L, 1);
        }
        index += (int)cnt;
    }

    lua_newtable(L);
    int sym = lua_absindex(L, -1);
    lua_pushvalue(L, map); lua_rawseti(L, sym, 1);
    lua_pushvalue(L, vk);  lua_rawseti(L, sym, 2);
    lua_pushinteger(L, index); lua_rawseti(L, sym, 3);
    lua_pushvalue(L, map); lua_setfield(L, sym, "map");
    lua_pushvalue(L, vk);  lua_setfield(L, sym, "map_vk");
    lua_pushinteger(L, index); lua_setfield(L, sym, "index");

    lua_replace(L, base + 1);
    lua_settop(L, base + 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* sample                                                             */
/* ------------------------------------------------------------------ */
static int cmp_count(lua_State *L)
{
    lua_getfield(L, 1, "count");
    lua_Number a = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "count");
    lua_Number b = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_pushboolean(L, a > b);
    return 1;
}

static void sample_count(lua_State *L, int counts, int list, int idx)
{
    lua_pushvalue(L, idx);
    lua_rawget(L, counts);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, idx); lua_setfield(L, -2, "key");
        lua_pushinteger(L, 1); lua_setfield(L, -2, "count");
        lua_pushvalue(L, -1);
        lua_rawseti(L, list, (lua_Integer)lua_rawlen(L, list) + 1);
        lua_pushvalue(L, idx);
        lua_pushvalue(L, -2);
        lua_rawset(L, counts);
        lua_pop(L, 1);
    } else {
        lua_getfield(L, -1, "count");
        lua_Integer cnt = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_pushinteger(L, cnt + 1);
        lua_setfield(L, -2, "count");
        lua_pop(L, 1);
    }
}

static void sample_walk(lua_State *L, int counts, int list, int idx, int depth)
{
    if (depth > OBJ_MAX_DEPTH) return;
    if (lua_type(L, idx) != LUA_TTABLE) {
        sample_count(L, counts, list, idx);
        return;
    }

    lua_Integer total = 0;
    for (;;) {
        lua_rawgeti(L, idx, total + 1);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        total++;
        sample_walk(L, counts, list, lua_gettop(L), depth + 1);
        lua_pop(L, 1);
    }

    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        int vidx = lua_gettop(L);
        int kidx = vidx - 1;
        int is_arr = 0;
        if (lua_type(L, kidx) == LUA_TNUMBER) {
            lua_Number nk = lua_tonumber(L, kidx);
            lua_Integer ik = (lua_Integer)nk;
            if ((lua_Number)ik == nk && ik >= 1 && ik <= total) is_arr = 1;
        }
        if (!is_arr) {
            sample_walk(L, counts, list, kidx, depth + 1);
            sample_walk(L, counts, list, vidx, depth + 1);
        }
        lua_pop(L, 1);
    }
}

static int l_sample(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) return luaL_error(L, "no argument.");
    int limit = (int)luaL_optinteger(L, 2, 127);
    if (limit < 0) limit = 0;
    int base = lua_gettop(L);

    lua_newtable(L); int counts = lua_absindex(L, -1);
    lua_newtable(L); int list   = lua_absindex(L, -1);
    sample_walk(L, counts, list, 1, 0);

    lua_getglobal(L, "table");
    lua_getfield(L, -1, "sort");
    lua_pushvalue(L, list);
    lua_pushcfunction(L, cmp_count);
    lua_call(L, 2, 0);
    lua_pop(L, 1);

    lua_newtable(L);
    int out = lua_absindex(L, -1);
    int n = (int)lua_rawlen(L, list);
    int take = n < limit ? n : limit;
    for (int i = 1; i <= take; ++i) {
        lua_rawgeti(L, list, i);
        lua_getfield(L, -1, "key");
        lua_rawseti(L, out, i);
        lua_pop(L, 1);
    }

    lua_replace(L, base + 1);
    lua_settop(L, base + 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* registration                                                       */
/* ------------------------------------------------------------------ */
void fan_objectbuf_register(lua_State *L)
{
    int fan = lua_gettop(L);   /* fan module table is on top */
    lua_newtable(L);
    int ob = lua_absindex(L, -1);

    lua_pushcfunction(L, l_encode); lua_setfield(L, ob, "encode");
    lua_pushcfunction(L, l_decode); lua_setfield(L, ob, "decode");
    lua_pushcfunction(L, l_symbol); lua_setfield(L, ob, "symbol");
    lua_pushcfunction(L, l_sample); lua_setfield(L, ob, "sample");

    lua_pushvalue(L, ob);
    lua_setfield(L, fan, "objectbuf");

    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    lua_pushvalue(L, ob);
    lua_setfield(L, -2, "fan.objectbuf");
    lua_pop(L, 2);

    lua_pop(L, 1);   /* pop ob, leave fan module table on top */
}
