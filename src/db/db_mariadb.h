/*
 * db/db_mariadb.h — LuaFan v2 MariaDB binding (synchronous first phase).
 *
 * M5.5.a scope: synchronous, single-connection API sufficient to bring up
 * the ORM base against a MariaDB driver and validate the shared contract
 * against SQLite. M5.5.b will layer libevent-integrated async wait on top
 * of the same C surface via mysql_*_start / mysql_*_cont; the R19/R13
 * regressions live there.
 *
 * Registered as fan.mariadb with:
 *   db = fan.mariadb.connect{
 *     host=..., port=..., user=..., password=..., database=...,
 *     unix_socket=..., charset=..., autocommit=true/false }
 *   db:close()
 *   db:exec(sql)                             -> rows_affected or nil,err
 *   db:query(sql [, params...])              -> array of row-maps or nil,err
 *   db:prepare(sql) -> stmt (bind_all/step/columns/finalize)
 *   db:begin() / db:commit() / db:rollback()
 *   db:last_insert_id()
 *   db:affected_rows()
 *   db:ping()
 *   db:server_version()
 */
#ifndef FAN2_DB_MARIADB_H
#define FAN2_DB_MARIADB_H
#include <lua.h>
void fan_mariadb_register(lua_State *L);
#endif
