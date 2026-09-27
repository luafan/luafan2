/*
 * db/db_sqlite3.h — LuaFan v2 SQLite3 binding registration.
 *
 * Registered as fan.sqlite3 with:
 *   db = fan.sqlite3.open(path [, flags])
 *   db:close()
 *   db:exec(sql)                             -- returns rows_affected or nil,err
 *   db:query(sql [, params...])              -- returns array of rows (map), or nil,err
 *   db:prepare(sql) -> stmt                  -- for repeated execution
 *     stmt:bind(idx, val) / stmt:bind_all(...)
 *     stmt:step() -> "row" | "done" | nil,err
 *     stmt:columns() -> map                  -- current row as string->value
 *     stmt:reset()
 *     stmt:finalize()
 *   db:begin() / db:commit() / db:rollback()
 *   db:last_insert_rowid()
 *   db:changes()
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
