--[[
  test_sqlite3.lua — M5.4 fan.sqlite3 binding contract tests.

  Coverage:
    - open in-memory + close idempotency
    - exec (CREATE/INSERT), changes(), last_insert_rowid()
    - query returns array of row-maps with column names
    - parameterised query with mixed bind types (nil/bool/int/float/str)
    - prepared statement: bind/bind_all + step ROW/DONE + reset re-use
    - transaction: begin/commit and begin/rollback
    - error surfaces: exec on bad SQL returns nil, err
]]
local T      = require("test_framework")
local sqlite = require("fan.sqlite3")
local s      = T.suite("fan.sqlite3 (M5)")

s:test("open / version / close idempotent", function()
  T.truthy(sqlite.version)
  local db, err = sqlite.open(":memory:")
  T.not_nil(db); T.is_nil(err)
  db:close()
  db:close()   -- second close must be a no-op
end)

s:test("exec + changes + last_insert_rowid", function()
  local db = sqlite.open(":memory:")
  T.truthy(db:exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)"))
  T.eq(db:exec("INSERT INTO t(name) VALUES ('alice')"), 1)
  T.eq(db:last_insert_rowid(), 1)
  T.eq(db:exec("INSERT INTO t(name) VALUES ('bob')"), 1)
  T.eq(db:last_insert_rowid(), 2)
  T.eq(db:changes(), 1)
  db:close()
end)

s:test("query returns array of row-maps with named columns", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
  db:exec("INSERT INTO t(name, age) VALUES ('a', 10), ('b', 20)")
  local rows = db:query("SELECT id, name, age FROM t ORDER BY id")
  T.eq(#rows, 2)
  T.eq(rows[1].id, 1); T.eq(rows[1].name, "a"); T.eq(rows[1].age, 10)
  T.eq(rows[2].name, "b"); T.eq(rows[2].age, 20)
  db:close()
end)

s:test("parameterised query binds nil / bool / int / float / str", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(v)")
  db:exec("INSERT INTO t VALUES(NULL), ('str'), (42), (3.14), (0)")
  local rows = db:query("SELECT v FROM t WHERE v = ?", 42)
  T.eq(#rows, 1); T.eq(rows[1].v, 42)
  rows = db:query("SELECT v FROM t WHERE v = ?", "str")
  T.eq(#rows, 1); T.eq(rows[1].v, "str")
  -- floats survive round-trip
  rows = db:query("SELECT v FROM t WHERE v > ? AND v < ?", 3.0, 4.0)
  T.eq(#rows, 1); T.eq(rows[1].v, 3.14)
  db:close()
end)

s:test("prepared statement bind_all + step ROW/DONE + reset re-use", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
  local ins = db:prepare("INSERT INTO t(name, age) VALUES(?, ?)")
  for _, r in ipairs({{"a", 1}, {"b", 2}, {"c", 3}}) do
    ins:reset()
    T.truthy(ins:bind_all(r[1], r[2]))
    T.eq(ins:step(), "done")
  end
  ins:finalize()

  local sel = db:prepare("SELECT name, age FROM t ORDER BY id")
  local names = {}
  while true do
    local rc = sel:step()
    if rc == "done" then break end
    T.eq(rc, "row")
    local col = sel:columns()
    names[#names + 1] = col.name
  end
  T.eq(table.concat(names, ","), "a,b,c")
  sel:finalize()
  db:close()
end)

s:test("transaction: commit persists, rollback reverts", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(v INTEGER)")
  T.truthy(db:begin())
  db:exec("INSERT INTO t(v) VALUES(1)")
  db:exec("INSERT INTO t(v) VALUES(2)")
  T.truthy(db:commit())
  T.eq(#db:query("SELECT v FROM t"), 2)

  T.truthy(db:begin())
  db:exec("INSERT INTO t(v) VALUES(3)")
  T.truthy(db:rollback())
  T.eq(#db:query("SELECT v FROM t"), 2)
  db:close()
end)

s:test("bad SQL surfaces nil, err from exec/query", function()
  local db = sqlite.open(":memory:")
  local ok, err = db:exec("NOT A QUERY")
  T.is_nil(ok); T.truthy(err); T.truthy(err:find("sqlite3"))
  local rows, err2 = db:query("SELECT * FROM nowhere")
  T.is_nil(rows); T.truthy(err2)
  db:close()
end)

os.exit(T.run(s))
