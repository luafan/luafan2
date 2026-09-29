/*
 * codec/json.c — LuaFan v2 JSON codec (RFC 8259) in pure C.
 *
 * Behaviour matches the Lua reference implementation that briefly lived at
 * lua/fan/json.lua (commit a58070a). See that file in git history for the
 * annotated design.
 *
 * Highlights:
 *   - array / object markers via metatables carrying __jsontype = "array" or
 *     "object", set by fan.json.array() / fan.json.object(). Empty containers
 *     round-trip unambiguously.
 *   - fan.json.null is a shared table sentinel; decode returns it for JSON
 *     null so it survives inside objects without dropping keys.
 *   - encode: UTF-8 verbatim, escapes control chars < 0x20 as \uXXXX plus
 *     the six single-char escapes and the JSON specials " \ .
 *   - decode: full RFC 8259 numbers, surrogate pairs, precise line/col
 *     error messages, trailing-garbage rejection.
 *   - Object keys are emitted sorted for deterministic output.
 *   - Integer subtype (Lua 5.3+) is preserved on encode; on decode we push
 *     integer when the literal has no '.' / 'e' / 'E' and fits.
 *
 * Empty untagged tables encode as {} (JavaScript intuition). Untagged tables
 * with contiguous 1..#t integer keys encode as arrays; otherwise as objects.
 */
#include "json.h"
#include "../platform.h"

#include <lauxlib.h>
#include <lua.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* -------- registry keys for shared metatables + null sentinel -------------- */
static const char *K_ARRAY_MT  = "fan.json.array.mt";
static const char *K_OBJECT_MT = "fan.json.object.mt";
static const char *K_NULL      = "fan.json.null";

/* Push the array metatable onto the stack (creates on first call). */
static void push_array_mt(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, K_ARRAY_MT);
    if (!lua_isnil(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushliteral(L, "array");
    lua_setfield(L, -2, "__jsontype");
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, K_ARRAY_MT);
}
static void push_object_mt(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, K_OBJECT_MT);
    if (!lua_isnil(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushliteral(L, "object");
    lua_setfield(L, -2, "__jsontype");
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, K_OBJECT_MT);
}
/* Null sentinel is created lazily in fan_json_register(); no separate helper.
 * (An earlier design had push_null() here; register handles it directly.) */

/* Return 1 if value at idx has __jsontype == want. */
static int has_jsontype(lua_State *L, int idx, const char *want) {
    if (!lua_getmetatable(L, idx)) return 0;
    lua_getfield(L, -1, "__jsontype");
    int match = 0;
    if (lua_type(L, -1) == LUA_TSTRING) {
        match = (strcmp(lua_tostring(L, -1), want) == 0);
    }
    lua_pop(L, 2);
    return match;
}

/* Return 1 if the value at idx is the fan.json.null sentinel. */
static int is_null(lua_State *L, int idx) {
    if (lua_type(L, idx) != LUA_TTABLE) return 0;
    lua_getfield(L, LUA_REGISTRYINDEX, K_NULL);
    int eq = lua_rawequal(L, idx, -1);
    lua_pop(L, 1);
    return eq;
}

/* =========================================================================
 * ENCODE
 * ========================================================================= */
typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
    /* pretty-print state */
    const char *indent;   /* NULL if compact */
    size_t      indent_len;
} enc_t;

static int enc_reserve(enc_t *e, size_t need) {
    if (e->len + need <= e->cap) return 0;
    size_t nc = e->cap ? e->cap * 2 : 128;
    while (nc < e->len + need) nc *= 2;
    char *nb = (char *)realloc(e->buf, nc);
    if (!nb) return -1;
    e->buf = nb; e->cap = nc;
    return 0;
}
static int enc_write(enc_t *e, const char *s, size_t n) {
    if (enc_reserve(e, n) != 0) return -1;
    memcpy(e->buf + e->len, s, n);
    e->len += n;
    return 0;
}
static int enc_char(enc_t *e, char c) { return enc_write(e, &c, 1); }
static int enc_str(enc_t *e, const char *s) { return enc_write(e, s, strlen(s)); }

static int enc_newline_indent(enc_t *e, int depth) {
    if (!e->indent) return 0;
    if (enc_char(e, '\n') != 0) return -1;
    for (int i = 0; i < depth; i++) {
        if (enc_write(e, e->indent, e->indent_len) != 0) return -1;
    }
    return 0;
}

/* Emit a JSON string (with surrounding quotes) escaping \, ", control chars. */
static int enc_string(enc_t *e, const char *s, size_t n) {
    if (enc_char(e, '"') != 0) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { if (enc_str(e, "\\\"") != 0) return -1; }
        else if (c == '\\') { if (enc_str(e, "\\\\") != 0) return -1; }
        else if (c == '\b') { if (enc_str(e, "\\b") != 0) return -1; }
        else if (c == '\f') { if (enc_str(e, "\\f") != 0) return -1; }
        else if (c == '\n') { if (enc_str(e, "\\n") != 0) return -1; }
        else if (c == '\r') { if (enc_str(e, "\\r") != 0) return -1; }
        else if (c == '\t') { if (enc_str(e, "\\t") != 0) return -1; }
        else if (c < 0x20) {
            char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c);
            if (enc_str(e, buf) != 0) return -1;
        } else {
            if (enc_char(e, (char)c) != 0) return -1;
        }
    }
    return enc_char(e, '"');
}

static int enc_number(lua_State *L, enc_t *e, int idx) {
#if LUA_VERSION_NUM >= 503
    if (lua_isinteger(L, idx)) {
        char buf[32];
        lua_Integer v = lua_tointeger(L, idx);
        int n = snprintf(buf, sizeof(buf), LUA_INTEGER_FMT, v);
        return enc_write(e, buf, (size_t)n);
    }
#endif
    lua_Number n = lua_tonumber(L, idx);
    /* reject NaN / Inf */
    if (n != n || n == HUGE_VAL || n == -HUGE_VAL) {
        lua_pushliteral(L, "cannot encode NaN/Inf as JSON");
        return -2;   /* signal Lua error to be raised by caller */
    }
    char buf[64];
    int nc = snprintf(buf, sizeof(buf), "%.17g", n);
    if (nc <= 0) return -1;
    /* trim trailing zeros in the mantissa (keep at least one digit after .). */
    char *dot = NULL, *exp = NULL;
    for (int i = 0; i < nc; i++) {
        if (buf[i] == '.' && !dot) dot = &buf[i];
        else if ((buf[i] == 'e' || buf[i] == 'E') && !exp) exp = &buf[i];
    }
    if (dot) {
        char *m_end = exp ? exp : &buf[nc];
        char *p = m_end - 1;
        while (p > dot + 1 && *p == '0') { *p = '\0'; p--; nc--; }
        if (exp && (p + 1) != exp) {
            memmove(p + 1, exp, strlen(exp) + 1);
            nc = (int)strlen(buf);
        }
    } else if (!exp) {
        /* integer-valued double without decimal: %.17g may print e.g. "10";
         * that's still valid JSON, leave as-is. */
    }
    return enc_write(e, buf, strlen(buf));
}

static int enc_value(lua_State *L, enc_t *e, int idx, int depth);

/* Encode array at absolute stack index idx. */
static int enc_array(lua_State *L, enc_t *e, int idx, int depth) {
    lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
    if (n == 0) return enc_str(e, "[]");
    if (enc_char(e, '[') != 0) return -1;
    for (lua_Integer i = 1; i <= n; i++) {
        if (i > 1) {
            if (enc_char(e, ',') != 0) return -1;
        }
        if (enc_newline_indent(e, depth + 1) != 0) return -1;
        lua_rawgeti(L, idx, i);
        int rc = enc_value(L, e, lua_gettop(L), depth + 1);
        lua_pop(L, 1);
        if (rc < 0) return rc;
    }
    if (enc_newline_indent(e, depth) != 0) return -1;
    return enc_char(e, ']');
}

/* Comparator for qsort of sorted-string-keys array. */
static int cmpstr(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

/* Encode object at absolute stack index idx (keys must be strings). */
static int enc_object(lua_State *L, enc_t *e, int idx, int depth) {
    /* Two passes: collect keys into a Lua sequence table for stable ordering. */
    lua_newtable(L);           /* keys array */
    int keys_idx = lua_gettop(L);
    lua_pushnil(L);
    int nkeys = 0;
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 3);     /* value, key, keys */
            lua_pushliteral(L, "JSON object keys must be strings");
            return -2;
        }
        lua_pop(L, 1);         /* value */
        lua_pushvalue(L, -1);  /* dup key */
        nkeys++;
        lua_rawseti(L, keys_idx, nkeys);
    }
    if (nkeys == 0) {
        lua_pop(L, 1);         /* keys */
        return enc_str(e, "{}");
    }
    /* sort keys: pull into C array of pointers, sort, use ordered output */
    const char **arr = (const char **)malloc(sizeof(char *) * (size_t)nkeys);
    if (!arr) { lua_pop(L, 1); return -1; }
    for (int i = 1; i <= nkeys; i++) {
        lua_rawgeti(L, keys_idx, i);
        arr[i - 1] = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    /* Note: keys strings are still referenced by keys_idx, so pointers stay
     * live for the duration of qsort/emit. */
    qsort(arr, (size_t)nkeys, sizeof(char *), cmpstr);

    if (enc_char(e, '{') != 0) { free((void *)arr); return -1; }
    for (int i = 0; i < nkeys; i++) {
        if (i > 0) {
            if (enc_char(e, ',') != 0) { free((void *)arr); return -1; }
        }
        if (enc_newline_indent(e, depth + 1) != 0) { free((void *)arr); return -1; }
        const char *k = arr[i];
        if (enc_string(e, k, strlen(k)) != 0) { free((void *)arr); return -1; }
        if (enc_char(e, ':') != 0) { free((void *)arr); return -1; }
        if (e->indent) {
            if (enc_char(e, ' ') != 0) { free((void *)arr); return -1; }
        }
        lua_getfield(L, idx, k);
        int rc = enc_value(L, e, lua_gettop(L), depth + 1);
        lua_pop(L, 1);
        if (rc < 0) { free((void *)arr); return rc; }
    }
    free((void *)arr);
    if (enc_newline_indent(e, depth) != 0) return -1;
    lua_pop(L, 1);   /* keys */
    return enc_char(e, '}');
}

/* Heuristic: untagged table with contiguous 1..#t integer keys => array. */
static int looks_like_array(lua_State *L, int idx) {
    lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
    if (n == 0) return 0;
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) != LUA_TNUMBER) {
            lua_pop(L, 2); return 0;
        }
        lua_Number kn = lua_tonumber(L, -2);
        lua_Integer ki = (lua_Integer)kn;
        if ((lua_Number)ki != kn || ki < 1 || ki > n) {
            lua_pop(L, 2); return 0;
        }
        count++;
        lua_pop(L, 1);
    }
    return count == (int)n;
}

static int enc_value(lua_State *L, enc_t *e, int idx, int depth) {
    int t = lua_type(L, idx);
    if (t == LUA_TNIL) return enc_str(e, "null");
    if (t == LUA_TBOOLEAN) {
        return enc_str(e, lua_toboolean(L, idx) ? "true" : "false");
    }
    if (t == LUA_TNUMBER) return enc_number(L, e, idx);
    if (t == LUA_TSTRING) {
        size_t n; const char *s = lua_tolstring(L, idx, &n);
        return enc_string(e, s, n);
    }
    if (t == LUA_TTABLE) {
        if (is_null(L, idx)) return enc_str(e, "null");
        if (has_jsontype(L, idx, "array"))  return enc_array(L, e, idx, depth);
        if (has_jsontype(L, idx, "object")) return enc_object(L, e, idx, depth);
        /* untagged */
        if (looks_like_array(L, idx)) return enc_array(L, e, idx, depth);
        return enc_object(L, e, idx, depth);
    }
    lua_pushfstring(L, "cannot encode a Lua %s to JSON", lua_typename(L, t));
    return -2;
}

static int l_encode(lua_State *L) {
    luaL_checkany(L, 1);
    /* opts (optional table): opts.indent = "  " */
    enc_t e;
    memset(&e, 0, sizeof(e));
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_getfield(L, 2, "indent");
        if (lua_type(L, -1) == LUA_TSTRING) {
            size_t n; const char *ind = lua_tolstring(L, -1, &n);
            /* stash indent bytes; since it's referenced in the Lua stack it stays
             * live for the duration of this call. */
            e.indent = ind; e.indent_len = n;
        }
        lua_pop(L, 1);
    }
    int rc = enc_value(L, &e, 1, 0);
    if (rc < 0) {
        free(e.buf);
        if (rc == -2) {
            /* error string was pushed by enc_value */
            return lua_error(L);
        }
        return luaL_error(L, "json.encode: out of memory");
    }
    lua_pushlstring(L, e.buf, e.len);
    free(e.buf);
    return 1;
}

/* =========================================================================
 * DECODE
 * ========================================================================= */
typedef struct {
    const char *s;
    size_t      n;
    size_t      i;
} dec_t;

static void skip_ws(dec_t *d) {
    while (d->i < d->n) {
        char c = d->s[d->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') d->i++;
        else break;
    }
}

/* Push nil + error string; return -1. */
static int dec_error(lua_State *L, dec_t *d, size_t at, const char *msg) {
    /* compute 1-based line/col */
    int line = 1;
    size_t last_nl = 0;
    for (size_t p = 0; p < at && p < d->n; p++) {
        if (d->s[p] == '\n') { line++; last_nl = p + 1; }
    }
    int col = (int)(at - last_nl + 1);
    lua_pushnil(L);
    lua_pushfstring(L, "json decode: %s at line %d col %d (pos %d)",
                    msg, line, col, (int)(at + 1));
    return -1;
}

static int decode_value(lua_State *L, dec_t *d);

/* Encode single Unicode codepoint (0..0x10FFFF) as UTF-8 into out (>=4 bytes).
 * Returns byte count. */
static int cp_to_utf8(uint32_t cp, unsigned char *out) {
    if (cp < 0x80) { out[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xC0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_string(lua_State *L, dec_t *d) {
    /* d->i points at opening " */
    d->i++;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    #define STR_FAIL(pos, msg) do {                                   \
        luaL_pushresult(&b);   /* materialise buffer to keep stack sane */ \
        lua_pop(L, 1);         /* drop the produced (partial) string */    \
        return dec_error(L, d, (pos), (msg));                          \
    } while (0)
    while (d->i < d->n) {
        unsigned char c = (unsigned char)d->s[d->i];
        if (c == '"') { d->i++; luaL_pushresult(&b); return 0; }
        if (c < 0x20) {
            STR_FAIL(d->i, "unescaped control character");
        }
        if (c != '\\') {
            luaL_addchar(&b, (char)c);
            d->i++;
            continue;
        }
        /* escape */
        if (d->i + 1 >= d->n) {
            STR_FAIL(d->i, "bad escape at EOF");
        }
        unsigned char esc = (unsigned char)d->s[d->i + 1];
        if      (esc == '"')  { luaL_addchar(&b, '"');  d->i += 2; }
        else if (esc == '\\') { luaL_addchar(&b, '\\'); d->i += 2; }
        else if (esc == '/')  { luaL_addchar(&b, '/');  d->i += 2; }
        else if (esc == 'b')  { luaL_addchar(&b, '\b'); d->i += 2; }
        else if (esc == 'f')  { luaL_addchar(&b, '\f'); d->i += 2; }
        else if (esc == 'n')  { luaL_addchar(&b, '\n'); d->i += 2; }
        else if (esc == 'r')  { luaL_addchar(&b, '\r'); d->i += 2; }
        else if (esc == 't')  { luaL_addchar(&b, '\t'); d->i += 2; }
        else if (esc == 'u') {
            if (d->i + 5 >= d->n) {
                STR_FAIL(d->i, "bad \\u escape");
            }
            int h[4];
            for (int k = 0; k < 4; k++) {
                h[k] = hexval((unsigned char)d->s[d->i + 2 + k]);
                if (h[k] < 0) STR_FAIL(d->i, "bad \\u escape");
            }
            uint32_t cp = ((uint32_t)h[0] << 12) | ((uint32_t)h[1] << 8)
                        | ((uint32_t)h[2] << 4)  |  (uint32_t)h[3];
            d->i += 6;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                /* need low surrogate */
                if (d->i + 5 >= d->n || d->s[d->i] != '\\' ||
                    d->s[d->i + 1] != 'u') {
                    STR_FAIL(d->i, "unpaired high surrogate");
                }
                int h2[4];
                for (int k = 0; k < 4; k++) {
                    h2[k] = hexval((unsigned char)d->s[d->i + 2 + k]);
                    if (h2[k] < 0) STR_FAIL(d->i, "bad \\u escape after surrogate");
                }
                uint32_t low = ((uint32_t)h2[0] << 12) | ((uint32_t)h2[1] << 8)
                             | ((uint32_t)h2[2] << 4)  |  (uint32_t)h2[3];
                if (low < 0xDC00 || low > 0xDFFF) {
                    STR_FAIL(d->i, "high surrogate not followed by low");
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                d->i += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                STR_FAIL(d->i, "orphan low surrogate");
            }
            unsigned char utf8[4];
            int nb = cp_to_utf8(cp, utf8);
            luaL_addlstring(&b, (const char *)utf8, (size_t)nb);
        } else {
            STR_FAIL(d->i, "bad escape");
        }
    }
    STR_FAIL(d->i, "unterminated string");
    #undef STR_FAIL
}

static int decode_number(lua_State *L, dec_t *d) {
    size_t start = d->i;
    int is_int = 1;
    if (d->i < d->n && d->s[d->i] == '-') d->i++;
    /* integer part */
    if (d->i >= d->n) return dec_error(L, d, start, "bad number");
    if (d->s[d->i] == '0') {
        d->i++;
    } else if (d->s[d->i] >= '1' && d->s[d->i] <= '9') {
        while (d->i < d->n && d->s[d->i] >= '0' && d->s[d->i] <= '9') d->i++;
    } else {
        return dec_error(L, d, start, "bad number");
    }
    /* fraction */
    if (d->i < d->n && d->s[d->i] == '.') {
        is_int = 0; d->i++;
        size_t frac0 = d->i;
        while (d->i < d->n && d->s[d->i] >= '0' && d->s[d->i] <= '9') d->i++;
        if (d->i == frac0) return dec_error(L, d, start, "bad number fraction");
    }
    /* exponent */
    if (d->i < d->n && (d->s[d->i] == 'e' || d->s[d->i] == 'E')) {
        is_int = 0; d->i++;
        if (d->i < d->n && (d->s[d->i] == '+' || d->s[d->i] == '-')) d->i++;
        size_t exp0 = d->i;
        while (d->i < d->n && d->s[d->i] >= '0' && d->s[d->i] <= '9') d->i++;
        if (d->i == exp0) return dec_error(L, d, start, "bad exponent");
    }
    /* parse */
    char tmp[64];
    size_t len = d->i - start;
    if (len >= sizeof(tmp)) {
        /* very long number: fall back to strtod on a malloc slice */
        char *big = (char *)malloc(len + 1);
        if (!big) return dec_error(L, d, start, "OOM");
        memcpy(big, d->s + start, len); big[len] = '\0';
        if (is_int) {
            char *endp = NULL;
            long long v = strtoll(big, &endp, 10);
            (void)v; (void)endp;
            lua_pushnumber(L, strtod(big, NULL));
        } else {
            lua_pushnumber(L, strtod(big, NULL));
        }
        free(big);
        return 0;
    }
    memcpy(tmp, d->s + start, len); tmp[len] = '\0';
    if (is_int) {
#if LUA_VERSION_NUM >= 503
        char *endp = NULL;
        long long v = strtoll(tmp, &endp, 10);
        if (endp && *endp == '\0') {
            lua_pushinteger(L, (lua_Integer)v);
        } else {
            lua_pushnumber(L, strtod(tmp, NULL));
        }
#else
        lua_pushnumber(L, strtod(tmp, NULL));
#endif
    } else {
        lua_pushnumber(L, strtod(tmp, NULL));
    }
    return 0;
}

static int decode_array(lua_State *L, dec_t *d) {
    d->i++;   /* past '[' */
    lua_newtable(L);
    push_array_mt(L);
    lua_setmetatable(L, -2);
    skip_ws(d);
    if (d->i < d->n && d->s[d->i] == ']') { d->i++; return 0; }
    lua_Integer idx = 1;
    while (1) {
        skip_ws(d);
        if (decode_value(L, d) != 0) {
            /* Error: nil + msg were pushed by decode_value. Remove the array
             * beneath them so caller sees only (nil, err). */
            lua_remove(L, -3);
            return -1;
        }
        lua_rawseti(L, -2, idx++);
        skip_ws(d);
        if (d->i >= d->n) return dec_error(L, d, d->i, "unterminated array");
        char c = d->s[d->i];
        if (c == ',') { d->i++; continue; }
        if (c == ']') { d->i++; return 0; }
        return dec_error(L, d, d->i, "expected ',' or ']' in array");
    }
}

static int decode_object(lua_State *L, dec_t *d) {
    d->i++;   /* past '{' */
    lua_newtable(L);
    push_object_mt(L);
    lua_setmetatable(L, -2);
    skip_ws(d);
    if (d->i < d->n && d->s[d->i] == '}') { d->i++; return 0; }
    while (1) {
        skip_ws(d);
        if (d->i >= d->n || d->s[d->i] != '"') {
            return dec_error(L, d, d->i, "expected string key in object");
        }
        if (decode_string(L, d) != 0) {
            lua_remove(L, -3);   /* remove object beneath (nil,err) */
            return -1;
        }
        skip_ws(d);
        if (d->i >= d->n || d->s[d->i] != ':') {
            /* pop key */
            lua_pop(L, 1);
            return dec_error(L, d, d->i, "expected ':' after object key");
        }
        d->i++;
        skip_ws(d);
        if (decode_value(L, d) != 0) {
            /* stack: obj, key, nil, err  -> remove key and obj */
            lua_remove(L, -3);   /* remove key */
            lua_remove(L, -3);   /* remove obj */
            return -1;
        }
        /* stack: obj, key, value */
        lua_rawset(L, -3);
        skip_ws(d);
        if (d->i >= d->n) return dec_error(L, d, d->i, "unterminated object");
        char c = d->s[d->i];
        if (c == ',') { d->i++; continue; }
        if (c == '}') { d->i++; return 0; }
        return dec_error(L, d, d->i, "expected ',' or '}' in object");
    }
}

static int decode_value(lua_State *L, dec_t *d) {
    skip_ws(d);
    if (d->i >= d->n) return dec_error(L, d, d->i, "unexpected end of input");
    char c = d->s[d->i];
    if (c == '"') return decode_string(L, d);
    if (c == '{') return decode_object(L, d);
    if (c == '[') return decode_array(L, d);
    if (c == '-' || (c >= '0' && c <= '9')) return decode_number(L, d);
    /* literals: true / false / null */
    if (c == 't' && d->i + 4 <= d->n && memcmp(d->s + d->i, "true", 4) == 0) {
        d->i += 4; lua_pushboolean(L, 1); return 0;
    }
    if (c == 'f' && d->i + 5 <= d->n && memcmp(d->s + d->i, "false", 5) == 0) {
        d->i += 5; lua_pushboolean(L, 0); return 0;
    }
    if (c == 'n' && d->i + 4 <= d->n && memcmp(d->s + d->i, "null", 4) == 0) {
        d->i += 4;
        lua_getfield(L, LUA_REGISTRYINDEX, K_NULL);
        return 0;
    }
    return dec_error(L, d, d->i, "unexpected character");
}

static int l_decode(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushnil(L);
        lua_pushliteral(L, "json.decode: input must be string");
        return 2;
    }
    size_t n; const char *s = lua_tolstring(L, 1, &n);
    dec_t d = { s, n, 0 };
    if (decode_value(L, &d) != 0) {
        /* nil + err already pushed */
        return 2;
    }
    /* trailing garbage check */
    skip_ws(&d);
    if (d.i < d.n) {
        lua_pop(L, 1);   /* pop the value */
        lua_pushnil(L);
        lua_pushfstring(L, "json.decode: trailing garbage at pos %d",
                        (int)(d.i + 1));
        return 2;
    }
    return 1;
}

/* =========================================================================
 * PUBLIC HELPERS: array(), object(), null, is_array, is_object
 * ========================================================================= */
static int l_array(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        lua_newtable(L);
    } else {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_settop(L, 1);
    }
    push_array_mt(L);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_object(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        lua_newtable(L);
    } else {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_settop(L, 1);
    }
    push_object_mt(L);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_is_array(lua_State *L) {
    lua_pushboolean(L,
        lua_type(L, 1) == LUA_TTABLE && has_jsontype(L, 1, "array"));
    return 1;
}
static int l_is_object(lua_State *L) {
    lua_pushboolean(L,
        lua_type(L, 1) == LUA_TTABLE && has_jsontype(L, 1, "object"));
    return 1;
}

/* json.is_nonempty_string(v) -> boolean.
 * True iff v is a Lua string with length > 0.  Convenience for the very
 * common "did the caller pass a real, non-empty string field" check on
 * decoded JSON bodies.  A missing key would surface as nil (type != string
 * -> false); an empty string "" from `{"name":""}` also returns false. */
static int l_is_nonempty_string(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushboolean(L, 0);
        return 1;
    }
    size_t len;
    lua_tolstring(L, 1, &len);
    lua_pushboolean(L, len > 0);
    return 1;
}

/* json.is_present(v) -> boolean.
 * True iff v is neither Lua nil nor the fan.json.null sentinel table.
 *
 * Because fan.json.decode ALWAYS represents JSON null as the sentinel
 * (fan.json has no `enable_null` toggle — that v1 knob is intentionally
 * absent from v2), a plain `if body.field then ... end` check treats
 * "user wrote null" and "user wrote 42" the same when the app cares.
 * is_present distinguishes them:
 *
 *   body = json.decode('{"a": null, "b": 1}')
 *   body.a  == nil                 -- false, it's the null sentinel
 *   json.is_present(body.a)        -- false
 *   json.is_present(body.b)        -- true
 *   json.is_present(body.missing)  -- false (nil)
 */
static int l_is_present(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, !is_null(L, 1));
    return 1;
}

/* null.__tostring => "null" */
static int null_tostring(lua_State *L) {
    lua_pushliteral(L, "null");
    return 1;
}

void fan_json_register(lua_State *L) {
    /* module table */
    lua_newtable(L);

    /* build shared metatables + null sentinel with correct __tostring */
    push_array_mt(L);  lua_pop(L, 1);
    push_object_mt(L); lua_pop(L, 1);

    /* null sentinel: create fresh (push_null in this file leaves __tostring
     * unset; do it properly here). */
    lua_getfield(L, LUA_REGISTRYINDEX, K_NULL);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);                                /* null table */
        lua_newtable(L);                                /* its metatable */
        lua_pushliteral(L, "null"); lua_setfield(L, -2, "__jsontype");
        lua_pushcfunction(L, null_tostring); lua_setfield(L, -2, "__tostring");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, K_NULL);
    }
    lua_setfield(L, -2, "null");

    lua_pushcfunction(L, l_encode);    lua_setfield(L, -2, "encode");
    lua_pushcfunction(L, l_decode);    lua_setfield(L, -2, "decode");
    lua_pushcfunction(L, l_array);     lua_setfield(L, -2, "array");
    lua_pushcfunction(L, l_object);    lua_setfield(L, -2, "object");
    lua_pushcfunction(L, l_is_array);  lua_setfield(L, -2, "is_array");
    lua_pushcfunction(L, l_is_object); lua_setfield(L, -2, "is_object");
    lua_pushcfunction(L, l_is_nonempty_string);
    lua_setfield(L, -2, "is_nonempty_string");
    lua_pushcfunction(L, l_is_present); lua_setfield(L, -2, "is_present");

    lua_setfield(L, -2, "json");
}
