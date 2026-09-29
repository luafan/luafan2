# fan.orm — active-record ORM base (M5 + M16.2)

`fan.orm` is a driver-agnostic ORM base that runs on top of any db handle
that implements a small driver contract.  Two driver adapters ship in
the module itself:

- `orm.sqlite_driver(db)` — over `fan.sqlite3`
- `orm.mariadb_driver(db)` — over `fan.mariadb`

Both share exactly the same `Model` API, so switching backends only
changes the driver constructor.

## The 20-second overview

```lua
local sqlite = require("fan.sqlite3")
local orm    = require("fan.orm")

local db  = sqlite.open("app.db")
local ctx = orm.new_context(orm.sqlite_driver(db))

-- Schema definition: column_name = "SQL TYPE"; schema.pk names the pk
-- column (default "id").  Missing columns are added by ALTER TABLE ADD
-- on re-define — the migration story.
local User = ctx:define("users", {
  id    = "INTEGER PRIMARY KEY AUTOINCREMENT",
  name  = "TEXT NOT NULL",
  email = "TEXT",
  age   = "INTEGER",
})

-- Insert returns the fresh row as a live object (M16.2):
local u = User.insert{ name = "alice", email = "a@x", age = 30 }
print(u.id)             -- 1 (from last_insert_rowid)

-- Mutate + auto-diff:
u.email = "alice@example.com"
u:update()              -- only UPDATEs `email` (only changed column)

-- Explicit column set:
u:update{ age = 31 }    -- UPDATEs just `age` regardless of diff state

-- Delete + detach:
u:delete()              -- DELETE WHERE id = u.id
-- After :delete(), u loses its metatable — further method calls error.

-- Query:
local one   = User.find_by{ name = "bob" }      -- live row, or nil
local all   = User.list{ where = { age = 30 } } -- array of live rows
local plain = User.list{ raw = true }           -- array of plain maps (no metatable)
local rows  = User.raw_query("SELECT COUNT(*) AS n FROM users")  -- always plain

-- Legacy id-based helpers still work:
User.update(1, { age = 32 })
User.delete(1)

-- Transactions:
ctx:transaction(function()
  User.insert{ name = "dave" }
  User.insert{ name = "eve" }
end)   -- pcall'd: on any Lua error, rollback + return nil, err
```

## Active-record row objects (M16.2)

Rows returned by `Model.insert`, `Model.find_by`, and `Model.list` carry
a metatable that gives them three methods that talk back to the database.

### `row:update([override])`

- **Zero-arg auto-diff** (v1-era default): compares each schema-known
  column against the baseline snapshot taken at load time; emits
  `UPDATE ... SET col = ?, ... WHERE pk = ?` for the columns whose value
  actually changed.  Returns rows_affected (which is `0` when nothing
  changed, i.e. no SQL is issued at all).  After a successful update
  the baseline is refreshed to the new state, so successive
  `:update()` calls diff against the last-flushed state.
- **Override mode**: `row:update{ col = val, ... }` UPDATEs exactly the
  columns supplied, regardless of the current diff state, and syncs
  those new values into both `self` and the baseline.  The pk column
  can be renamed this way — the WHERE clause snapshots the old pk
  before mutating `self`.

Return value on error is `(nil, err)`, matching every other orm method.

### `row:delete()` / `row:remove()`

Emit `DELETE FROM t WHERE pk = ?` for `self[pk_field]`, then detach the
row (call `setmetatable(self, nil)` and forget its baseline).  Post-delete,
`self` is a plain Lua table whose column values are still readable but
whose methods are gone — calling `self:update()` fails cleanly with
"attempt to call a nil value (method 'update')" instead of silently
issuing a stale UPDATE.  `:remove` is a v1-era alias.

### Baseline storage: implementation note

The pre-mutation snapshot lives in a **weak-key table attached to the
model's row metatable**: `row_mt.__attr_map[row] = { col=val, ... }`.
Weak keys let the snapshot be garbage-collected automatically when the
row is unreferenced — no explicit cleanup needed.  Keeping the snapshot
in the metatable rather than inside the row itself means the row's own
key space stays clean: `for k, v in pairs(row) do ... end` iterates
only the SQL columns.

This differs from v1's approach (a fixed private string key
`r["^attr"]`) — same effect, but no risk of colliding with a hypothetical
column named `^attr`, and no need to teach every reader what the
`KEY_*` constants at the top of `orm_base.lua` are for.

## Escape hatches

Sometimes you want plain-map rows — large read-only bulk reports, or
columns not attached to a single well-defined table:

```lua
User.list{ raw = true }        -- plain maps, ipairs-friendly, no metatable overhead
User.raw_query("SELECT ...")   -- always plain (semantics unknown, no pk)
```

The metatable-installed methods (`:update`, `:delete`, `:remove`) will
NOT be reachable from these rows — that's the point.  Fall back to the
class-level helpers `Model.update(id, fields)` / `Model.delete(id)` when
you need to write.

## Driver contract

If you want to run fan.orm against a database driver other than
fan.sqlite3 / fan.mariadb (say, lsqlite3 or luasql), write an adapter
table with these methods (see `sqlite_driver` in `lua/fan/orm.lua` for
a working ~30-line example):

```
driver:exec(sql)              -> rows_affected  or  nil, err
driver:query(sql, ...)        -> array of row-maps  or  nil, err
driver:exec_with_args(sql,args) -> rows_affected  or  nil, err
driver:last_insert_rowid()    -> integer
driver:begin() / :commit() / :rollback()
driver:quote_ident(name)      -> string (backticks or double-quotes)
driver:placeholder(i)         -> "?" or "$1" etc.
driver:default_pk_type()      -> "INTEGER PRIMARY KEY AUTOINCREMENT" (SQLite)
                                 or "BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT" (MariaDB)
driver:table_info(name)       -> array of { name=..., type=... } for the ADD COLUMN migration
```

## Test coverage

- `tests/lua/test_orm.lua`         — base contract (define/insert/find_by/list/update/delete/migration/transaction) against SQLite in-memory
- `tests/lua/test_orm_activerow.lua` — M16.2 active-record methods:
  auto-diff, override mode, no-op returns 0, baseline refresh across
  successive updates, delete + detach, remove alias, find_by/list return
  live rows, list{raw=true} escape hatch, raw_query is plain, legacy
  Model.update/delete still work, pk aliasing (schema.pk = "uuid")
- `tests/lua/test_orm_mariadb.lua`  — same contract against MariaDB
- `tests/lua/test_integration.lua`  — REST /users CRUD end-to-end
  (httpd_c + orm + sqlite3 + json + http_c) and mariadb-pool + orm
  concurrency

M16.2 raised Lua coverage from 90.25% to 90.36%.

## Not yet implemented (v1 gap)

The following v1 ORM features are not present in v2:

- **Callback iteration**: `ctx.users(function(row) ... end, "where ...")`
  for streaming large result sets without loading them all into memory.
  Currently the only way is `list{}` + `for _, row in ipairs(rows)` (all
  in RAM) or drop down to `driver.db:nrows(sql)` directly.
- **Column-level `__call`**: `ctx.users.name("=?", "alice")` shortcut.
  Use `User.find_by{name="alice"}` or `User.raw_query(...)`.
- **`fmt` string on select**: `ctx.users("select", "where age > ?", 18)`.
  Currently `list{where=...}` supports only equality-AND.  Non-equality
  filters need `raw_query`.
- **MariaDB built-in values**: `BUILTIN_VALUE_NOW = "NOW()"` insert-time
  sentinel; **LONG_DATA** chunked BLOB bind; **readonly context**.
- **`ctx:select / :update / :delete / :insert`** context-level raw
  helpers (only per-model `raw_query` exists).

These are all additive and could be reintroduced in a future M16.x
without breaking the M16.2 API.
