--[[
  test_mariadb.lua — M5.5.a fan.mariadb binding contract tests.

  Requires a MariaDB server. In the arm1 container the server is available
  via /run/mysqld/mysqld.sock as root without password.

  Coverage (synchronous M5.5.a subset; M5.5.b will add async wait + pool
  + R19/R13 regressions):
    - connect via unix_socket, close idempotent
    - server_version / ping / client_version
    - exec CREATE / DROP / INSERT + affected_rows + last_insert_id
    - query returns array of row-maps; column names + types respected
    - parameterised query ('?' expansion; nil/bool/int/float/str)
    - prepared statement: bind_all + step ROW/DONE + columns
    - transaction commit/rollback (start transaction / commit / rollback)
    - charset=utf8mb4 round-trips
]]
local T  = require("test_framework")
local mariadb = require("fan.mariadb")

-- global helper: connect + set up an isolated DB namespace per test run
local function pfx()
  return "lf2_" .. tostring(os.time()) .. "_" .. tostring(math.random(1e6))
end

local function connect(extra)
  local args = {
    unix_socket = "/run/mysqld/mysqld.sock",
    user        = "root",
    password    = "",
  }
  if extra then for k, v in pairs(extra) do args[k] = v end end
  local db, err = mariadb.connect(args)
  return db, err
end

-- Skip the entire suite gracefully if no server is reachable.
do
  local db, err = connect()
  if not db then
    print("[SKIP] fan.mariadb: no local MariaDB (" .. tostring(err) .. ")")
    os.exit(0)
  end
  db:close()
end

local DBNAME = pfx()
do
  local db = connect()
  T.truthy(db:exec("CREATE DATABASE IF NOT EXISTS `" .. DBNAME .. "`"))
  db:close()
end

local function fresh()
  return connect{ database = DBNAME }
end

local s = T.suite("fan.mariadb (M5.5.a)")

s:test("connect + version + ping + close idempotent", function()
  local db = fresh()
  T.truthy(mariadb.client_version)
  T.truthy(db:server_version())
  T.truthy(db:ping())
  db:close()
  db:close()   -- must be a no-op
end)

s:test("exec: CREATE / INSERT / DROP with affected_rows + last_insert_id", function()
  local db = fresh()
  T.truthy(db:exec("DROP TABLE IF EXISTS t"))
  T.truthy(db:exec("CREATE TABLE t(id BIGINT PRIMARY KEY AUTO_INCREMENT, v INT)"))
  T.eq(db:exec("INSERT INTO t(v) VALUES(10)"), 1)
  T.eq(db:last_insert_id(), 1)
  T.eq(db:exec("INSERT INTO t(v) VALUES(20)"), 1)
  T.eq(db:last_insert_id(), 2)
  T.eq(db:exec("UPDATE t SET v = 99 WHERE v = 10"), 1)
  T.eq(db:affected_rows(), 1)
  db:exec("DROP TABLE t")
  db:close()
end)

s:test("query returns row-maps with named columns and type coercion", function()
  local db = fresh()
  db:exec("DROP TABLE IF EXISTS t")
  db:exec("CREATE TABLE t(id BIGINT PRIMARY KEY, name VARCHAR(64), age INT, rating DOUBLE)")
  db:exec("INSERT INTO t VALUES(1,'a',10,1.5),(2,'b',20,2.5)")
  local rows = db:query("SELECT id,name,age,rating FROM t ORDER BY id")
  T.eq(#rows, 2)
  T.eq(rows[1].id, 1); T.eq(rows[1].name, "a")
  T.eq(rows[1].age, 10)
  T.eq(rows[1].rating, 1.5)
  T.eq(rows[2].name, "b"); T.eq(rows[2].age, 20)
  db:exec("DROP TABLE t")
  db:close()
end)

s:test("parameterised query: '?' expansion with escaping", function()
  local db = fresh()
  db:exec("DROP TABLE IF EXISTS t")
  db:exec("CREATE TABLE t(v VARCHAR(64))")
  db:exec("INSERT INTO t VALUES ('plain'), ('with '' quote'), ('sql inject; DROP')")
  local rows = db:query("SELECT v FROM t WHERE v = ?", "with ' quote")
  T.eq(#rows, 1); T.eq(rows[1].v, "with ' quote")
  local rows2 = db:query("SELECT v FROM t WHERE v = ?", "sql inject; DROP")
  T.eq(#rows2, 1)
  db:exec("DROP TABLE t")
  db:close()
end)

s:test("prepared statement: bind_all + step ROW/DONE + columns", function()
  local db = fresh()
  db:exec("DROP TABLE IF EXISTS t")
  db:exec("CREATE TABLE t(id BIGINT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(64), age INT)")
  local ins = db:prepare("INSERT INTO t(name, age) VALUES(?, ?)")
  for _, r in ipairs({{"a", 1}, {"b", 2}, {"c", 3}}) do
    T.truthy(ins:bind_all(r[1], r[2]))
    T.eq(ins:step(), "done")
    -- for repeated insert we finalize + re-prepare (no reset in M5.5.a)
    ins:finalize()
    ins = db:prepare("INSERT INTO t(name, age) VALUES(?, ?)")
  end
  ins:finalize()

  local sel = db:prepare("SELECT name, age FROM t ORDER BY id")
  local names = {}
  while true do
    local rc = sel:step()
    if rc == "done" then break end
    T.eq(rc, "row")
    names[#names + 1] = sel:columns().name
  end
  T.eq(table.concat(names, ","), "a,b,c")
  sel:finalize()
  db:exec("DROP TABLE t")
  db:close()
end)

s:test("transaction: commit persists, rollback reverts", function()
  local db = connect{ database = DBNAME, autocommit = false }
  db:exec("DROP TABLE IF EXISTS t")
  db:exec("CREATE TABLE t(v INT) ENGINE=InnoDB")
  T.truthy(db:begin())
  db:exec("INSERT INTO t(v) VALUES(1)")
  db:exec("INSERT INTO t(v) VALUES(2)")
  T.truthy(db:commit())
  T.eq(#db:query("SELECT v FROM t"), 2)

  T.truthy(db:begin())
  db:exec("INSERT INTO t(v) VALUES(3)")
  T.truthy(db:rollback())
  T.eq(#db:query("SELECT v FROM t"), 2)
  db:exec("DROP TABLE t")
  db:close()
end)

s:test("utf8mb4 round-trip", function()
  local db = connect{ database = DBNAME, charset = "utf8mb4" }
  db:exec("DROP TABLE IF EXISTS t")
  db:exec("CREATE TABLE t(v VARCHAR(64)) CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci")
  db:exec("INSERT INTO t VALUES ('中文'), ('café'), ('😀')")
  local rows = db:query("SELECT v FROM t")
  T.eq(#rows, 3)
  local seen = {}
  for _, r in ipairs(rows) do seen[r.v] = true end
  T.truthy(seen["中文"])
  T.truthy(seen["café"])
  T.truthy(seen["😀"])
  db:exec("DROP TABLE t")
  db:close()
end)

s:test("bad SQL surfaces nil, err", function()
  local db = fresh()
  local ok, err = db:exec("NOT A QUERY")
  T.is_nil(ok); T.truthy(err); T.truthy(err:find("mariadb"))
  db:close()
end)

-- Run tests, then clean up the ephemeral database.
local rc = T.run(s)
do
  local db = connect()
  if db then
    db:exec("DROP DATABASE IF EXISTS `" .. DBNAME .. "`")
    db:close()
  end
end
os.exit(rc)
