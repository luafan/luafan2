# fan.sqlite3 — SQLite3 driver (M5 + M16.1)

`fan.sqlite3` is luafan2's native SQLite3 binding, registered as a fan
submodule and requiring **zero LuaRocks dependencies** at runtime.  Only
the system `libsqlite3` C library is needed.

As of M16.1 the public API surface is a **drop-in replacement for the
LuaRocks `lsqlite3` binding**: legacy v1-style code that uses
`require "lsqlite3"` can migrate by changing only the `require` line, and
the numeric result codes, iterator shapes, named-parameter bind, and
column metadata reflection all match lsqlite3.  A small set of
convenience methods that lsqlite3 lacks (`db:query`, `db:begin/commit/
rollback` wrappers) are kept as strict additions.

## Build

```
cmake -DFAN_WITH_SQLITE3=ON ...
```

All release images (`Dockerfile.release.ubuntu`, `Dockerfile.release.alpine`)
and the CI test image ship with SQLite3 enabled.  `fan.sqlite3` sits inside
`fan.so` so it enjoys the M15 `RTLD_LOCAL` dependency isolation (libsqlite3
lives entirely in fan.so's namespace, not the executable's).

## Module surface

```
fan.sqlite3.open(path [, flags]) -> db  or  nil, err
fan.sqlite3.version              — SQLite library version string
fan.sqlite3.complete(sql)        — sqlite3_complete() heuristic
```

### Result-code constants (numeric, matching SQLite C API)

```
OK ERROR INTERNAL PERM ABORT BUSY LOCKED NOMEM READONLY INTERRUPT IOERR
CORRUPT NOTFOUND FULL CANTOPEN PROTOCOL EMPTY SCHEMA TOOBIG CONSTRAINT
MISMATCH MISUSE NOLFS AUTH FORMAT RANGE NOTADB ROW DONE
```

### Storage-class constants

```
INTEGER FLOAT TEXT BLOB NULL
```

### Open-flag constants

```
OPEN_READONLY OPEN_READWRITE OPEN_CREATE OPEN_URI OPEN_MEMORY
```

## Database methods

| Method | Notes |
|---|---|
| `db:close()` | Idempotent; safe to call twice. |
| `db:exec(sql)` / `db:execute(sql)` | Returns `changes` on success, `nil,err` on failure.  No bind params — use `prepare` or `query` for parameterised. |
| `db:query(sql [, params...])` | luafan2 shortcut: array of row-maps.  Not in lsqlite3. |
| `db:nrows(sql)` / `db:rows(sql)` / `db:urows(sql)` | Iterators — map / array / unpacked returns.  Auto-finalize on end. |
| `db:prepare(sql) -> stmt` | Returns a stmt userdata for repeated execution. |
| `db:begin()` / `db:commit()` / `db:rollback()` | luafan2 convenience wrappers; equivalent to `db:exec("BEGIN"/"COMMIT"/"ROLLBACK")`.  Not in lsqlite3. |
| `db:last_insert_rowid()` | |
| `db:changes()` | Rows affected by the most recent statement. |
| `db:total_changes()` | Cumulative rows changed since the connection opened. |
| `db:busy_timeout(ms)` | Wraps `sqlite3_busy_timeout`.  **Required in production for multi-writer databases.** |
| `db:interrupt()` | Cancel a long-running query (call from another thread). |
| `db:errcode()` / `db:errmsg()` | Reflection of the last SQLite error on this handle. |
| `db:isopen()` | Boolean. |
| `db:get_autocommit()` | Boolean; false while inside a transaction. |

## Statement methods

| Method | Notes |
|---|---|
| `stmt:bind(idx, val)` | Bind one 1-based positional parameter. |
| `stmt:bind_all(...)` / `stmt:bind_values(...)` | Bind all positional `?` parameters at once.  `bind_values` is the lsqlite3-compatible alias. |
| `stmt:bind_names(t)` | Named parameters (`:name` / `@name` / `$name`).  Keys may be given with or without the prefix character.  Missing keys bind NULL. |
| `stmt:bind_parameter_count()` | |
| `stmt:bind_parameter_name(i)` | Full parameter name with prefix, or nil for `?`. |
| `stmt:step() -> sqlite3.ROW \| sqlite3.DONE \| nil,err` | **Numeric return** (M16.1 change from earlier "row"/"done" strings). |
| `stmt:reset()` | Reset stmt + clear bindings; ready for another execution. |
| `stmt:finalize()` | Idempotent; safe. |
| `stmt:isopen()` | Boolean. |
| `stmt:columns()` | **Column COUNT (integer)**, matching lsqlite3.  Was pre-M16.1 "current row map"; that role is now covered by `stmt:nrows`. |
| `stmt:get_names()` | Array of column names. |
| `stmt:get_types()` | Array of declared column types (from CREATE TABLE) — may be empty string for computed columns. |
| `stmt:nrows()` / `stmt:rows()` / `stmt:urows()` | Iterators over the *remaining* rows.  Do NOT finalize the stmt on end (unlike `db:nrows(sql)` above); the caller keeps ownership. |

## Migration from lsqlite3

For code originally written against `require "lsqlite3"`, only the module
name changes:

```diff
-local sqlite3 = require("lsqlite3")
+local sqlite3 = require("fan.sqlite3")
```

Everything else — `sqlite3.open`, `db:execute`, `db:prepare`, `stmt:bind_values`,
`stmt:step() == sqlite3.DONE`, `for row in stmt:nrows() do ... end`,
`stmt:bind_names{ ... }`, `db:busy_timeout`, `db:errcode` / `db:errmsg`,
`sqlite3.ROW`/`DONE`/`BUSY`/... constants — is unchanged.

The `tests/lua/test_lsqlite3_compat.lua` smoke test proves this: its body
is written in the pure lsqlite3 idiom (no fan.sqlite3-specific shortcuts)
and passes verbatim.

## Not yet implemented

The following lsqlite3 APIs are **not** in M16.1 and require an explicit
follow-up milestone (M16.2 / M16.3 / M16.4):

| API | Milestone (planned) |
|---|---|
| `db:create_function(name, nargs, fn)` — SQL-callable Lua funcs | M16.2 |
| `db:create_aggregate` / `create_collation` | M16.2 |
| `db:load_extension` / `enable_load_extension` — FTS5, spatialite, sqlite-vss | M16.3 |
| `db:backup` / `db:restore` — online backup API | M16.3 |
| `db:trace` / `db:profile` / `db:update_hook` / `db:commit_hook` / `db:rollback_hook` / `db:progress_handler` | M16.4 |

These are additive and will not change any existing API.

## Related

- `fan.orm` — driver-agnostic ORM base with a `sqlite_driver` adapter over
  this module.  See `lua/fan/orm.lua`.
- Test coverage lives in `tests/lua/test_sqlite3.lua` (21 API contract
  tests) + `tests/lua/test_lsqlite3_compat.lua` (integration-style smoke
  proving drop-in compat).
