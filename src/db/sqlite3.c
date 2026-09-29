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
 *
 * M16.1: API surface expanded to be a drop-in replacement for the LuaRocks
 * `lsqlite3` binding (function names, iterator shapes, numeric result codes,
 * result-code + column-type constants).  The pre-M16.1 luafan2-native shape
 * (db:query shortcut, db:begin/commit/rollback wrappers, stmt:bind_all name)
 * is preserved so existing luafan2 code keeps working; the new methods are
 * strict additions.  See db_sqlite3.h for the full surface list.
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

/* Build an array { value1, value2, ... } for the current row (position order). */
static void push_row_array(lua_State *L, sqlite3_stmt *st) {
    int ncols = sqlite3_column_count(st);
    lua_createtable(L, ncols, 0);
    for (int i = 0; i < ncols; i++) {
        push_column(L, st, i);
        lua_rawseti(L, -2, i + 1);
    }
}

/* Push each column value as a separate return value (for urows) */
static int push_row_unpacked(lua_State *L, sqlite3_stmt *st) {
    int ncols = sqlite3_column_count(st);
    luaL_checkstack(L, ncols + 4, "sqlite3.urows: too many columns");
    for (int i = 0; i < ncols; i++) push_column(L, st, i);
    return ncols;
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

/* db:query(sql [, params...]) -> array of row-maps.  luafan2 shortcut, not
 * present in lsqlite3.  Used heavily by fan.orm. */
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

static int l_total_changes(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (!d->db) return push_sqlite_error(L, NULL, "closed db");
    lua_pushinteger(L, (lua_Integer)sqlite3_total_changes(d->db));
    return 1;
}

static int l_busy_timeout(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (!d->db) return push_sqlite_error(L, NULL, "closed db");
    int ms = (int)luaL_checkinteger(L, 2);
    int rc = sqlite3_busy_timeout(d->db, ms);
    if (rc != SQLITE_OK) return push_sqlite_error(L, d->db, "busy_timeout");
    lua_pushboolean(L, 1);
    return 1;
}

static int l_interrupt(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (!d->db) return push_sqlite_error(L, NULL, "closed db");
    sqlite3_interrupt(d->db);
    return 0;
}

static int l_errcode(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (!d->db) { lua_pushinteger(L, SQLITE_MISUSE); return 1; }
    lua_pushinteger(L, sqlite3_errcode(d->db));
    return 1;
}

static int l_errmsg(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (!d->db) { lua_pushstring(L, "db is closed"); return 1; }
    lua_pushstring(L, sqlite3_errmsg(d->db));
    return 1;
}

static int l_isopen(lua_State *L) {
    db_t *d = check_db(L, 1);
    lua_pushboolean(L, d->db != NULL);
    return 1;
}

static int l_get_autocommit(lua_State *L) {
    db_t *d = check_db(L, 1);
    if (!d->db) return push_sqlite_error(L, NULL, "closed db");
    lua_pushboolean(L, sqlite3_get_autocommit(d->db) != 0);
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

/* ---- db-level iterators (lsqlite3 style) --------------------------------- */
/*
 * db:nrows(sql), db:rows(sql), db:urows(sql).  lsqlite3 semantics:
 *   for row in db:nrows("SELECT ...") do ... end
 * The iterator owns a prepared statement in its first upvalue; when the walk
 * ends (SQLITE_DONE) or the iterator table goes out of scope, the stmt is
 * finalized.  We store the stmt as a stmt userdata so its __gc reclaims it
 * even if the user breaks out of the loop early.
 *
 * `mode` upvalue #2:
 *   1 = nrows (map),  2 = rows (array),  3 = urows (unpacked returns)
 */

static int db_iter_step(lua_State *L) {
    stmt_t *s = (stmt_t *)lua_touserdata(L, lua_upvalueindex(1));
    int mode = (int)lua_tointeger(L, lua_upvalueindex(2));
    if (!s || !s->stmt) return 0;   /* already finalized */
    int rc = sqlite3_step(s->stmt);
    if (rc == SQLITE_ROW) {
        if (mode == 1) { push_row_map(L, s->stmt);   return 1; }
        if (mode == 2) { push_row_array(L, s->stmt); return 1; }
        return push_row_unpacked(L, s->stmt);
    }
    /* End of results, or an error: finalize now so we release the row lock
     * even if the caller keeps the iterator around. */
    sqlite3_finalize(s->stmt);
    s->stmt = NULL;
    if (rc != SQLITE_DONE) {
        /* mid-iteration errors are raised (there is no room to return
         * (nil, err) inside a for-in loop without the loop treating nil as
         * "end of iteration").  This matches lsqlite3's behaviour. */
        return luaL_error(L, "sqlite3: iterator step: rc=%d", rc);
    }
    return 0;   /* signal end-of-iteration to the for-loop */
}

static int db_make_iter(lua_State *L, int mode) {
    db_t *d = check_db(L, 1);
    const char *sql = luaL_checkstring(L, 2);
    if (!d->db) return push_sqlite_error(L, NULL, "iterator on closed db");
    sqlite3_stmt *raw = NULL;
    int rc = sqlite3_prepare_v2(d->db, sql, -1, &raw, NULL);
    if (rc != SQLITE_OK) return push_sqlite_error(L, d->db, "prepare");
    /* Wrap in a stmt userdata so GC will finalize even on early break. */
    stmt_t *s = (stmt_t *)lua_newuserdata(L, sizeof(*s));
    s->stmt = raw;
    lua_pushvalue(L, 1);
    s->db_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    luaL_getmetatable(L, STMT_MT);
    lua_setmetatable(L, -2);
    /* Push mode as second upvalue and return the C closure. */
    lua_pushinteger(L, mode);
    lua_pushcclosure(L, db_iter_step, 2);
    return 1;
}

static int l_nrows(lua_State *L) { return db_make_iter(L, 1); }
static int l_rows(lua_State *L)  { return db_make_iter(L, 2); }
static int l_urows(lua_State *L) { return db_make_iter(L, 3); }

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

/* stmt:bind_names{ [":name"|"name"] = value, ... }
 * lsqlite3 accepts keys with or without the leading ':'/'@'/'$'; we do the
 * same by trying the raw key first, then falling back to ":key". */
static int st_bind_names(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: bind_names on finalized stmt");
    luaL_checktype(L, 2, LUA_TTABLE);
    int nparam = sqlite3_bind_parameter_count(s->stmt);
    for (int pi = 1; pi <= nparam; pi++) {
        const char *pname = sqlite3_bind_parameter_name(s->stmt, pi);
        if (!pname) continue;   /* positional '?': skip in named bind */
        /* pname is ":foo" / "@foo" / "$foo"; user may supply "foo" or ":foo" */
        lua_pushstring(L, pname);       /* try raw ":foo" */
        lua_gettable(L, 2);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushstring(L, pname + 1);   /* try bare "foo" (skip prefix char) */
            lua_gettable(L, 2);
        }
        int rc;
        if (lua_isnil(L, -1)) {
            rc = sqlite3_bind_null(s->stmt, pi);
        } else {
            rc = bind_one(L, s->stmt, pi, lua_gettop(L));
        }
        lua_pop(L, 1);
        if (rc != SQLITE_OK) {
            lua_pushnil(L);
            lua_pushfstring(L, "sqlite3: bind_names[%s]: rc=%d", pname, rc);
            return 2;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int st_bind_parameter_count(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: bind_parameter_count on finalized stmt");
    lua_pushinteger(L, sqlite3_bind_parameter_count(s->stmt));
    return 1;
}

static int st_bind_parameter_name(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: bind_parameter_name on finalized stmt");
    int i = (int)luaL_checkinteger(L, 2);
    const char *name = sqlite3_bind_parameter_name(s->stmt, i);
    if (name) lua_pushstring(L, name); else lua_pushnil(L);
    return 1;
}

static int st_step(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: step on finalized stmt");
    int rc = sqlite3_step(s->stmt);
    /* M16.1: return numeric result code (lsqlite3-compatible).  Callers must
     * compare against sqlite3.ROW / sqlite3.DONE, not string constants. */
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
        lua_pushinteger(L, rc);
        return 1;
    }
    lua_pushnil(L);
    lua_pushfstring(L, "sqlite3: step: rc=%d", rc);
    return 2;
}

/* stmt:columns() -> integer.  M16.1: lsqlite3 semantics (column COUNT).
 * The pre-M16.1 "current row as map" behaviour is available via nrows()
 * iterator or a step()+get_names()+... walk. */
static int st_columns(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: columns on finalized stmt");
    lua_pushinteger(L, sqlite3_column_count(s->stmt));
    return 1;
}

/* stmt:get_names() -> array of column names (empty if statement has no cols) */
static int st_get_names(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: get_names on finalized stmt");
    int n = sqlite3_column_count(s->stmt);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        const char *nm = sqlite3_column_name(s->stmt, i);
        lua_pushstring(L, nm ? nm : "");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* stmt:get_types() -> array of declared column types (from CREATE TABLE).
 * Note: these are the DECLARED types (may be NULL for computed columns),
 * NOT the runtime storage class of the current row's values. */
static int st_get_types(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: get_types on finalized stmt");
    int n = sqlite3_column_count(s->stmt);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        const char *ty = sqlite3_column_decltype(s->stmt, i);
        lua_pushstring(L, ty ? ty : "");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int st_isopen(lua_State *L) {
    stmt_t *s = check_stmt(L, 1);
    lua_pushboolean(L, s->stmt != NULL);
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

/* ---- stmt-level iterators (lsqlite3 style) -------------------------------
 *
 * for row in stmt:nrows() do ... end
 *
 * Unlike db:nrows(sql), the stmt is NOT finalized when iteration ends — the
 * caller owns the stmt and may reset+bind+iterate again.  We just push a
 * C closure whose upvalue is the stmt userdata.
 */

static int stmt_iter_step(lua_State *L) {
    stmt_t *s = (stmt_t *)lua_touserdata(L, lua_upvalueindex(1));
    int mode = (int)lua_tointeger(L, lua_upvalueindex(2));
    if (!s || !s->stmt) return 0;
    int rc = sqlite3_step(s->stmt);
    if (rc == SQLITE_ROW) {
        if (mode == 1) { push_row_map(L, s->stmt);   return 1; }
        if (mode == 2) { push_row_array(L, s->stmt); return 1; }
        return push_row_unpacked(L, s->stmt);
    }
    if (rc != SQLITE_DONE) {
        return luaL_error(L, "sqlite3: iterator step: rc=%d", rc);
    }
    return 0;
}

static int st_make_iter(lua_State *L, int mode) {
    stmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "sqlite3: iterator on finalized stmt");
    lua_pushvalue(L, 1);            /* upvalue #1: the stmt userdata itself */
    lua_pushinteger(L, mode);       /* upvalue #2: mode */
    lua_pushcclosure(L, stmt_iter_step, 2);
    return 1;
}

static int st_nrows(lua_State *L) { return st_make_iter(L, 1); }
static int st_rows(lua_State *L)  { return st_make_iter(L, 2); }
static int st_urows(lua_State *L) { return st_make_iter(L, 3); }

/* ---- module-level helpers ------------------------------------------------ */

/* sqlite3.complete(sql) -> bool.  Heuristic check: does the SQL text
 * terminate in a way that looks like a complete statement?  Wraps
 * sqlite3_complete().  Useful for REPLs. */
static int l_mod_complete(lua_State *L) {
    const char *sql = luaL_checkstring(L, 1);
    lua_pushboolean(L, sqlite3_complete(sql));
    return 1;
}

/* ---- registration -------------------------------------------------------- */
static const luaL_Reg db_methods[] = {
    {"close",              l_close},
    {"exec",               l_exec},
    {"execute",            l_exec},   /* lsqlite3 alias */
    {"query",              l_query},
    {"nrows",              l_nrows},
    {"rows",               l_rows},
    {"urows",              l_urows},
    {"prepare",            l_prepare},
    {"begin",              l_begin},
    {"commit",             l_commit},
    {"rollback",           l_rollback},
    {"last_insert_rowid",  l_last_insert_rowid},
    {"changes",            l_changes},
    {"total_changes",      l_total_changes},
    {"busy_timeout",       l_busy_timeout},
    {"interrupt",          l_interrupt},
    {"errcode",            l_errcode},
    {"errmsg",             l_errmsg},
    {"isopen",             l_isopen},
    {"get_autocommit",     l_get_autocommit},
    {NULL, NULL},
};

static const luaL_Reg stmt_methods[] = {
    {"bind",                  st_bind},
    {"bind_all",              st_bind_all},
    {"bind_values",           st_bind_all},   /* lsqlite3 alias */
    {"bind_names",            st_bind_names},
    {"bind_parameter_count",  st_bind_parameter_count},
    {"bind_parameter_name",   st_bind_parameter_name},
    {"step",                  st_step},
    {"columns",               st_columns},
    {"get_names",             st_get_names},
    {"get_types",             st_get_types},
    {"nrows",                 st_nrows},
    {"rows",                  st_rows},
    {"urows",                 st_urows},
    {"isopen",                st_isopen},
    {"reset",                 st_reset},
    {"finalize",              st_finalize},
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

/* Push name=integer onto the sqlite3 module table at top of stack. */
static void set_int_const(lua_State *L, const char *name, int val) {
    lua_pushinteger(L, val);
    lua_setfield(L, -2, name);
}

void fan_sqlite3_register(lua_State *L) {
    register_metatable(L, DB_MT,   db_methods,   db_gc);
    register_metatable(L, STMT_MT, stmt_methods, st_gc);

    lua_newtable(L);
    lua_pushcfunction(L, l_open);        lua_setfield(L, -2, "open");
    lua_pushcfunction(L, l_mod_complete); lua_setfield(L, -2, "complete");

    /* Open flags */
    set_int_const(L, "OPEN_READONLY",     SQLITE_OPEN_READONLY);
    set_int_const(L, "OPEN_READWRITE",    SQLITE_OPEN_READWRITE);
    set_int_const(L, "OPEN_CREATE",       SQLITE_OPEN_CREATE);
    set_int_const(L, "OPEN_URI",          SQLITE_OPEN_URI);
    set_int_const(L, "OPEN_MEMORY",       SQLITE_OPEN_MEMORY);

    /* Result codes (lsqlite3-compatible names; needed for step() comparison
     * and for errcode() inspection).  Only the common subset — SQLite has
     * ~30 extended codes but callers rarely disambiguate below this level. */
    set_int_const(L, "OK",         SQLITE_OK);
    set_int_const(L, "ERROR",      SQLITE_ERROR);
    set_int_const(L, "INTERNAL",   SQLITE_INTERNAL);
    set_int_const(L, "PERM",       SQLITE_PERM);
    set_int_const(L, "ABORT",      SQLITE_ABORT);
    set_int_const(L, "BUSY",       SQLITE_BUSY);
    set_int_const(L, "LOCKED",     SQLITE_LOCKED);
    set_int_const(L, "NOMEM",      SQLITE_NOMEM);
    set_int_const(L, "READONLY",   SQLITE_READONLY);
    set_int_const(L, "INTERRUPT",  SQLITE_INTERRUPT);
    set_int_const(L, "IOERR",      SQLITE_IOERR);
    set_int_const(L, "CORRUPT",    SQLITE_CORRUPT);
    set_int_const(L, "NOTFOUND",   SQLITE_NOTFOUND);
    set_int_const(L, "FULL",       SQLITE_FULL);
    set_int_const(L, "CANTOPEN",   SQLITE_CANTOPEN);
    set_int_const(L, "PROTOCOL",   SQLITE_PROTOCOL);
    set_int_const(L, "EMPTY",      SQLITE_EMPTY);
    set_int_const(L, "SCHEMA",     SQLITE_SCHEMA);
    set_int_const(L, "TOOBIG",     SQLITE_TOOBIG);
    set_int_const(L, "CONSTRAINT", SQLITE_CONSTRAINT);
    set_int_const(L, "MISMATCH",   SQLITE_MISMATCH);
    set_int_const(L, "MISUSE",     SQLITE_MISUSE);
    set_int_const(L, "NOLFS",      SQLITE_NOLFS);
    set_int_const(L, "AUTH",       SQLITE_AUTH);
    set_int_const(L, "FORMAT",     SQLITE_FORMAT);
    set_int_const(L, "RANGE",      SQLITE_RANGE);
    set_int_const(L, "NOTADB",     SQLITE_NOTADB);
    set_int_const(L, "ROW",        SQLITE_ROW);
    set_int_const(L, "DONE",       SQLITE_DONE);

    /* Column storage-class type codes (for use with a future column_type() /
     * for parity with lsqlite3's sqlite3.INTEGER etc). */
    set_int_const(L, "INTEGER",    SQLITE_INTEGER);
    set_int_const(L, "FLOAT",      SQLITE_FLOAT);
    set_int_const(L, "TEXT",       SQLITE_TEXT);
    set_int_const(L, "BLOB",       SQLITE_BLOB);
    set_int_const(L, "NULL",       SQLITE_NULL);

    lua_pushstring(L, sqlite3_libversion());   lua_setfield(L, -2, "version");
    lua_setfield(L, -2, "sqlite3");
}
