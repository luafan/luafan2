/*
 * db/sqlite3.c — LuaFan v2 SQLite3 binding.
 *
 * Two userdata types:
 *   fan.sqlite3.db     -> sqlite3*
 *   fan.sqlite3.stmt   -> sqlite3_stmt* with a back-pointer to its db
 *
 * All errors surface as (nil, "sqlite3: ...") return pairs from methods
 * that can fail at runtime. Programming errors (wrong arg type) raise via
 * luaL_error.
 */
#include "../platform.h"

#include <lauxlib.h>
#include <sqlite3.h>
#include "db_sqlite3.h"         /* our own registration prototype */
#include <stdint.h>
#include <string.h>

#define DB_MT   "fan.sqlite3.db"
#define STMT_MT "fan.sqlite3.stmt"

typedef struct {
    sqlite3 *db;   /* NULL after close */
} db_t;

typedef struct {
    sqlite3_stmt *stmt; /* NULL after finalize */
    int db_ref;         /* registry ref to owning db userdata (pins it alive) */
} stmt_t;

/* ---- helpers ------------------------------------------------------------- */
static db_t *check_db(lua_State *L, int idx) {
    return (db_t *)luaL_checkudata(L, idx, DB_MT);
}
static stmt_t *check_stmt(lua_State *L, int idx) {
    return (stmt_t *)luaL_checkudata(L, idx, STMT_MT);
}

static int push_sqlite_error(lua_State *L, sqlite3 *db, const char *ctx) {
    lua_pushnil(L);
    if (db) {
        lua_pushfstring(L, "sqlite3: %s: %s", ctx, sqlite3_errmsg(db));
    } else {
        lua_pushfstring(L, "sqlite3: %s", ctx);
    }
    return 2;
}

/* Bind a single Lua value to a prepared statement parameter (1-based idx).
 * Returns 0 on success, SQLite error code otherwise. Raises on unsupported
 * Lua types (programming error). */
static int bind_one(lua_State *L, sqlite3_stmt *st, int pi, int val_idx) {
    int t = lua_type(L, val_idx);
    switch (t) {
    case LUA_TNIL:
        return sqlite3_bind_null(st, pi);
    case LUA_TBOOLEAN:
        return sqlite3_bind_int(st, pi, lua_toboolean(L, val_idx) ? 1 : 0);
    case LUA_TNUMBER:
#if LUA_VERSION_NUM >= 503
        if (lua_isinteger(L, val_idx)) {
            return sqlite3_bind_int64(st, pi,
                (sqlite3_int64)lua_tointeger(L, val_idx));
        }
#endif
        return sqlite3_bind_double(st, pi, (double)lua_tonumber(L, val_idx));
    case LUA_TSTRING: {
        size_t n; const char *s = lua_tolstring(L, val_idx, &n);
        return sqlite3_bind_text(st, pi, s, (int)n, SQLITE_TRANSIENT);
    }
    default:
        return luaL_error(L, "sqlite3: cannot bind Lua %s",
                          lua_typename(L, t));
    }
}

/* Push a single column value (0-based ci) from st onto the Lua stack. */
static void push_column(lua_State *L, sqlite3_stmt *st, int ci) {
    int ct = sqlite3_column_type(st, ci);
    switch (ct) {
    case SQLITE_INTEGER:
        lua_pushinteger(L, (lua_Integer)sqlite3_column_int64(st, ci));
        break;
    case SQLITE_FLOAT:
        lua_pushnumber(L, (lua_Number)sqlite3_column_double(st, ci));
        break;
    case SQLITE_TEXT: {
        const unsigned char *s = sqlite3_column_text(st, ci);
        int n = sqlite3_column_bytes(st, ci);
        lua_pushlstring(L, (const char *)s, (size_t)n);
        break;
    }
    case SQLITE_BLOB: {
        const void *s = sqlite3_column_blob(st, ci);
        int n = sqlite3_column_bytes(st, ci);
        lua_pushlstring(L, (const char *)s, (size_t)n);
        break;
    }
    case SQLITE_NULL:
    default:
        lua_pushnil(L);
        break;
    }
}

/* Build a map { col_name = value, ... } for the current row of st, using
 * sqlite3_column_name for keys. */
static void push_row_map(lua_State *L, sqlite3_stmt *st) {
    int ncols = sqlite3_column_count(st);
    lua_createtable(L, 0, ncols);
    for (int i = 0; i < ncols; i++) {
        const char *name = sqlite3_column_name(st, i);
        lua_pushstring(L, name ? name : "");
        push_column(L, st, i);
        lua_rawset(L, -3);
    }
}

/* ---- db methods ---------------------------------------------------------- */
static int l_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (lua_type(L, 2) == LUA_TNUMBER) flags = (int)lua_tointeger(L, 2);
    sqlite3 *raw = NULL;
    int rc = sqlite3_open_v2(path, &raw, flags, NULL);
    if (rc != SQLITE_OK) {
        int r = push_sqlite_error(L, raw, "open");
        if (raw) sqlite3_close(raw);
        return r;
    }
    db_t *d = (db_t *)lua_newuserdata(L, sizeof(*d));
    d->db = raw;
    luaL_getmetatable(L, DB_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_close(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (d->db) {
        sqlite3_close(d->db);
        d->db = NULL;
    }
    return 0;
}

static int l_exec(lua_State *L) {
    db_t *d = check_db(L, 1);
    const char *sql = luaL_checkstring(L, 2);
    if (!d->db) return push_sqlite_error(L, NULL, "exec on closed db");
    char *err = NULL;
    int rc = sqlite3_exec(d->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "sqlite3: exec: %s", err ? err : "(?)");
        if (err) sqlite3_free(err);
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)sqlite3_changes(d->db));
    return 1;
}

/* db:query(sql [, params...]) -> array of row-maps */
static int l_query(lua_State *L) {
    db_t *d = check_db(L, 1);
    const char *sql = luaL_checkstring(L, 2);
    if (!d->db) return push_sqlite_error(L, NULL, "query on closed db");
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(d->db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) return push_sqlite_error(L, d->db, "prepare");
    /* bind extra args: positions 1..(nargs-2) */
    int nargs = lua_gettop(L);
    int params = sqlite3_bind_parameter_count(st);
    for (int i = 0; i < params && i < nargs - 2; i++) {
        int br = bind_one(L, st, i + 1, 3 + i);
        if (br != SQLITE_OK) {
            sqlite3_finalize(st);
            return push_sqlite_error(L, d->db, "bind");
        }
    }
    /* iterate rows */
    lua_newtable(L);   /* results */
    int idx = 1;
    while (1) {
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            push_row_map(L, st);
            lua_rawseti(L, -2, idx++);
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            sqlite3_finalize(st);
            return push_sqlite_error(L, d->db, "step");
        }
    }
    sqlite3_finalize(st);
    return 1;
}

static int l_prepare(lua_State *L) {
    db_t *d = check_db(L, 1);
    const char *sql = luaL_checkstring(L, 2);
    if (!d->db) return push_sqlite_error(L, NULL, "prepare on closed db");
    sqlite3_stmt *raw = NULL;
    int rc = sqlite3_prepare_v2(d->db, sql, -1, &raw, NULL);
    if (rc != SQLITE_OK) return push_sqlite_error(L, d->db, "prepare");
    stmt_t *s = (stmt_t *)lua_newuserdata(L, sizeof(*s));
    s->stmt = raw;
    /* pin the db so it outlives the statement */
    lua_pushvalue(L, 1);
    s->db_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    luaL_getmetatable(L, STMT_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_last_insert_rowid(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (!d->db) return push_sqlite_error(L, NULL, "closed db");
    lua_pushinteger(L, (lua_Integer)sqlite3_last_insert_rowid(d->db));
    return 1;
}

static int l_changes(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (!d->db) return push_sqlite_error(L, NULL, "closed db");
    lua_pushinteger(L, (lua_Integer)sqlite3_changes(d->db));
    return 1;
}

static int l_begin(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (!d->db) return push_sqlite_error(L, NULL, "closed db");
    char *err = NULL;
    if (sqlite3_exec(d->db, "BEGIN", NULL, NULL, &err) != SQLITE_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "sqlite3: begin: %s", err ? err : "");
        if (err) sqlite3_free(err);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int l_commit(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (!d->db) return push_sqlite_error(L, NULL, "closed db");
    char *err = NULL;
    if (sqlite3_exec(d->db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "sqlite3: commit: %s", err ? err : "");
        if (err) sqlite3_free(err);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int l_rollback(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (!d->db) return push_sqlite_error(L, NULL, "closed db");
    char *err = NULL;
    if (sqlite3_exec(d->db, "ROLLBACK", NULL, NULL, &err) != SQLITE_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "sqlite3: rollback: %s", err ? err : "");
        if (err) sqlite3_free(err);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int db_gc(lua_State *L) {
    db_t *d = (db_t *)luaL_checkudata(L, 1, DB_MT);
    if (d->db) { sqlite3_close(d->db); d->db = NULL; }
    return 0;
}

/* ---- statement methods --------------------------------------------------- */
static int st_bind(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: bind on finalized stmt");
    int pi = luaL_checkinteger(L, 2);
    int rc = bind_one(L, s->stmt, pi, 3);
    if (rc != SQLITE_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "sqlite3: bind: rc=%d", rc);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int st_bind_all(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: bind_all on finalized stmt");
    int params = sqlite3_bind_parameter_count(s->stmt);
    int nargs = lua_gettop(L) - 1;
    if (nargs > params) nargs = params;
    for (int i = 0; i < nargs; i++) {
        int rc = bind_one(L, s->stmt, i + 1, 2 + i);
        if (rc != SQLITE_OK) {
            lua_pushnil(L);
            lua_pushfstring(L, "sqlite3: bind_all[%d]: rc=%d", i + 1, rc);
            return 2;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int st_step(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: step on finalized stmt");
    int rc = sqlite3_step(s->stmt);
    if (rc == SQLITE_ROW)  { lua_pushliteral(L, "row");  return 1; }
    if (rc == SQLITE_DONE) { lua_pushliteral(L, "done"); return 1; }
    lua_pushnil(L);
    lua_pushfstring(L, "sqlite3: step: rc=%d", rc);
    return 2;
}

static int st_columns(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: columns on finalized stmt");
    push_row_map(L, s->stmt);
    return 1;
}

static int st_reset(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: reset on finalized stmt");
    sqlite3_reset(s->stmt);
    sqlite3_clear_bindings(s->stmt);
    return 0;
}

static int st_finalize(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (s->stmt) { sqlite3_finalize(s->stmt); s->stmt = NULL; }
    if (s->db_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s->db_ref);
        s->db_ref = LUA_NOREF;
    }
    return 0;
}

static int st_gc(lua_State *L) {
    return st_finalize(L);
}

/* ---- registration -------------------------------------------------------- */
static const luaL_Reg db_methods[] = {
    {"close",              l_close},
    {"exec",               l_exec},
    {"query",              l_query},
    {"prepare",            l_prepare},
    {"begin",              l_begin},
    {"commit",             l_commit},
    {"rollback",           l_rollback},
    {"last_insert_rowid",  l_last_insert_rowid},
    {"changes",            l_changes},
    {NULL, NULL},
};

static const luaL_Reg stmt_methods[] = {
    {"bind",     st_bind},
    {"bind_all", st_bind_all},
    {"step",     st_step},
    {"columns",  st_columns},
    {"reset",    st_reset},
    {"finalize", st_finalize},
    {NULL, NULL},
};

static void register_metatable(lua_State *L, const char *name,
                               const luaL_Reg *methods, lua_CFunction gc) {
    luaL_newmetatable(L, name);
    lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, gc); lua_setfield(L, -2, "__gc");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, methods, 0);
#else
    luaL_register(L, NULL, methods);
#endif
    lua_pop(L, 1);
}

void fan_sqlite3_register(lua_State *L) {
    register_metatable(L, DB_MT,   db_methods,   db_gc);
    register_metatable(L, STMT_MT, stmt_methods, st_gc);

    lua_newtable(L);
    lua_pushcfunction(L, l_open);
    lua_setfield(L, -2, "open");
    /* Expose common SQLITE_OPEN_* flags for advanced callers. */
    lua_pushinteger(L, SQLITE_OPEN_READONLY);  lua_setfield(L, -2, "OPEN_READONLY");
    lua_pushinteger(L, SQLITE_OPEN_READWRITE); lua_setfield(L, -2, "OPEN_READWRITE");
    lua_pushinteger(L, SQLITE_OPEN_CREATE);    lua_setfield(L, -2, "OPEN_CREATE");
    lua_pushinteger(L, SQLITE_OPEN_URI);       lua_setfield(L, -2, "OPEN_URI");
    lua_pushinteger(L, SQLITE_OPEN_MEMORY);    lua_setfield(L, -2, "OPEN_MEMORY");
    lua_pushstring(L, sqlite3_libversion());   lua_setfield(L, -2, "version");
    lua_setfield(L, -2, "sqlite3");
}
