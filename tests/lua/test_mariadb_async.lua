--[[
  test_mariadb_async.lua — M5.5.c async wait + R19/R13 regressions.

  Requires MariaDB server. Uses fan.spawn / fan.loop cooperative model:
  each test starts a coroutine that performs async db operations, and
  fan.loop() drives the event loop until they finish.
]]
local T       = require("test_framework")
local fan     = require("fan")
local mariadb = require("fan.mariadb")

local function base_opts(extra)
  local o = {
    unix_socket = "/run/mysqld/mysqld.sock",
    user = "root", password = "",
  }
  if extra then for k, v in pairs(extra) do o[k] = v end end
  return o
end

do
  local db, err = mariadb.connect(base_opts())
  if not db then
    print("[SKIP] fan.mariadb async: no local MariaDB (" .. tostring(err) .. ")")
    os.exit(0)
  end
  db:close()
end

local DBNAME = "lf2_ax_" .. tostring(os.time()) .. "_" .. tostring(math.random(1e6))
do
  local db = mariadb.connect(base_opts())
  db:exec("CREATE DATABASE IF NOT EXISTS `" .. DBNAME .. "`")
  db:close()
end

local function fresh() return mariadb.connect(base_opts{database = DBNAME}) end

local s = T.suite("fan.mariadb async + R19/R13 (M5.5.c)")

-- Helper: run a fn as a coroutine and drive the event loop until it settles.
-- Captures results via an upvalue table.
local function run_coro(fn)
  local result = {}
  fan.spawn(function()
    local ok, r1, r2 = pcall(fn)
    result.ok = ok; result.r1 = r1; result.r2 = r2
    result.done = true
    fan.loopbreak()
  end)
  fan.loop()
  return result
end

s:test("exec_async: CREATE + INSERT return affected_rows", function()
  local r = run_coro(function()
    local db = fresh()
    db:exec_async("DROP TABLE IF EXISTS t")
    db:exec_async("CREATE TABLE t(id BIGINT PRIMARY KEY AUTO_INCREMENT, v INT)")
    local n1 = db:exec_async("INSERT INTO t(v) VALUES(10)")
    local n2 = db:exec_async("INSERT INTO t(v) VALUES(20)")
    db:exec_async("DROP TABLE t")
    db:close()
    return n1, n2
  end)
  T.truthy(r.ok, "coroutine error: " .. tostring(r.r1))
  T.eq(r.r1, 1); T.eq(r.r2, 1)
end)

s:test("query_async: SELECT returns row-map array", function()
  local r = run_coro(function()
    local db = fresh()
    db:exec_async("DROP TABLE IF EXISTS t")
    db:exec_async("CREATE TABLE t(id INT, name VARCHAR(32))")
    db:exec_async("INSERT INTO t VALUES(1,'a'),(2,'b'),(3,'c')")
    local rows = db:query_async("SELECT id, name FROM t ORDER BY id")
    db:exec_async("DROP TABLE t")
    db:close()
    return rows
  end)
  T.truthy(r.ok, "coroutine error: " .. tostring(r.r1))
  T.eq(#r.r1, 3)
  T.eq(r.r1[1].name, "a"); T.eq(r.r1[3].id, 3)
end)

s:test("query_async: ? placeholder expansion works in async path", function()
  local r = run_coro(function()
    local db = fresh()
    db:exec_async("DROP TABLE IF EXISTS t")
    db:exec_async("CREATE TABLE t(v VARCHAR(64))")
    db:exec_async("INSERT INTO t VALUES('alpha'),('beta'),('gamma')")
    local rows = db:query_async("SELECT v FROM t WHERE v = ?", "beta")
    db:exec_async("DROP TABLE t")
    db:close()
    return rows
  end)
  T.truthy(r.ok, "coroutine error: " .. tostring(r.r1))
  T.eq(#r.r1, 1); T.eq(r.r1[1].v, "beta")
end)

s:test("bad SQL async: returns nil, err (does not crash)", function()
  local r = run_coro(function()
    local db = fresh()
    local ok, err = db:query_async("NOT SQL")
    db:close()
    return ok, err
  end)
  T.truthy(r.ok)
  T.is_nil(r.r1); T.truthy(r.r2); T.truthy(r.r2:find("mariadb"))
end)

s:test("concurrent conns: two coroutines each with own async query", function()
  local out = {}
  fan.spawn(function()
    local db = fresh()
    db:exec_async("DROP TABLE IF EXISTS ct")
    db:exec_async("CREATE TABLE ct(v INT)")
    for i = 1, 20 do db:exec_async("INSERT INTO ct(v) VALUES(" .. i .. ")") end
    db:close()

    -- launch two concurrent coroutines
    local finished = 0
    for tag = 1, 2 do
      fan.spawn(function()
        local d = fresh()
        local rows = d:query_async("SELECT v FROM ct ORDER BY v")
        out[tag] = #rows
        d:close()
        finished = finished + 1
        if finished == 2 then fan.loopbreak() end
      end)
    end
  end)
  fan.loop()
  T.eq(out[1], 20); T.eq(out[2], 20)
  -- cleanup
  local db = fresh(); db:exec("DROP TABLE ct"); db:close()
end)

-- R19: close() while an async op is pending resumes the coroutine with
-- (nil, "mariadb: closed"). We simulate a slow query via SLEEP() and
-- close the connection from another coroutine while it waits.
s:test("R19: db:close() during pending query resumes with (nil,'closed')", function()
  local outcome = {}
  local slow_db = fresh()
  fan.spawn(function()
    -- this coroutine parks inside SLEEP(2)
    local rows, err = slow_db:query_async("SELECT SLEEP(2)")
    outcome.rows = rows
    outcome.err  = err
    fan.loopbreak()
  end)
  fan.spawn(function()
    fan.sleep(0.2)
    slow_db:close()      -- R19: cancels the pending wait
  end)
  fan.loop()
  T.is_nil(outcome.rows)
  T.truthy(outcome.err); T.truthy(outcome.err:find("closed"))
end)

-- R13: db __gc must also cancel any pending wait (defensive path). In
-- practice, an in-flight wait pins the coroutine (via co_ref), which pins
-- the db on the coroutine's stack, so GC won't actually collect it while
-- suspended. We therefore validate the code path by closing the db (which
-- runs cancel_pending_wait synchronously — same routine used by db_gc),
-- then dropping the ref and forcing GC to confirm the finalizer is safe
-- (idempotent) and doesn't crash.
s:test("R13: db_gc after close is idempotent (no crash, no dangling event)", function()
  local outcome = {}
  local slow_db = fresh()
  fan.spawn(function()
    local rows, err = slow_db:query_async("SELECT SLEEP(2)")
    outcome.rows = rows; outcome.err = err
    fan.loopbreak()
  end)
  fan.spawn(function()
    fan.sleep(0.2)
    slow_db:close()      -- runs cancel_pending_wait (same path as db_gc)
    slow_db = nil        -- drop the strong ref
    collectgarbage("collect")
    collectgarbage("collect") -- db_gc runs here; must be a no-op (idempotent)
  end)
  fan.loop()
  T.is_nil(outcome.rows)
  T.truthy(outcome.err); T.truthy(outcome.err:find("closed"))
end)

local rc = T.run(s)
do
  local db = mariadb.connect(base_opts())
  if db then
    db:exec("DROP DATABASE IF EXISTS `" .. DBNAME .. "`")
    db:close()
  end
end
os.exit(rc)
