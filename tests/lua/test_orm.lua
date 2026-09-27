--[[
  test_orm.lua — M5.4 fan.orm base contract tests (SQLite driver).

  Coverage:
    - context/define: CREATE TABLE IF NOT EXISTS with schema
    - Model.insert / Model.find_by / Model.list with where/limit/order
    - Model.update / Model.delete return rows_affected
    - schema migration: ADD COLUMN when define adds a new field
    - transaction wrapper: commit path + rollback on error
]]
local T      = require("test_framework")
local sqlite = require("fan.sqlite3")
local orm    = require("fan.orm")
local s      = T.suite("fan.orm base + sqlite (M5)")

local function fresh_ctx()
  local db  = sqlite.open(":memory:")
  local drv = orm.sqlite_driver(db)
  local ctx = orm.new_context(drv)
  return ctx, db
end

s:test("define + insert + find_by + list", function()
  local ctx = fresh_ctx()
  local User = ctx:define("users", {
    id    = "INTEGER PRIMARY KEY AUTOINCREMENT",
    name  = "TEXT NOT NULL",
    email = "TEXT",
  })
  local id1 = User.insert{name = "alice", email = "a@x"}
  local id2 = User.insert{name = "bob"}
  T.eq(id1, 1); T.eq(id2, 2)

  local u = User.find_by{name = "alice"}
  T.eq(u.email, "a@x"); T.eq(u.id, 1)

  local all = User.list{order = "id"}
  T.eq(#all, 2)
  T.eq(all[1].name, "alice"); T.eq(all[2].name, "bob")
end)

s:test("update / delete return rows_affected", function()
  local ctx = fresh_ctx()
  local T2 = ctx:define("t2", { id = "INTEGER PRIMARY KEY AUTOINCREMENT", v = "INTEGER" })
  T2.insert{v = 10}; T2.insert{v = 20}
  T.eq(T2.update(1, {v = 99}), 1)
  T.eq(T2.find_by{id = 1}.v, 99)
  T.eq(T2.delete(2), 1)
  T.eq(#T2.list{}, 1)
end)

s:test("list with where filter + limit", function()
  local ctx = fresh_ctx()
  local Log = ctx:define("logs", {
    id = "INTEGER PRIMARY KEY AUTOINCREMENT",
    level = "TEXT", msg = "TEXT",
  })
  Log.insert{level="info",  msg="a"}
  Log.insert{level="warn",  msg="b"}
  Log.insert{level="info",  msg="c"}
  Log.insert{level="error", msg="d"}
  local infos = Log.list{ where = {level = "info"}, order = "id" }
  T.eq(#infos, 2)
  local first = Log.list{ order = "id", limit = 1 }
  T.eq(#first, 1); T.eq(first[1].id, 1)
end)

s:test("schema migration: define adds column via ALTER TABLE ADD COLUMN", function()
  local ctx, db = fresh_ctx()
  local U1 = ctx:define("u", {
    id = "INTEGER PRIMARY KEY AUTOINCREMENT",
    name = "TEXT",
  })
  U1.insert{name = "x"}
  -- re-define with an extra column
  local U2 = ctx:define("u", {
    id = "INTEGER PRIMARY KEY AUTOINCREMENT",
    name = "TEXT",
    age  = "INTEGER",
  })
  -- verify column presence via PRAGMA
  local info = db:query("PRAGMA table_info(u)")
  local names = {}
  for _, r in ipairs(info) do names[r.name] = true end
  T.truthy(names.name); T.truthy(names.age); T.truthy(names.id)
  -- old row still readable, age is nil
  local u = U2.find_by{id = 1}
  T.eq(u.name, "x")
end)

s:test("transaction: pcall on fn commits, error rolls back", function()
  local ctx = fresh_ctx()
  local T3 = ctx:define("t3", { id = "INTEGER PRIMARY KEY AUTOINCREMENT", v = "INTEGER" })
  local ok = ctx:transaction(function()
    T3.insert{v = 1}
    T3.insert{v = 2}
  end)
  T.truthy(ok)
  T.eq(#T3.list{}, 2)

  local ok2, err2 = ctx:transaction(function()
    T3.insert{v = 3}
    error("boom")
  end)
  T.is_nil(ok2); T.truthy(err2:find("boom"))
  T.eq(#T3.list{}, 2)   -- unchanged
end)

os.exit(T.run(s))
