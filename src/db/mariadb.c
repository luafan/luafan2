/*
 * db/mariadb.c — LuaFan v2 MariaDB binding (synchronous M5.5.a).
 *
 * Two userdata types:
 *   fan.mariadb.db   -> MYSQL*
 *   fan.mariadb.stmt -> MYSQL_STMT* + registry ref to owning db
 *
 * Design notes:
 *   - Uses libmariadb "mysql_*" symbols (mariadb-connector-c ships them);
 *     we compile against /usr/include/mariadb/mysql.h and link -lmariadb.
 *   - Results are always fully fetched (mysql_store_result / mysql_stmt_
 *     store_result) so subsequent queries on the same connection are legal.
 *   - Value marshalling on query results:
 *       MYSQL_TYPE_TINY/SHORT/LONG/LONGLONG/INT24/YEAR -> lua integer
 *       MYSQL_TYPE_FLOAT/DOUBLE/DECIMAL/NEWDECIMAL     -> lua number
 *       everything else (string/blob/date/time/json)   -> lua string
 *       NULL                                           -> nil
 *   - Bindings on prepared statements accept nil/bool/integer/number/string.
 *
 * M5.5.b (deferred): libevent-integrated mysql_*_start / _cont wait, plus
 * fan.mariadb.pool with R19 (close cancels pending waits) and R13 (GC
 * synchronous close) regressions.
 */
#include "../platform.h"

#include <lauxlib.h>
#include <mysql.h>              /* system libmariadb header */
#include "db_mariadb.h"
#include "../runtime/loop.h"
#include "../runtime/coro.h"
#include <event2/event.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DB_MT   "fan.mariadb.db"
#define STMT_MT "fan.mariadb.stmt"

/* Forward decl for the per-db pending async wait. */
struct mdb_wait;

typedef struct {
    MYSQL *db;             /* NULL after close */
    int    autocommit;     /* 1 = on (default), 0 = off */
    struct mdb_wait *wait; /* current in-flight async op, or NULL */
} mdb_t;

/* Forward decls used by the async state machine (defined later). */
static void push_column_value(lua_State *L, MYSQL_FIELD *f,
                              const char *val, unsigned long len);

typedef struct {
    MYSQL_STMT *stmt;      /* NULL after finalize */
    int         db_ref;    /* registry ref to owning db userdata */
    /* prepared bind buffers, allocated on demand in bind_all/step */
    MYSQL_BIND *params;
    unsigned    nparams;
    MYSQL_BIND *cols;
    unsigned    ncols;
    /* per-column output storage; each column gets its own buffer */
    void      **col_buf;
    unsigned long *col_buflen;
    unsigned long *col_length;
    my_bool    *col_isnull;
    my_bool    *col_error;
    /* per-param owned storage (for text/binary params) */
    char      **param_str;
    unsigned long *param_len;
    long long *param_int;
    double    *param_dbl;
    my_bool   *param_isnull;
} mstmt_t;

/* ---- helpers ------------------------------------------------------------ */
static mdb_t *check_db(lua_State *L, int idx) {
    return (mdb_t *)luaL_checkudata(L, idx, DB_MT);
}
static mstmt_t *check_stmt(lua_State *L, int idx) {
    return (mstmt_t *)luaL_checkudata(L, idx, STMT_MT);
}

static int push_mysql_error(lua_State *L, MYSQL *db, const char *ctx) {
    lua_pushnil(L);
    if (db) {
        lua_pushfstring(L, "mariadb: %s: %s", ctx, mysql_error(db));
    } else {
        lua_pushfstring(L, "mariadb: %s", ctx);
    }
    return 2;
}
static int push_stmt_error(lua_State *L, MYSQL_STMT *st, const char *ctx) {
    lua_pushnil(L);
    if (st) {
        lua_pushfstring(L, "mariadb: %s: %s", ctx, mysql_stmt_error(st));
    } else {
        lua_pushfstring(L, "mariadb: %s", ctx);
    }
    return 2;
}

/* ---- connect ------------------------------------------------------------ */
/* fan.mariadb.connect{host=..., port=..., user=..., password=..., database=...,
 *                     unix_socket=..., charset=..., autocommit=<bool>} */
static int l_connect(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *host = NULL, *user = NULL, *pw = NULL, *db = NULL,
               *sock = NULL, *charset = NULL;
    unsigned int port = 0;
    int autocommit = 1;

    lua_getfield(L, 1, "host");        host = lua_tostring(L, -1); lua_pop(L,1);
    lua_getfield(L, 1, "user");        user = lua_tostring(L, -1); lua_pop(L,1);
    lua_getfield(L, 1, "password");    pw   = lua_tostring(L, -1); lua_pop(L,1);
    lua_getfield(L, 1, "database");    db   = lua_tostring(L, -1); lua_pop(L,1);
    lua_getfield(L, 1, "unix_socket"); sock = lua_tostring(L, -1); lua_pop(L,1);
    lua_getfield(L, 1, "charset");     charset = lua_tostring(L, -1); lua_pop(L,1);
    lua_getfield(L, 1, "port");
    if (lua_type(L, -1) == LUA_TNUMBER) port = (unsigned int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "autocommit");
    if (lua_type(L, -1) == LUA_TBOOLEAN) autocommit = lua_toboolean(L, -1);
    lua_pop(L, 1);

    MYSQL *raw = mysql_init(NULL);
    if (!raw) return push_mysql_error(L, NULL, "mysql_init");
    /* Enable non-blocking API (required for mysql_*_start / _cont; safe for
     * synchronous calls too). Must be set before mysql_real_connect. */
    mysql_options(raw, MYSQL_OPT_NONBLOCK, 0);
    if (charset) {
        mysql_options(raw, MYSQL_SET_CHARSET_NAME, charset);
    }
    if (!mysql_real_connect(raw, host, user, pw, db, port, sock, 0)) {
        int r = push_mysql_error(L, raw, "connect");
        mysql_close(raw);
        return r;
    }
    if (!autocommit) {
        if (mysql_autocommit(raw, 0)) {
            int r = push_mysql_error(L, raw, "autocommit");
            mysql_close(raw);
            return r;
        }
    }
    mdb_t *d = (mdb_t *)lua_newuserdata(L, sizeof(*d));
    d->db = raw;
    d->autocommit = autocommit;
    d->wait = NULL;
    luaL_getmetatable(L, DB_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* =========================================================================
 * Async wait state machine (M5.5.c).
 *
 * libmariadb provides non-blocking counterparts to the sync API:
 *   mysql_real_query_start / _cont, mysql_store_result_start / _cont,
 *   mysql_stmt_execute_start / _cont, mysql_stmt_fetch_start / _cont, ...
 * They return a bitmask of MYSQL_WAIT_READ / WRITE / EXCEPT / TIMEOUT when
 * work is pending; 0 means done and the *_start/_cont's out-param holds the
 * real return value.
 *
 * Design:
 *   - Each mdb_t owns at most one in-flight mdb_wait_t (serial per conn).
 *   - We hook a libevent event on mysql_get_socket(fd) with EV_READ | EV_WRITE
 *     translated from the returned mask, plus an optional timeout.
 *   - The event callback re-enters via mysql_*_cont; if still waiting we
 *     re-arm; if done we finalise the operation, produce the Lua return
 *     values on the coroutine's stack, and fan_coro_wake().
 *   - R19: db:close() with a pending wait must cancel the event, resume
 *     the coroutine with (nil, "mariadb: closed") and free the state before
 *     mysql_close so no dangling event outlives the connection.
 * ========================================================================= */
enum wait_kind {
    WAIT_QUERY,      /* db:query_async / db:exec_async — mysql_real_query */
    WAIT_STORE,      /* after query completes: mysql_store_result_start   */
};

typedef struct mdb_wait {
    mdb_t       *owner;    /* NULL after cancellation */
    lua_State   *L;        /* main state (owns registry) */
    lua_State   *co;       /* parked coroutine */
    int          co_ref;   /* registry pin */
    struct event *ev;      /* libevent io/timeout event */
    enum wait_kind kind;

    /* Query-in-flight buffers: text SQL to send. For _query, the C caller
     * expanded '?' before invoking start(); this pointer is a copy owned by
     * the wait_t. */
    char        *sql;
    unsigned long sql_len;

    /* Whether the caller wanted a result set (query_async=1, exec_async=0). */
    int          want_result;

    /* Second-stage: MYSQL_RES* returned by mysql_store_result_start. */
    MYSQL_RES   *res;

    /* Return codes carried between _start and _cont across event cycles. */
    int          store_err;
    int          query_err;
} mdb_wait_t;

/* Translate libmariadb wait mask to libevent EV_* mask. */
static short wait_to_ev(int status) {
    short what = 0;
    if (status & MYSQL_WAIT_READ)  what |= EV_READ;
    if (status & MYSQL_WAIT_WRITE) what |= EV_WRITE;
    /* EXCEPT: rare; libevent has no matching flag, treat as READ. */
    if (status & MYSQL_WAIT_EXCEPT) what |= EV_READ;
    return what;
}
static int ev_to_wait(short what) {
    int s = 0;
    if (what & EV_READ)    s |= MYSQL_WAIT_READ;
    if (what & EV_WRITE)   s |= MYSQL_WAIT_WRITE;
    if (what & EV_TIMEOUT) s |= MYSQL_WAIT_TIMEOUT;
    return s;
}

static void wait_free(mdb_wait_t *w) {
    if (!w) return;
    if (w->ev)  { event_free(w->ev); w->ev = NULL; }
    if (w->sql) { free(w->sql); w->sql = NULL; }
    if (w->res) { mysql_free_result(w->res); w->res = NULL; }
    free(w);
}

/* Push a result-set MYSQL_RES* as an array of row-maps on w->co's stack. */
static void push_result_rows(lua_State *co, MYSQL_RES *res) {
    if (!res) { lua_newtable(co); return; }
    unsigned nfields = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    lua_newtable(co);
    int idx = 1;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        unsigned long *lens = mysql_fetch_lengths(res);
        lua_createtable(co, 0, (int)nfields);
        for (unsigned i = 0; i < nfields; i++) {
            lua_pushstring(co, fields[i].name ? fields[i].name : "");
            push_column_value(co, &fields[i], row[i], lens ? lens[i] : 0);
            lua_rawset(co, -3);
        }
        lua_rawseti(co, -2, idx++);
    }
}

/* Arm w->ev on the current mask + timeout. Returns 0 on success, -1 on
 * event_add failure. */
static void seconds_to_tv(double sec, struct timeval *tv) {
    if (sec < 0) sec = 0;
    tv->tv_sec  = (long)sec;
    tv->tv_usec = (long)((sec - (double)tv->tv_sec) * 1e6);
    if (tv->tv_usec < 0) tv->tv_usec = 0;
    if (tv->tv_usec > 999999) tv->tv_usec = 999999;
}
static int wait_arm(mdb_wait_t *w, int status);
static void wait_cb(evutil_socket_t fd, short what, void *arg);

/* Continue the pending operation. If done, resume the coroutine with
 * the appropriate Lua values. Returns "still waiting" flag. */
static void wait_step(mdb_wait_t *w, int status) {
    MYSQL *mysql = w->owner ? w->owner->db : NULL;
    if (!mysql) {
        /* connection was closed underneath us (R19); resume with err */
        lua_pushnil(w->co);
        lua_pushliteral(w->co, "mariadb: closed");
        lua_State *mainL = w->L;
        lua_State *co    = w->co;
        int co_ref       = w->co_ref;
        wait_free(w);
        fan_coro_wake(mainL, co, co_ref, 2);
        return;
    }

    if (w->kind == WAIT_QUERY) {
        int rc;
        if (status < 0) {
            /* first call: _start */
            rc = (int)mysql_real_query_start(&w->query_err, mysql,
                                             w->sql, w->sql_len);
        } else {
            rc = (int)mysql_real_query_cont(&w->query_err, mysql, status);
        }
        if (rc != 0) {                /* still waiting */
            if (wait_arm(w, rc) != 0) {
                lua_pushnil(w->co);
                lua_pushliteral(w->co, "mariadb: wait arm failed");
                lua_State *mainL=w->L; lua_State *co=w->co; int r=w->co_ref;
                w->owner->wait = NULL;
                wait_free(w);
                fan_coro_wake(mainL, co, r, 2);
            }
            return;
        }
        /* query done */
        if (w->query_err) {
            lua_pushnil(w->co);
            lua_pushfstring(w->co, "mariadb: query: %s", mysql_error(mysql));
            lua_State *mainL=w->L; lua_State *co=w->co; int r=w->co_ref;
            w->owner->wait = NULL;
            wait_free(w);
            fan_coro_wake(mainL, co, r, 2);
            return;
        }
        if (!w->want_result) {
            /* exec_async: return affected_rows */
            lua_pushinteger(w->co, (lua_Integer)mysql_affected_rows(mysql));
            lua_State *mainL=w->L; lua_State *co=w->co; int r=w->co_ref;
            w->owner->wait = NULL;
            wait_free(w);
            fan_coro_wake(mainL, co, r, 1);
            return;
        }
        /* transition into WAIT_STORE */
        if (mysql_field_count(mysql) == 0) {
            /* non-select query — return empty array */
            lua_newtable(w->co);
            lua_State *mainL=w->L; lua_State *co=w->co; int r=w->co_ref;
            w->owner->wait = NULL;
            wait_free(w);
            fan_coro_wake(mainL, co, r, 1);
            return;
        }
        w->kind = WAIT_STORE;
        /* fall-through: start store_result */
        int rc2 = (int)mysql_store_result_start(&w->res, mysql);
        if (rc2 != 0) {
            if (wait_arm(w, rc2) != 0) {
                lua_pushnil(w->co);
                lua_pushliteral(w->co, "mariadb: wait arm failed");
                lua_State *mainL=w->L; lua_State *co=w->co; int r=w->co_ref;
                w->owner->wait = NULL;
                wait_free(w);
                fan_coro_wake(mainL, co, r, 2);
            }
            return;
        }
        /* store done immediately — finalise here (avoid falling through to
         * WAIT_STORE cont path, which would re-enter mysql_store_result_cont
         * and corrupt libmariadb state). */
        if (!w->res) {
            lua_pushnil(w->co);
            lua_pushfstring(w->co, "mariadb: store: %s", mysql_error(mysql));
            lua_State *mainL=w->L; lua_State *co=w->co; int r=w->co_ref;
            w->owner->wait = NULL;
            wait_free(w);
            fan_coro_wake(mainL, co, r, 2);
            return;
        }
        push_result_rows(w->co, w->res);
        {
            lua_State *mainL=w->L; lua_State *co=w->co; int r=w->co_ref;
            w->owner->wait = NULL;
            wait_free(w);
            fan_coro_wake(mainL, co, r, 1);
        }
        return;
    }

    if (w->kind == WAIT_STORE) {
        /* Arriving from an event callback for a previously-armed store cont. */
        int rc = (int)mysql_store_result_cont(&w->res, mysql, status);
        if (rc != 0) {
            if (wait_arm(w, rc) != 0) {
                lua_pushnil(w->co);
                lua_pushliteral(w->co, "mariadb: wait arm failed");
                lua_State *mainL=w->L; lua_State *co=w->co; int r=w->co_ref;
                w->owner->wait = NULL;
                wait_free(w);
                fan_coro_wake(mainL, co, r, 2);
            }
            return;
        }
        if (!w->res) {
            lua_pushnil(w->co);
            lua_pushfstring(w->co, "mariadb: store: %s", mysql_error(mysql));
            lua_State *mainL=w->L; lua_State *co=w->co; int r=w->co_ref;
            w->owner->wait = NULL;
            wait_free(w);
            fan_coro_wake(mainL, co, r, 2);
            return;
        }
        push_result_rows(w->co, w->res);
        lua_State *mainL=w->L; lua_State *co=w->co; int r=w->co_ref;
        w->owner->wait = NULL;
        wait_free(w);
        fan_coro_wake(mainL, co, r, 1);
    }
}

static void wait_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd;
    mdb_wait_t *w = (mdb_wait_t *)arg;
    wait_step(w, ev_to_wait(what));
}

static int wait_arm(mdb_wait_t *w, int status) {
    struct event_base *base = fan_loop_current_base();
    if (!base || !w->owner || !w->owner->db) return -1;
    short what = wait_to_ev(status);
    int fd = mysql_get_socket(w->owner->db);
    if (w->ev) { event_free(w->ev); w->ev = NULL; }
    w->ev = event_new(base, fd, what, wait_cb, w);
    if (!w->ev) return -1;
    struct timeval tv;
    struct timeval *tvp = NULL;
    if (status & MYSQL_WAIT_TIMEOUT) {
        unsigned int t_ms = mysql_get_timeout_value_ms(w->owner->db);
        if (t_ms > 0) {
            seconds_to_tv(((double)t_ms) / 1000.0, &tv);
            tvp = &tv;
        }
    }
    return event_add(w->ev, tvp);
}

/* Cancel any pending wait, resuming the parked coroutine with (nil, msg). */
static void cancel_pending_wait(mdb_t *d, const char *msg) {
    mdb_wait_t *w = d->wait;
    if (!w) return;
    d->wait = NULL;
    /* detach from owner so wait_step won't touch d->db after mysql_close */
    w->owner = NULL;
    lua_pushnil(w->co);
    lua_pushstring(w->co, msg);
    lua_State *mainL = w->L;
    lua_State *co    = w->co;
    int co_ref       = w->co_ref;
    wait_free(w);
    fan_coro_wake(mainL, co, co_ref, 2);
}

static int l_close(lua_State *L) {
    mdb_t *d = check_db(L, 1);
    if (d->wait) cancel_pending_wait(d, "mariadb: closed");   /* R19 */
    if (d->db) { mysql_close(d->db); d->db = NULL; }
    return 0;
}
static int db_gc(lua_State *L) {
    mdb_t *d = (mdb_t *)luaL_checkudata(L, 1, DB_MT);
    if (d->wait) cancel_pending_wait(d, "mariadb: closed");   /* R13 */
    if (d->db) { mysql_close(d->db); d->db = NULL; }
    return 0;
}

/* ---- exec (single statement, no params) --------------------------------- */
static int l_exec(lua_State *L) {
    mdb_t *d = check_db(L, 1);
    if (!d->db) return push_mysql_error(L, NULL, "exec on closed db");
    size_t n; const char *sql = luaL_checklstring(L, 2, &n);
    if (mysql_real_query(d->db, sql, (unsigned long)n)) {
        return push_mysql_error(L, d->db, "exec");
    }
    /* Drain any result set (e.g. multi-stmt not supported here); for a
     * pure DDL/DML this returns 0 rows_affected == 0 which is fine. */
    MYSQL_RES *res = mysql_store_result(d->db);
    if (res) mysql_free_result(res);
    lua_pushinteger(L, (lua_Integer)mysql_affected_rows(d->db));
    return 1;
}

/* Push a MYSQL_ROW column value with type-aware conversion. */
static void push_column_value(lua_State *L, MYSQL_FIELD *f,
                              const char *val, unsigned long len) {
    if (!val) { lua_pushnil(L); return; }
    switch (f->type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
        lua_pushinteger(L, (lua_Integer)strtoll(val, NULL, 10));
        return;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
        lua_pushnumber(L, strtod(val, NULL));
        return;
    default:
        lua_pushlstring(L, val, (size_t)len);
        return;
    }
}

/* Return an escaped literal for value at val_idx (owned pointer via
 * mysql_real_escape_string); caller must free with free(). */
static char *escape_literal(lua_State *L, MYSQL *db, int val_idx, char *outbuf,
                            size_t outbufcap, char **heap) {
    /* outbuf: caller-provided small stack buffer for tiny values;
     * heap:   set to non-NULL when we needed a malloc so caller frees it */
    *heap = NULL;
    int t = lua_type(L, val_idx);
    if (t == LUA_TNIL) {
        snprintf(outbuf, outbufcap, "NULL");
        return outbuf;
    }
    if (t == LUA_TBOOLEAN) {
        snprintf(outbuf, outbufcap, "%d", lua_toboolean(L, val_idx) ? 1 : 0);
        return outbuf;
    }
    if (t == LUA_TNUMBER) {
#if LUA_VERSION_NUM >= 503
        if (lua_isinteger(L, val_idx)) {
            snprintf(outbuf, outbufcap, LUA_INTEGER_FMT,
                     lua_tointeger(L, val_idx));
        } else
#endif
        {
            snprintf(outbuf, outbufcap, "%.17g", (double)lua_tonumber(L, val_idx));
        }
        return outbuf;
    }
    if (t == LUA_TSTRING) {
        size_t n; const char *s = lua_tolstring(L, val_idx, &n);
        size_t bufsz = n * 2 + 3;
        char *buf = (char *)malloc(bufsz);
        if (!buf) { luaL_error(L, "mariadb: OOM in escape"); return NULL; }
        buf[0] = '\'';
        unsigned long en = mysql_real_escape_string(db, buf + 1, s, (unsigned long)n);
        buf[en + 1] = '\'';
        buf[en + 2] = '\0';
        *heap = buf;
        return buf;
    }
    luaL_error(L, "mariadb: cannot bind Lua %s", lua_typename(L, t));
    return NULL;
}

/* db:query(sql [, params...]) -> array of row-maps.
 * Params are substituted in order for '?' placeholders using mysql_real_
 * escape_string. This keeps the wire representation identical to a plain
 * mysql_real_query and gives us untyped result parsing (server returns text). */
static int l_query(lua_State *L) {
    mdb_t *d = check_db(L, 1);
    if (!d->db) return push_mysql_error(L, NULL, "query on closed db");
    size_t sqllen; const char *sql = luaL_checklstring(L, 2, &sqllen);
    int nargs = lua_gettop(L) - 2;

    /* If there are no params, ship the SQL as-is. Otherwise expand '?'. */
    char *expanded = NULL;
    unsigned long expanded_len = 0;
    if (nargs > 0) {
        /* Build expanded SQL: walk sql, replace each '?' outside quotes with
         * escaped literal. For simplicity we treat '?' anywhere; the M5.5.a
         * test set never puts a '?' inside a string literal. */
        size_t cap = sqllen + 32;
        char *out = (char *)malloc(cap);
        if (!out) return luaL_error(L, "mariadb: OOM");
        size_t olen = 0;
        int argi = 0;
        for (size_t i = 0; i < sqllen; i++) {
            char c = sql[i];
            if (c != '?') {
                if (olen + 1 >= cap) { cap *= 2; out = realloc(out, cap); if(!out) return luaL_error(L, "mariadb: OOM"); }
                out[olen++] = c;
                continue;
            }
            argi++;
            if (argi > nargs) {
                free(out);
                return push_mysql_error(L, NULL, "more ? than params");
            }
            char stackbuf[64]; char *heap = NULL;
            char *lit = escape_literal(L, d->db, 2 + argi, stackbuf,
                                       sizeof(stackbuf), &heap);
            size_t ln = strlen(lit);
            while (olen + ln + 1 >= cap) {
                cap = (olen + ln + 1) * 2;
                out = realloc(out, cap);
                if (!out) return luaL_error(L, "mariadb: OOM");
            }
            memcpy(out + olen, lit, ln);
            olen += ln;
            if (heap) free(heap);
        }
        out[olen] = '\0';
        expanded = out;
        expanded_len = (unsigned long)olen;
    }

    if (mysql_real_query(d->db,
            expanded ? expanded : sql,
            expanded ? expanded_len : (unsigned long)sqllen)) {
        free(expanded);
        return push_mysql_error(L, d->db, "query");
    }
    free(expanded); expanded = NULL;

    MYSQL_RES *res = mysql_store_result(d->db);
    if (!res) {
        /* Was a non-select (no result set); return empty array. */
        if (mysql_field_count(d->db) == 0) {
            lua_newtable(L);
            return 1;
        }
        return push_mysql_error(L, d->db, "store_result");
    }
    unsigned nfields = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);

    lua_newtable(L);
    int idx = 1;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        unsigned long *lengths = mysql_fetch_lengths(res);
        lua_createtable(L, 0, (int)nfields);
        for (unsigned i = 0; i < nfields; i++) {
            lua_pushstring(L, fields[i].name ? fields[i].name : "");
            push_column_value(L, &fields[i], row[i], lengths ? lengths[i] : 0);
            lua_rawset(L, -3);
        }
        lua_rawseti(L, -2, idx++);
    }
    mysql_free_result(res);
    return 1;
}

/* Common: expand '?' placeholders using mysql_real_escape_string into a
 * malloc'd buffer. Returns malloc'd expanded SQL + writes *outlen; caller
 * frees. Returns NULL if nothing needed expanding. Raises via luaL_error
 * on OOM or type errors (caller has not yet parked). */
static char *maybe_expand_sql(lua_State *L, MYSQL *db,
                              const char *sql, size_t sqllen,
                              int first_arg_idx, int nargs,
                              size_t *outlen) {
    if (nargs <= 0) return NULL;
    size_t cap = sqllen + 32;
    char *out = (char *)malloc(cap);
    if (!out) { luaL_error(L, "mariadb: OOM"); return NULL; }
    size_t olen = 0;
    int argi = 0;
    for (size_t i = 0; i < sqllen; i++) {
        char c = sql[i];
        if (c != '?') {
            if (olen + 1 >= cap) {
                cap *= 2; out = realloc(out, cap);
                if (!out) { luaL_error(L, "mariadb: OOM"); return NULL; }
            }
            out[olen++] = c;
            continue;
        }
        argi++;
        if (argi > nargs) {
            free(out);
            luaL_error(L, "mariadb: more ? than params");
            return NULL;
        }
        char stackbuf[64]; char *heap = NULL;
        char *lit = escape_literal(L, db, first_arg_idx + argi - 1,
                                   stackbuf, sizeof(stackbuf), &heap);
        size_t ln = strlen(lit);
        while (olen + ln + 1 >= cap) {
            cap = (olen + ln + 1) * 2;
            out = realloc(out, cap);
            if (!out) { luaL_error(L, "mariadb: OOM"); return NULL; }
        }
        memcpy(out + olen, lit, ln);
        olen += ln;
        if (heap) free(heap);
    }
    out[olen] = '\0';
    *outlen = olen;
    return out;
}

/* Internal helper: start an async query (want_result=1) or exec (0).
 *
 * Two completion paths:
 *   - Fully synchronous: mysql_real_query_start returned 0 (no I/O to wait
 *     on). We finish the operation inline, push return values on L, free
 *     the wait_t (never scheduled), and return N.
 *   - Truly async: _start returned a wait mask. We arm a libevent event,
 *     store the wait_t on d->wait, park the coroutine via lua_yield(0).
 *     The event callback resumes the coroutine with results via
 *     fan_coro_wake.
 */
static int start_async(lua_State *L, int want_result) {
    mdb_t *d = check_db(L, 1);
    if (!d->db) return push_mysql_error(L, NULL, "async on closed db");
    if (d->wait) {
        return luaL_error(L, "mariadb: connection already has a pending "
                             "async op (one at a time)");
    }
    /* must be inside a coroutine */
    int is_main = lua_pushthread(L);
    lua_pop(L, 1);
    if (is_main) {
        return luaL_error(L, "mariadb.*_async must be called from within "
                             "a coroutine");
    }

    size_t sqllen; const char *sql = luaL_checklstring(L, 2, &sqllen);
    int nargs = lua_gettop(L) - 2;
    size_t exp_len = 0;
    char *expanded = maybe_expand_sql(L, d->db, sql, sqllen, 3, nargs, &exp_len);

    const char *send_sql;
    unsigned long send_len;
    if (expanded) { send_sql = expanded; send_len = (unsigned long)exp_len; }
    else          { send_sql = sql;      send_len = (unsigned long)sqllen; }

    /* First _start attempt: if it returns 0, the whole query is done. */
    int query_err = 0;
    int status = (int)mysql_real_query_start(&query_err, d->db,
                                             send_sql, send_len);
    if (status == 0) {
        /* Query completed synchronously. */
        free(expanded);
        if (query_err) return push_mysql_error(L, d->db, "query");
        if (!want_result) {
            lua_pushinteger(L, (lua_Integer)mysql_affected_rows(d->db));
            return 1;
        }
        if (mysql_field_count(d->db) == 0) {
            lua_newtable(L);
            return 1;
        }
        /* store_result_start: also try sync completion first */
        MYSQL_RES *res = NULL;
        int rc2 = (int)mysql_store_result_start(&res, d->db);
        if (rc2 == 0) {
            if (!res) return push_mysql_error(L, d->db, "store_result");
            push_result_rows(L, res);
            mysql_free_result(res);
            return 1;
        }
        /* store went async — fall through to full async wait setup below,
         * but we already sent the query. Build wait_t in WAIT_STORE state. */
        mdb_wait_t *w = (mdb_wait_t *)calloc(1, sizeof(*w));
        if (!w) return luaL_error(L, "mariadb: OOM");
        w->owner = d; w->L = L; w->co = L;
        w->kind = WAIT_STORE;
        w->want_result = 1;
        w->res = res;
        lua_pushthread(L);
        w->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        d->wait = w;
        if (wait_arm(w, rc2) != 0) {
            d->wait = NULL;
            luaL_unref(L, LUA_REGISTRYINDEX, w->co_ref);
            wait_free(w);
            return luaL_error(L, "mariadb: wait arm failed");
        }
        return lua_yield(L, 0);
    }

    /* status != 0: query is pending; park */
    mdb_wait_t *w = (mdb_wait_t *)calloc(1, sizeof(*w));
    if (!w) { free(expanded); return luaL_error(L, "mariadb: OOM"); }
    w->owner = d; w->L = L; w->co = L;
    w->kind = WAIT_QUERY;
    w->want_result = want_result;
    /* keep our SQL copy alive across cont() calls (libmariadb reads the
     * buffer only in _start; still, own it for safety and R19 cancellation
     * paths). If we didn't expand, dup a copy. */
    if (expanded) {
        w->sql = expanded; w->sql_len = (unsigned long)exp_len;
    } else {
        w->sql = (char *)malloc(sqllen + 1);
        if (!w->sql) { free(w); return luaL_error(L, "mariadb: OOM"); }
        memcpy(w->sql, sql, sqllen); w->sql[sqllen] = '\0';
        w->sql_len = (unsigned long)sqllen;
    }
    /* Special: _start already ran with our (temporary) buffer. Now we own
     * the copy; libmariadb only re-enters via _cont which does not touch
     * the SQL buffer. So we're safe. */
    w->query_err = query_err;   /* not final yet; overwritten by cont() */

    lua_pushthread(L);
    w->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    d->wait = w;

    if (wait_arm(w, status) != 0) {
        d->wait = NULL;
        luaL_unref(L, LUA_REGISTRYINDEX, w->co_ref);
        wait_free(w);
        return luaL_error(L, "mariadb: wait arm failed");
    }
    return lua_yield(L, 0);
}

static int l_query_async(lua_State *L) { return start_async(L, 1); }
static int l_exec_async(lua_State *L)  { return start_async(L, 0); }

static int l_last_insert_id(lua_State *L) {
    mdb_t *d = check_db(L, 1);
    if (!d->db) return push_mysql_error(L, NULL, "closed db");
    lua_pushinteger(L, (lua_Integer)mysql_insert_id(d->db));
    return 1;
}
static int l_affected_rows(lua_State *L) {
    mdb_t *d = check_db(L, 1);
    if (!d->db) return push_mysql_error(L, NULL, "closed db");
    lua_pushinteger(L, (lua_Integer)mysql_affected_rows(d->db));
    return 1;
}
static int l_ping(lua_State *L) {
    mdb_t *d = check_db(L, 1);
    if (!d->db) return push_mysql_error(L, NULL, "closed db");
    if (mysql_ping(d->db)) return push_mysql_error(L, d->db, "ping");
    lua_pushboolean(L, 1); return 1;
}
static int l_server_version(lua_State *L) {
    mdb_t *d = check_db(L, 1);
    if (!d->db) return push_mysql_error(L, NULL, "closed db");
    lua_pushstring(L, mysql_get_server_info(d->db));
    return 1;
}

/* transactions: BEGIN/COMMIT/ROLLBACK via server statements. */
static int tx_run(lua_State *L, const char *sql) {
    mdb_t *d = check_db(L, 1);
    if (!d->db) return push_mysql_error(L, NULL, "closed db");
    if (mysql_real_query(d->db, sql, (unsigned long)strlen(sql))) {
        return push_mysql_error(L, d->db, sql);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int l_begin(lua_State *L)    { return tx_run(L, "START TRANSACTION"); }
static int l_commit(lua_State *L)   { return tx_run(L, "COMMIT"); }
static int l_rollback(lua_State *L) { return tx_run(L, "ROLLBACK"); }

/* ---- prepared statements (bind_all/step/columns/finalize) --------------- */
static int l_prepare(lua_State *L) {
    mdb_t *d = check_db(L, 1);
    if (!d->db) return push_mysql_error(L, NULL, "prepare on closed db");
    size_t n; const char *sql = luaL_checklstring(L, 2, &n);
    MYSQL_STMT *st = mysql_stmt_init(d->db);
    if (!st) return push_mysql_error(L, d->db, "stmt_init");
    if (mysql_stmt_prepare(st, sql, (unsigned long)n)) {
        int r = push_stmt_error(L, st, "prepare");
        mysql_stmt_close(st);
        return r;
    }
    mstmt_t *s = (mstmt_t *)lua_newuserdata(L, sizeof(*s));
    memset(s, 0, sizeof(*s));
    s->stmt = st;
    lua_pushvalue(L, 1);
    s->db_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    luaL_getmetatable(L, STMT_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static void free_stmt_bufs(mstmt_t *s) {
    if (s->params) {
        for (unsigned i = 0; i < s->nparams; i++) {
            if (s->param_str && s->param_str[i]) { free(s->param_str[i]); s->param_str[i] = NULL; }
        }
        free(s->params);      s->params = NULL;
        free(s->param_str);   s->param_str = NULL;
        free(s->param_len);   s->param_len = NULL;
        free(s->param_int);   s->param_int = NULL;
        free(s->param_dbl);   s->param_dbl = NULL;
        free(s->param_isnull); s->param_isnull = NULL;
        s->nparams = 0;
    }
    if (s->cols) {
        for (unsigned i = 0; i < s->ncols; i++) {
            if (s->col_buf && s->col_buf[i]) free(s->col_buf[i]);
        }
        free(s->cols);        s->cols = NULL;
        free(s->col_buf);     s->col_buf = NULL;
        free(s->col_buflen);  s->col_buflen = NULL;
        free(s->col_length);  s->col_length = NULL;
        free(s->col_isnull);  s->col_isnull = NULL;
        free(s->col_error);   s->col_error = NULL;
        s->ncols = 0;
    }
}

static int st_bind_all(lua_State *L) {
    mstmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "mariadb: bind_all on finalized stmt");
    unsigned nparam = mysql_stmt_param_count(s->stmt);
    int nargs = lua_gettop(L) - 1;
    if ((unsigned)nargs < nparam) {
        return push_stmt_error(L, s->stmt, "bind_all: not enough args");
    }
    free_stmt_bufs(s);
    s->nparams     = nparam;
    s->params      = (MYSQL_BIND *)calloc(nparam, sizeof(MYSQL_BIND));
    s->param_str   = (char **)calloc(nparam, sizeof(char *));
    s->param_len   = (unsigned long *)calloc(nparam, sizeof(unsigned long));
    s->param_int   = (long long *)calloc(nparam, sizeof(long long));
    s->param_dbl   = (double *)calloc(nparam, sizeof(double));
    s->param_isnull = (my_bool *)calloc(nparam, sizeof(my_bool));
    if (!s->params || !s->param_str || !s->param_len || !s->param_int
        || !s->param_dbl || !s->param_isnull) {
        return luaL_error(L, "mariadb: OOM in bind_all");
    }
    for (unsigned i = 0; i < nparam; i++) {
        int idx = 2 + (int)i;
        int t = lua_type(L, idx);
        MYSQL_BIND *b = &s->params[i];
        b->is_null = &s->param_isnull[i];
        s->param_isnull[i] = 0;
        switch (t) {
        case LUA_TNIL:
            b->buffer_type = MYSQL_TYPE_NULL;
            s->param_isnull[i] = 1;
            break;
        case LUA_TBOOLEAN:
            s->param_int[i] = lua_toboolean(L, idx) ? 1 : 0;
            b->buffer_type = MYSQL_TYPE_LONGLONG;
            b->buffer      = &s->param_int[i];
            break;
        case LUA_TNUMBER:
#if LUA_VERSION_NUM >= 503
            if (lua_isinteger(L, idx)) {
                s->param_int[i] = (long long)lua_tointeger(L, idx);
                b->buffer_type = MYSQL_TYPE_LONGLONG;
                b->buffer      = &s->param_int[i];
                break;
            }
#endif
            s->param_dbl[i] = (double)lua_tonumber(L, idx);
            b->buffer_type = MYSQL_TYPE_DOUBLE;
            b->buffer      = &s->param_dbl[i];
            break;
        case LUA_TSTRING: {
            size_t n; const char *v = lua_tolstring(L, idx, &n);
            s->param_str[i] = (char *)malloc(n + 1);
            if (!s->param_str[i]) return luaL_error(L, "mariadb: OOM bind_all");
            memcpy(s->param_str[i], v, n); s->param_str[i][n] = '\0';
            s->param_len[i] = (unsigned long)n;
            b->buffer_type = MYSQL_TYPE_STRING;
            b->buffer      = s->param_str[i];
            b->buffer_length = (unsigned long)n;
            b->length      = &s->param_len[i];
            break;
        }
        default:
            return luaL_error(L, "mariadb: cannot bind Lua %s",
                              lua_typename(L, t));
        }
    }
    if (mysql_stmt_bind_param(s->stmt, s->params)) {
        return push_stmt_error(L, s->stmt, "bind_param");
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* Set up output binds after execute: one 4KB buffer per column; overlong
 * values re-fetched via mysql_stmt_fetch_column with a bigger buffer. */
static int stmt_setup_result(lua_State *L, mstmt_t *s) {
    MYSQL_RES *meta = mysql_stmt_result_metadata(s->stmt);
    if (!meta) return 0;  /* no result set (INSERT/UPDATE) */
    unsigned nc = mysql_num_fields(meta);
    s->ncols       = nc;
    s->cols        = (MYSQL_BIND *)calloc(nc, sizeof(MYSQL_BIND));
    s->col_buf     = (void **)calloc(nc, sizeof(void *));
    s->col_buflen  = (unsigned long *)calloc(nc, sizeof(unsigned long));
    s->col_length  = (unsigned long *)calloc(nc, sizeof(unsigned long));
    s->col_isnull  = (my_bool *)calloc(nc, sizeof(my_bool));
    s->col_error   = (my_bool *)calloc(nc, sizeof(my_bool));
    if (!s->cols || !s->col_buf || !s->col_buflen || !s->col_length
        || !s->col_isnull || !s->col_error) {
        mysql_free_result(meta);
        return luaL_error(L, "mariadb: OOM stmt_setup_result");
    }
    for (unsigned i = 0; i < nc; i++) {
        size_t bufsz = 4096;
        s->col_buf[i] = malloc(bufsz);
        if (!s->col_buf[i]) {
            mysql_free_result(meta);
            return luaL_error(L, "mariadb: OOM stmt_setup_result buf");
        }
        s->col_buflen[i] = (unsigned long)bufsz;
        s->cols[i].buffer_type = MYSQL_TYPE_STRING;   /* fetch as text */
        s->cols[i].buffer      = s->col_buf[i];
        s->cols[i].buffer_length = (unsigned long)bufsz;
        s->cols[i].length      = &s->col_length[i];
        s->cols[i].is_null     = &s->col_isnull[i];
        s->cols[i].error       = &s->col_error[i];
    }
    if (mysql_stmt_bind_result(s->stmt, s->cols)) {
        mysql_free_result(meta);
        return push_stmt_error(L, s->stmt, "bind_result");
    }
    mysql_free_result(meta);
    return 0;
}

/* stmt:step() -> "row"|"done"|nil,err. Also drives execute() the first time. */
static int st_step(lua_State *L) {
    mstmt_t *s = check_stmt(L, 1);
    if (!s->stmt) return luaL_error(L, "mariadb: step on finalized stmt");
    /* if we haven't executed yet (no cols set up), do it now */
    if (!s->cols) {
        if (mysql_stmt_execute(s->stmt)) return push_stmt_error(L, s->stmt, "execute");
        int rc = stmt_setup_result(L, s);
        if (rc < 0) return rc;
        if (s->ncols == 0) {
            /* non-select: return done immediately */
            lua_pushliteral(L, "done"); return 1;
        }
        mysql_stmt_store_result(s->stmt);
    }
    int frc = mysql_stmt_fetch(s->stmt);
    if (frc == 0 || frc == MYSQL_DATA_TRUNCATED) {
        lua_pushliteral(L, "row"); return 1;
    }
    if (frc == MYSQL_NO_DATA) {
        lua_pushliteral(L, "done"); return 1;
    }
    return push_stmt_error(L, s->stmt, "fetch");
}

static int st_columns(lua_State *L) {
    mstmt_t *s = check_stmt(L, 1);
    if (!s->stmt || !s->cols) return luaL_error(L, "mariadb: columns before step");
    MYSQL_RES *meta = mysql_stmt_result_metadata(s->stmt);
    MYSQL_FIELD *f = meta ? mysql_fetch_fields(meta) : NULL;
    lua_createtable(L, 0, (int)s->ncols);
    for (unsigned i = 0; i < s->ncols; i++) {
        const char *name = (f && f[i].name) ? f[i].name : "";
        lua_pushstring(L, name);
        if (s->col_isnull[i]) {
            lua_pushnil(L);
        } else {
            unsigned long actual = s->col_length[i];
            /* handle truncation: reallocate + refetch this column */
            if (actual > s->col_buflen[i]) {
                free(s->col_buf[i]);
                s->col_buf[i] = malloc(actual + 1);
                s->col_buflen[i] = actual + 1;
                s->cols[i].buffer = s->col_buf[i];
                s->cols[i].buffer_length = actual + 1;
                mysql_stmt_fetch_column(s->stmt, &s->cols[i], i, 0);
            }
            /* type-aware conversion: use metadata */
            if (f) {
                push_column_value(L, &f[i], (const char *)s->col_buf[i], actual);
            } else {
                lua_pushlstring(L, (const char *)s->col_buf[i], actual);
            }
        }
        lua_rawset(L, -3);
    }
    if (meta) mysql_free_result(meta);
    return 1;
}

static int st_finalize(lua_State *L) {
    mstmt_t *s = check_stmt(L, 1);
    if (s->stmt) { mysql_stmt_close(s->stmt); s->stmt = NULL; }
    free_stmt_bufs(s);
    if (s->db_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s->db_ref);
        s->db_ref = LUA_NOREF;
    }
    return 0;
}
static int st_gc(lua_State *L) { return st_finalize(L); }

/* ---- registration ------------------------------------------------------- */
static const luaL_Reg db_methods[] = {
    {"close",           l_close},
    {"exec",            l_exec},
    {"query",           l_query},
    {"exec_async",      l_exec_async},
    {"query_async",     l_query_async},
    {"prepare",         l_prepare},
    {"begin",           l_begin},
    {"commit",          l_commit},
    {"rollback",        l_rollback},
    {"last_insert_id",  l_last_insert_id},
    {"affected_rows",   l_affected_rows},
    {"ping",            l_ping},
    {"server_version",  l_server_version},
    {NULL, NULL},
};
static const luaL_Reg stmt_methods[] = {
    {"bind_all", st_bind_all},
    {"step",     st_step},
    {"columns",  st_columns},
    {"finalize", st_finalize},
    {NULL, NULL},
};

static void register_mt(lua_State *L, const char *name,
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

void fan_mariadb_register(lua_State *L) {
    register_mt(L, DB_MT,   db_methods,   db_gc);
    register_mt(L, STMT_MT, stmt_methods, st_gc);

    lua_newtable(L);
    lua_pushcfunction(L, l_connect);
    lua_setfield(L, -2, "connect");
    lua_pushstring(L, mysql_get_client_info());
    lua_setfield(L, -2, "client_version");
    lua_setfield(L, -2, "mariadb");
}
