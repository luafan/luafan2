--[[
  test_orm_mariadb.lua — M5.5.a fan.orm base + MariaDB driver contract.

  Same shape as test_orm.lua (SQLite driver) but driven against a live
  MariaDB server on the arm1 container. Skips gracefully if no server.
]]
local T   = require("test_framework")
local mariadb = require("fan.mariadb")
local orm = require("fan.orm")

local function connect(extra)
  local args = {
    unix_socket = "/run/mysqld/mysqld.sock",
    user = "root", password = "",
  }
  if extra then for k, v in pairs(extra) do args[k] = v end end
  return mariadb.connect(args)
end

do
  local db, err = connect()
  if not db then
    print("[SKIP] fan.orm/mariadb: no local MariaDB (" .. tostring(err) .. ")")
    os.exit(0)
  end
  db:close()
end

local DBNAME = "lf2_orm_" .. tostring(os.time()) .. "_" .. tostring(math.random(1e6))
do
  local db = connect()
  db:exec("CREATE DATABASE IF NOT EXISTS `" .. DBNAME .. "`")
  db:close()
end

local function fresh_ctx()
  local db  = connect{ database = DBNAME, charset = "utf8mb4" }
  local drv = orm.mariadb_driver(db)
  local ctx = orm.new_context(drv)
  return ctx, db
end

local s = T.suite("fan.orm base + mariadb (M5.5.a)")

local function drop_all(db, tables)
  for _, t in ipairs(tables) do db:exec("DROP TABLE IF EXISTS `" .. t .. "`") end
end

s:test("define + insert + find_by + list on mariadb", function()
  local ctx, db = fresh_ctx()
  drop_all(db, {"users"})
  local User = ctx:define("users", {
    id    = "BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT",
    name  = "VARCHAR(64) NOT NULL",
    email = "VARCHAR(128)",
  })
  local id1 = User.insert{name = "alice", email = "a@x"}
  local id2 = User.insert{name = "bob"}
  T.eq(id1, 1); T.eq(id2, 2)

  local u = User.find_by{name = "alice"}
  T.eq(u.email, "a@x"); T.eq(u.id, 1)

  local all = User.list{order = "id"}
  T.eq(#all, 2)
  T.eq(all[1].name, "alice"); T.eq(all[2].name, "bob")
  drop_all(db, {"users"})
  db:close()
end)

s:test("update / delete / where filter (mariadb)", function()
  local ctx, db = fresh_ctx()
  drop_all(db, {"t2","logs"})
  local T2 = ctx:define("t2", {
    id = "BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT", v = "INT",
  })
  T2.insert{v = 10}; T2.insert{v = 20}
  T.eq(T2.update(1, {v = 99}), 1)
  T.eq(T2.find_by{id = 1}.v, 99)
  T.eq(T2.delete(2), 1)
  T.eq(#T2.list{}, 1)

  local Log = ctx:define("logs", {
    id = "BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT",
    level = "VARCHAR(16)", msg = "TEXT",
  })
  Log.insert{level="info",  msg="a"}
  Log.insert{level="warn",  msg="b"}
  Log.insert{level="info",  msg="c"}
  local infos = Log.list{ where = {level = "info"}, order = "id" }
  T.eq(#infos, 2)
  drop_all(db, {"t2","logs"})
  db:close()
end)

s:test("transaction: commit + rollback (InnoDB required)", function()
  local ctx, db = fresh_ctx()
  drop_all(db, {"t3"})
  db:exec("CREATE TABLE t3(id BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT, v INT) ENGINE=InnoDB")
  local T3 = ctx:define("t3", {
    id = "BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT", v = "INT",
  })
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
  T.eq(#T3.list{}, 2)
  drop_all(db, {"t3"})
  db:close()
end)

-- Run tests, then clean up.
local rc = T.run(s)
do
  local db = connect()
  if db then
    db:exec("DROP DATABASE IF EXISTS `" .. DBNAME .. "`")
    db:close()
  end
end
os.exit(rc)
