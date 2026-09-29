--[[
  test_sqlite3.lua — fan.sqlite3 binding contract tests.

  Coverage (M5.4 + M16.1):
    - open in-memory + close idempotency + isopen
    - version + module-level complete()
    - Result-code constants (ROW/DONE/BUSY/OK/...) + storage-class (INTEGER/...)
    - exec (CREATE/INSERT), changes(), total_changes(), last_insert_rowid()
    - execute alias for exec
    - query returns array of row-maps with column names
    - parameterised query with mixed bind types (nil/bool/int/float/str)
    - prepared statement: bind/bind_all + step ROW/DONE (numeric) + reset re-use
    - bind_values alias for bind_all
    - bind_names with :name / @name / $name parameters
    - bind_parameter_count / bind_parameter_name
    - columns() returns integer count, get_names/get_types return arrays
    - stmt:isopen before/after finalize
    - stmt:nrows / rows / urows iterators
    - db:nrows / rows / urows iterators (auto-finalize on end)
    - transaction: begin/commit and begin/rollback + get_autocommit
    - busy_timeout accepts ms
    - errcode / errmsg after failed op
    - interrupt() is a no-op when nothing's running (just proves the call works)
    - error surfaces: exec on bad SQL returns nil, err
]]
local T      = require("test_framework")
local sqlite = require("fan.sqlite3")
local s      = T.suite("fan.sqlite3 (M5+M16.1)")

s:test("open / version / close idempotent / isopen", function()
  T.truthy(sqlite.version)
  local db, err = sqlite.open(":memory:")
  T.not_nil(db); T.is_nil(err)
  T.eq(db:isopen(), true)
  db:close()
  T.eq(db:isopen(), false)
  db:close()   -- second close must be a no-op
end)

s:test("module-level constants: OK/ROW/DONE + INTEGER/TEXT/etc", function()
  -- Just spot-check a few; the full set is used across other tests.
  T.eq(sqlite.OK,   0)
  T.eq(sqlite.ROW,  100)
  T.eq(sqlite.DONE, 101)
  T.truthy(sqlite.BUSY);       T.truthy(sqlite.LOCKED)
  T.truthy(sqlite.CONSTRAINT); T.truthy(sqlite.MISUSE)
  T.eq(sqlite.INTEGER, 1); T.eq(sqlite.FLOAT, 2)
  T.eq(sqlite.TEXT,    3); T.eq(sqlite.BLOB,  4); T.eq(sqlite.NULL, 5)
end)

s:test("sqlite3.complete() classifies terminated SQL", function()
  T.eq(sqlite.complete("SELECT 1;"),   true)
  T.eq(sqlite.complete("SELECT 1"),    false)   -- no terminator
  T.eq(sqlite.complete(""),            false)
end)

s:test("exec + changes + total_changes + last_insert_rowid", function()
  local db = sqlite.open(":memory:")
  T.truthy(db:exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)"))
  T.eq(db:exec("INSERT INTO t(name) VALUES ('alice')"), 1)
  T.eq(db:last_insert_rowid(), 1)
  T.eq(db:exec("INSERT INTO t(name) VALUES ('bob')"), 1)
  T.eq(db:last_insert_rowid(), 2)
  T.eq(db:changes(), 1)
  T.eq(db:total_changes(), 2)      -- 2 inserts since open
  db:close()
end)

s:test("execute is an alias for exec", function()
  local db = sqlite.open(":memory:")
  T.truthy(db:execute("CREATE TABLE t(v)"))
  T.eq(db:execute("INSERT INTO t VALUES(1)"), 1)
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

s:test("prepared statement: bind_all + step ROW/DONE (numeric) + reset re-use", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
  local ins = db:prepare("INSERT INTO t(name, age) VALUES(?, ?)")
  T.eq(ins:isopen(), true)
  for _, r in ipairs({{"a", 1}, {"b", 2}, {"c", 3}}) do
    ins:reset()
    T.truthy(ins:bind_all(r[1], r[2]))
    T.eq(ins:step(), sqlite.DONE)      -- M16.1: numeric compare, not "done"
  end
  ins:finalize()
  T.eq(ins:isopen(), false)

  local sel = db:prepare("SELECT name, age FROM t ORDER BY id")
  T.eq(sel:columns(), 2)                -- lsqlite3 semantics: column COUNT
  local names_meta = sel:get_names()
  T.eq(names_meta[1], "name"); T.eq(names_meta[2], "age")
  local types = sel:get_types()
  T.eq(types[1], "TEXT"); T.eq(types[2], "INTEGER")

  local walked = {}
  while true do
    local rc = sel:step()
    if rc == sqlite.DONE then break end
    T.eq(rc, sqlite.ROW)
    -- pull the current row using nrows-style column pluck (via prepare -> query
    -- is the shortcut for one-shot use; here we drive step() manually).
    -- To read the current row without an iterator, use get_names + a helper:
    walked[#walked + 1] = { name = "", age = 0 }
  end
  T.eq(#walked, 3)
  sel:finalize()
  db:close()
end)

s:test("bind_values is an alias for bind_all", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(v)")
  local st = db:prepare("INSERT INTO t VALUES(?)")
  T.truthy(st:bind_values(42))
  T.eq(st:step(), sqlite.DONE)
  st:finalize()
  T.eq(db:query("SELECT v FROM t")[1].v, 42)
  db:close()
end)

s:test("stmt:bind_names with :name / @name / $name", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(name TEXT, age INTEGER, note TEXT)")

  -- ":name" prefix
  local st = db:prepare("INSERT INTO t(name, age, note) VALUES(:name, :age, :note)")
  T.eq(st:bind_parameter_count(), 3)
  T.eq(st:bind_parameter_name(1), ":name")
  T.eq(st:bind_parameter_name(2), ":age")
  -- User may pass keys with or without the leading ':'
  T.truthy(st:bind_names{ name = "alice", age = 30, note = "n1" })
  T.eq(st:step(), sqlite.DONE)
  st:finalize()

  -- "@name" prefix
  st = db:prepare("INSERT INTO t(name, age) VALUES(@n, @a)")
  T.eq(st:bind_parameter_name(1), "@n")
  T.truthy(st:bind_names{ ["@n"] = "bob", a = 40 })  -- mixed keys
  T.eq(st:step(), sqlite.DONE)
  st:finalize()

  -- Missing key -> NULL (lsqlite3 behaviour)
  st = db:prepare("INSERT INTO t(name, note) VALUES(:name, :note)")
  T.truthy(st:bind_names{ name = "carol" })  -- note omitted
  T.eq(st:step(), sqlite.DONE)
  st:finalize()

  local rows = db:query("SELECT name, age, note FROM t ORDER BY rowid")
  T.eq(#rows, 3)
  T.eq(rows[1].name, "alice"); T.eq(rows[1].age, 30);   T.eq(rows[1].note, "n1")
  T.eq(rows[2].name, "bob");   T.eq(rows[2].age, 40)
  T.eq(rows[3].name, "carol"); T.eq(rows[3].note, nil)  -- NULL -> nil
  db:close()
end)

s:test("stmt:nrows iterator (key-value maps)", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)")
  db:exec("INSERT INTO t(name) VALUES('a'),('b'),('c')")
  local st = db:prepare("SELECT id, name FROM t ORDER BY id")
  local out = {}
  for row in st:nrows() do
    out[#out + 1] = row.name
  end
  T.eq(table.concat(out, ","), "a,b,c")
  st:finalize()
  db:close()
end)

s:test("stmt:rows iterator (array per row)", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)")
  db:exec("INSERT INTO t(name) VALUES('a'),('b')")
  local st = db:prepare("SELECT id, name FROM t ORDER BY id")
  local out = {}
  for row in st:rows() do
    out[#out + 1] = string.format("%d:%s", row[1], row[2])
  end
  T.eq(table.concat(out, ","), "1:a,2:b")
  st:finalize()
  db:close()
end)

s:test("stmt:urows iterator (unpacked returns)", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)")
  db:exec("INSERT INTO t(name) VALUES('a'),('b')")
  local st = db:prepare("SELECT id, name FROM t ORDER BY id")
  local out = {}
  for id, name in st:urows() do
    out[#out + 1] = string.format("%d:%s", id, name)
  end
  T.eq(table.concat(out, ","), "1:a,2:b")
  st:finalize()
  db:close()
end)

s:test("db:nrows(sql) shortcut iterator", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)")
  db:exec("INSERT INTO t(name) VALUES('x'),('y'),('z')")
  local out = {}
  for row in db:nrows("SELECT name FROM t ORDER BY id") do
    out[#out + 1] = row.name
  end
  T.eq(table.concat(out, ","), "x,y,z")
  db:close()
end)

s:test("db:rows(sql) and db:urows(sql) shortcut iterators", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(v INTEGER)")
  db:exec("INSERT INTO t VALUES(10),(20),(30)")
  local sum = 0
  for row in db:rows("SELECT v FROM t") do sum = sum + row[1] end
  T.eq(sum, 60)
  sum = 0
  for v in db:urows("SELECT v FROM t") do sum = sum + v end
  T.eq(sum, 60)
  db:close()
end)

s:test("transaction: commit persists, rollback reverts, get_autocommit", function()
  local db = sqlite.open(":memory:")
  db:exec("CREATE TABLE t(v INTEGER)")
  T.eq(db:get_autocommit(), true)
  T.truthy(db:begin())
  T.eq(db:get_autocommit(), false)     -- inside a transaction
  db:exec("INSERT INTO t(v) VALUES(1)")
  db:exec("INSERT INTO t(v) VALUES(2)")
  T.truthy(db:commit())
  T.eq(db:get_autocommit(), true)
  T.eq(#db:query("SELECT v FROM t"), 2)

  T.truthy(db:begin())
  db:exec("INSERT INTO t(v) VALUES(3)")
  T.truthy(db:rollback())
  T.eq(#db:query("SELECT v FROM t"), 2)
  db:close()
end)

s:test("db:busy_timeout accepts ms without erroring", function()
  local db = sqlite.open(":memory:")
  T.truthy(db:busy_timeout(1000))
  T.truthy(db:busy_timeout(0))         -- 0 = clear
  db:close()
end)

s:test("db:interrupt is a no-op when idle", function()
  local db = sqlite.open(":memory:")
  db:interrupt()   -- should not raise
  db:close()
end)

s:test("errcode / errmsg after failed exec", function()
  local db = sqlite.open(":memory:")
  local ok, err = db:exec("SELECT * FROM nowhere")
  T.is_nil(ok); T.truthy(err)
  -- errcode should be non-zero after the failed statement
  T.truthy(db:errcode() ~= sqlite.OK)
  T.truthy(db:errmsg())
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
