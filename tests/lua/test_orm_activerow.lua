--[[
  test_orm_activerow.lua — M16.2 fan.orm active-row contract tests.

  Coverage:
    - insert returns a live row object (metatable attached, pk populated)
    - find_by returns a live row (auto-diff via :update() works)
    - list returns array of live rows; list{raw=true} escape hatch
    - row:update() auto-diff: only UPDATEs changed columns; returns rows_affected
    - row:update() with no changes returns 0
    - row:update{k=v} explicit override: UPDATEs given columns, syncs snapshot
    - row:update() after :update(): baseline refreshes so successive diffs work
    - row:delete() DELETEs and detaches row (setmetatable nil)
    - row:remove() alias of :delete()
    - raw_query returns plain maps (no metatable)
    - Model.update(id, ...) / Model.delete(id) legacy API still works
    - pk aliasing: schema.pk = "uuid" makes row:update()/:delete() use `uuid`

  All tests use SQLite (in-memory) since it's the reference driver.
]]
local T      = require("test_framework")
local sqlite = require("fan.sqlite3")
local orm    = require("fan.orm")
local s      = T.suite("fan.orm active-row (M16.2)")

local function fresh_ctx()
  local db  = sqlite.open(":memory:")
  local drv = orm.sqlite_driver(db)
  local ctx = orm.new_context(drv)
  return ctx, db
end

s:test("insert returns a live row with the pk populated", function()
  local ctx = fresh_ctx()
  local User = ctx:define("users", {
    id    = "INTEGER PRIMARY KEY AUTOINCREMENT",
    name  = "TEXT NOT NULL",
    email = "TEXT",
  })
  local u = User.insert{name = "alice", email = "a@x"}
  T.eq(type(u), "table")
  T.eq(u.id, 1); T.eq(u.name, "alice"); T.eq(u.email, "a@x")
  T.eq(type(u.update), "function")   -- metatable methods reachable
  T.eq(type(u.delete), "function")
  T.eq(type(u.remove), "function")
end)

s:test("row:update() auto-diff: only changed columns are UPDATEd", function()
  local ctx = fresh_ctx()
  local User = ctx:define("users", {
    id    = "INTEGER PRIMARY KEY AUTOINCREMENT",
    name  = "TEXT",
    email = "TEXT",
    age   = "INTEGER",
  })
  local u = User.insert{name = "alice", email = "a@x", age = 30}
  u.email = "alice@new.example"        -- change one column
  T.eq(u:update(), 1)                  -- rows_affected = 1
  -- Verify persisted:
  local reloaded = User.find_by{id = u.id}
  T.eq(reloaded.email, "alice@new.example")
  T.eq(reloaded.name, "alice"); T.eq(reloaded.age, 30)  -- unchanged
end)

s:test("row:update() with no changes returns 0", function()
  local ctx = fresh_ctx()
  local T2 = ctx:define("t2", { id = "INTEGER PRIMARY KEY AUTOINCREMENT", v = "INTEGER" })
  local r = T2.insert{v = 10}
  T.eq(r:update(), 0)      -- nothing to write
end)

s:test("row:update{k=v} override mode: explicit column set", function()
  local ctx = fresh_ctx()
  local User = ctx:define("users", {
    id   = "INTEGER PRIMARY KEY AUTOINCREMENT",
    name = "TEXT",
    age  = "INTEGER",
  })
  local u = User.insert{name = "alice", age = 30}
  T.eq(u:update{age = 31}, 1)
  T.eq(u.age, 31)                       -- override syncs into row
  T.eq(User.find_by{id = u.id}.age, 31) -- and persists
end)

s:test("row:update() twice: baseline refreshes between calls", function()
  local ctx = fresh_ctx()
  local T2 = ctx:define("t2", { id = "INTEGER PRIMARY KEY AUTOINCREMENT", v = "INTEGER" })
  local r = T2.insert{v = 1}
  r.v = 2; T.eq(r:update(), 1)   -- first diff: {v: 1 -> 2}
  T.eq(r:update(), 0)            -- second call: baseline is now 2, no diff
  r.v = 3; T.eq(r:update(), 1)   -- third: {v: 2 -> 3}
  T.eq(T2.find_by{id = r.id}.v, 3)
end)

s:test("row:delete() removes row + detaches metatable", function()
  local ctx = fresh_ctx()
  local T2 = ctx:define("t2", { id = "INTEGER PRIMARY KEY AUTOINCREMENT", v = "INTEGER" })
  local r = T2.insert{v = 42}
  T.eq(r:delete(), 1)
  T.is_nil(T2.find_by{id = r.id})
  T.is_nil(getmetatable(r))                       -- detached
  -- Direct field access still works (r is a plain table now)
  T.eq(r.v, 42)
end)

s:test("row:remove() is an alias for :delete()", function()
  local ctx = fresh_ctx()
  local T2 = ctx:define("t2", { id = "INTEGER PRIMARY KEY AUTOINCREMENT", v = "INTEGER" })
  local r = T2.insert{v = 99}
  T.eq(r:remove(), 1)
  T.is_nil(T2.find_by{id = r.id})
end)

s:test("find_by returns a live row (auto-diff works after find)", function()
  local ctx = fresh_ctx()
  local T2 = ctx:define("t2", {
    id    = "INTEGER PRIMARY KEY AUTOINCREMENT",
    label = "TEXT",
  })
  T2.insert{label = "old"}
  local r = T2.find_by{id = 1}
  T.truthy(r)
  r.label = "new"
  T.eq(r:update(), 1)
  T.eq(T2.find_by{id = 1}.label, "new")
end)

s:test("list returns array of live rows", function()
  local ctx = fresh_ctx()
  local T3 = ctx:define("t3", {
    id = "INTEGER PRIMARY KEY AUTOINCREMENT",
    v  = "INTEGER",
  })
  T3.insert{v = 1}; T3.insert{v = 2}; T3.insert{v = 3}
  local all = T3.list{order = "id"}
  T.eq(#all, 3)
  -- Each element is a live row: mutate + update the middle one
  all[2].v = 99
  T.eq(all[2]:update(), 1)
  T.eq(T3.find_by{id = 2}.v, 99)
end)

s:test("list{raw=true} escape hatch: plain maps, no metatable", function()
  local ctx = fresh_ctx()
  local T3 = ctx:define("t3", {
    id = "INTEGER PRIMARY KEY AUTOINCREMENT",
    v  = "INTEGER",
  })
  T3.insert{v = 1}; T3.insert{v = 2}
  local raw = T3.list{raw = true, order = "id"}
  T.eq(#raw, 2)
  T.is_nil(getmetatable(raw[1]))
  T.is_nil(getmetatable(raw[2]))
  -- .update / .delete are not present:
  T.is_nil(raw[1].update)
  T.is_nil(raw[1].delete)
end)

s:test("raw_query returns plain maps, no metatable", function()
  local ctx = fresh_ctx()
  local T2 = ctx:define("t2", {
    id = "INTEGER PRIMARY KEY AUTOINCREMENT",
    v  = "INTEGER",
  })
  T2.insert{v = 1}; T2.insert{v = 2}
  local rows = T2.raw_query("SELECT id, v FROM t2 ORDER BY id")
  T.eq(#rows, 2)
  T.is_nil(getmetatable(rows[1]))
  T.is_nil(rows[1].update)
end)

s:test("Model.update(id, ...) / Model.delete(id) legacy API unchanged", function()
  local ctx = fresh_ctx()
  local T2 = ctx:define("t2", { id = "INTEGER PRIMARY KEY AUTOINCREMENT", v = "INTEGER" })
  T2.insert{v = 10}; T2.insert{v = 20}
  T.eq(T2.update(1, {v = 99}), 1)
  T.eq(T2.find_by{id = 1}.v, 99)
  T.eq(T2.delete(2), 1)
  T.eq(#T2.list{}, 1)
end)

s:test("pk aliasing: schema.pk='uuid' makes row methods use that column", function()
  local ctx = fresh_ctx()
  local Doc = ctx:define("docs", {
    pk    = "uuid",   -- tell orm the pk column name is 'uuid', not 'id'
    uuid  = "TEXT PRIMARY KEY",
    title = "TEXT",
    body  = "TEXT",
  })
  local d = Doc.insert{uuid = "u-1", title = "hello", body = "world"}
  T.eq(d.uuid, "u-1")

  d.body = "world!!"
  T.eq(d:update(), 1)
  T.eq(Doc.find_by{uuid = "u-1"}.body, "world!!")

  T.eq(d:delete(), 1)
  T.is_nil(Doc.find_by{uuid = "u-1"})
end)

os.exit(T.run(s))
