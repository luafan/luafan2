--[[
  test_lsqlite3_compat.lua — M16.1 lsqlite3 compatibility smoke.

  Purpose: prove that fan.sqlite3 can serve as a drop-in replacement for
  the LuaRocks `lsqlite3` binding in idiomatic v1-era code.  The body of
  this test uses ONLY lsqlite3-style API (naming, iterator shapes, numeric
  result codes) — no fan.sqlite3-native shortcuts (no db:query, no
  db:begin/commit/rollback wrappers).  If this test passes, migrating
  legacy `require "lsqlite3"` code to `require "fan.sqlite3"` should be
  a one-line change.

  The test is deliberately structured as a mini-program (not many small
  unit tests), because the point is that the whole flow — open → create
  → prepared insert with bind_values → nrows iteration → transactional
  BEGIN/COMMIT via db:execute — works together without any per-call
  translation.
]]
local T      = require("test_framework")
local sqlite = require("fan.sqlite3")     -- <<< the one line that would say lsqlite3 in v1
local s      = T.suite("lsqlite3 compat smoke (M16.1)")

s:test("full lsqlite3-style CRUD flow works verbatim under fan.sqlite3", function()
  -- lsqlite3-style open
  local db = assert(sqlite.open(":memory:"))

  -- Schema via db:execute (lsqlite3 primary name for the exec-no-params op)
  T.truthy(db:execute[[
    CREATE TABLE users (
      id    INTEGER PRIMARY KEY AUTOINCREMENT,
      name  TEXT NOT NULL,
      email TEXT
    )
  ]])

  -- Prepared insert with lsqlite3 name (bind_values) and numeric step compare
  local ins = assert(db:prepare("INSERT INTO users(name, email) VALUES(?, ?)"))
  local seed = {
    {"alice",  "a@x"},
    {"bob",    "b@x"},
    {"carol",  nil},           -- NULL email
  }
  for _, r in ipairs(seed) do
    ins:reset()
    assert(ins:bind_values(r[1], r[2]))
    T.eq(ins:step(), sqlite.DONE)          -- lsqlite3 numeric-compare style
  end
  ins:finalize()

  T.eq(db:last_insert_rowid(), 3)

  -- Query via prepare + nrows (the lsqlite3 idiomatic read pattern).
  local sel = assert(db:prepare("SELECT id, name, email FROM users ORDER BY id"))
  local names = {}
  for row in sel:nrows() do              -- row is {id=..., name=..., email=...}
    names[#names + 1] = row.name
  end
  sel:finalize()
  T.eq(table.concat(names, ","), "alice,bob,carol")

  -- Named parameters (":name" style) — lsqlite3 supports these; we do too.
  local upd = assert(db:prepare("UPDATE users SET email = :em WHERE name = :n"))
  T.truthy(upd:bind_names{ em = "a2@x", n = "alice" })
  T.eq(upd:step(), sqlite.DONE)
  upd:finalize()

  T.eq(db:changes(), 1)

  -- lsqlite3-style transaction via db:execute("BEGIN") / COMMIT
  T.truthy(db:execute("BEGIN"))
  db:execute("INSERT INTO users(name) VALUES('dave')")
  db:execute("INSERT INTO users(name) VALUES('eve')")
  T.truthy(db:execute("COMMIT"))
  T.eq(db:total_changes(), 6)            -- 3 inserts + 1 update + 2 inserts

  -- Rollback path
  T.truthy(db:execute("BEGIN"))
  db:execute("INSERT INTO users(name) VALUES('to-be-rolled-back')")
  T.truthy(db:execute("ROLLBACK"))

  -- Iterate the final state via db:nrows shortcut (also present in lsqlite3)
  local final = {}
  for row in db:nrows("SELECT name FROM users ORDER BY id") do
    final[#final + 1] = row.name
  end
  T.eq(table.concat(final, ","), "alice,bob,carol,dave,eve")

  -- urows unpacked-return style
  local ids = {}
  for id in db:urows("SELECT id FROM users ORDER BY id") do
    ids[#ids + 1] = id
  end
  T.eq(#ids, 5); T.eq(ids[1], 1); T.eq(ids[5], 5)

  -- errcode / errmsg after intentional bad SQL
  local ok = db:execute("SELECT * FROM nowhere")
  T.is_nil(ok)
  T.truthy(db:errcode() ~= sqlite.OK)
  T.truthy(db:errmsg())

  db:close()
  T.eq(db:isopen(), false)
end)

s:test("column metadata via stmt:columns / get_names / get_types", function()
  local db = assert(sqlite.open(":memory:"))
  db:execute("CREATE TABLE t(id INTEGER, label TEXT, ratio REAL, blob BLOB)")
  db:execute("INSERT INTO t VALUES(1, 'x', 0.5, x'DEAD')")
  local st = assert(db:prepare("SELECT id, label, ratio, blob FROM t"))
  T.eq(st:columns(), 4)
  local names = st:get_names()
  T.eq(names[1], "id");    T.eq(names[2], "label")
  T.eq(names[3], "ratio"); T.eq(names[4], "blob")
  local types = st:get_types()
  T.eq(types[1], "INTEGER"); T.eq(types[2], "TEXT")
  T.eq(types[3], "REAL");    T.eq(types[4], "BLOB")
  st:finalize()
  db:close()
end)

os.exit(T.run(s))
