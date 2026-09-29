/*
 * db/db_sqlite3.h — LuaFan v2 SQLite3 binding registration.
 *
 * Registered as fan.sqlite3 with (M16.1 — lsqlite3-compatible surface):
 *
 * Module:
 *   fan.sqlite3.open(path [, flags]) -> db
 *   fan.sqlite3.version              (SQLite library version string)
 *   fan.sqlite3.complete(sql)        (heuristic: is sql statement-complete)
 *   Constants: OPEN_* + result-code (OK/ERROR/BUSY/LOCKED/ROW/DONE/CONSTRAINT
 *   /CORRUPT/READONLY/NOTFOUND/FULL/CANTOPEN/PROTOCOL/EMPTY/SCHEMA/TOOBIG/
 *   MISMATCH/MISUSE/NOLFS/AUTH/FORMAT/RANGE/NOTADB) + column-type
 *   (INTEGER/FLOAT/TEXT/BLOB/NULL)
 *
 * db methods:
 *   db:close()
 *   db:exec(sql) / db:execute(sql)   -- alias; returns rows_affected or nil,err
 *   db:query(sql [, params...])      -- luafan2 shortcut: array of row-maps
 *   db:nrows(sql) / db:rows(sql) / db:urows(sql)  -- lsqlite3-style iterators
 *   db:prepare(sql) -> stmt
 *   db:begin() / db:commit() / db:rollback()   -- luafan2 convenience wrappers
 *   db:last_insert_rowid()
 *   db:changes() / db:total_changes()
 *   db:busy_timeout(ms)
 *   db:interrupt()
 *   db:errcode() / db:errmsg()
 *   db:isopen()
 *   db:get_autocommit()
 *
 * stmt methods:
 *   stmt:bind(idx, val)
 *   stmt:bind_all(...) / stmt:bind_values(...)   -- alias
 *   stmt:bind_names(t)               -- named parameters (":name" / "@name" / "$name")
 *   stmt:bind_parameter_count()
 *   stmt:bind_parameter_name(i)
 *   stmt:step() -> sqlite3.ROW | sqlite3.DONE | nil,err   -- M16.1: numeric (lsqlite3-compat)
 *   stmt:reset()
 *   stmt:finalize()
 *   stmt:isopen()
 *   stmt:columns()                   -- lsqlite3: returns column COUNT (integer)
 *   stmt:get_names()                 -- array of column names
 *   stmt:get_types()                 -- array of declared column types
 *   stmt:nrows() / stmt:rows() / stmt:urows()    -- iterators over remaining rows
 *
 * Note: file is deliberately named db_sqlite3.h (not sqlite3.h) to avoid
 * clashing with the system <sqlite3.h> during a "#include <sqlite3.h>" that
 * happens to see src/db on the include path first.
 *
 * Values: NULL -> nil, INTEGER -> lua integer, REAL -> lua number,
 *         TEXT/BLOB -> lua string.
 * Bindings: nil/boolean/integer/number/string are accepted; anything else errors.
 */
#ifndef FAN2_DB_SQLITE3_H
#define FAN2_DB_SQLITE3_H
#include <lua.h>
void fan_sqlite3_register(lua_State *L);
#endif
