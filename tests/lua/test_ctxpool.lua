-- test_ctxpool.lua — M25 compatibility facade tests.
--
-- These tests deliberately do not require a running MariaDB.  Loading the
-- facade must be lazy with respect to connections: schema scanning and pool
-- construction are safe, while the first :pop() is where connection errors
-- are returned.  Full ORM CRUD remains covered by the MariaDB integration
-- suites when a server is available.

local T = require("test_framework")
local s = T.suite("ctxpool compatibility (M25)")

-- The release image flattens the historical source directory into /root;
-- the test runner keeps it under /work/webase. Keep one facade instance for
-- the suite so a close in one case cannot make a later require() return the
-- same closed singleton.
package.path = "/work/webase/?.lua;/work/webase/?/init.lua;" .. package.path
local pool = require("ctxpool")

s:test("loads without connecting and exposes legacy facade", function()
  T.is_type(pool, "table")
  T.is_type(pool.pop, "function")
  T.is_type(pool.push, "function")
  T.is_type(pool.safe, "function")
  T.is_type(pool.close, "function")
  T.is_type(pool.stats, "function")
  T.is_type(pool.schemas, "table")
  local stats = pool:stats()
  T.eq(stats.max, 10)
  T.eq(stats.live, 0)
  T.eq(stats.idle, 0)
end)

s:test("safe rejects a non-function without touching MariaDB", function()
  local value, err = pool:safe("not a function")
  T.is_nil(value)
  T.eq(err, "ctxpool: safe expects function")
end)

s:test("pop returns a connection error rather than raising when DB is absent", function()
  local ctx, err = pool:pop()
  T.is_nil(ctx)
  T.not_nil(err)
  T.truthy(tostring(err):find("mariadb", 1, true)
        or tostring(err):find("connect", 1, true), tostring(err))
end)

s:test("real MariaDB ctxpool supports concurrent safe CRUD", function()
  local fan = require("fan")
  local mariadb = require("fan.mariadb")
  local root_opts = {
    unix_socket = "/run/mysqld/mysqld.sock",
    user = "root",
    password = "",
  }
  local probe, probe_err = mariadb.connect(root_opts)
  if not probe then
    print("[SKIP] ctxpool concurrency: no local MariaDB (" .. tostring(probe_err) .. ")")
    return
  end

  local dbname = "lf2_ctxpool_test"
  probe:exec("DROP DATABASE IF EXISTS `" .. dbname .. "`")
  probe:exec("CREATE DATABASE `" .. dbname .. "`")
  probe:close()
  local setup = assert(mariadb.connect{
    unix_socket = root_opts.unix_socket,
    user = root_opts.user,
    password = root_opts.password,
    database = dbname,
  })
  setup:exec("CREATE TABLE items (id INT AUTO_INCREMENT PRIMARY KEY, worker INT NOT NULL, value INT NOT NULL)")
  setup:close()

  local old_workdir = _G.WORKDIR
  _G.WORKDIR = "/work/tests/fixtures/ctxpool/"
  package.loaded["config"] = nil
  package.loaded["fan.config"] = nil
  package.loaded["ctxpool"] = nil
  local live_pool = require("ctxpool")
  T.eq(live_pool.schemas.items.value, "INT NOT NULL")

  local N = 6
  local REQUESTS = 100
  local completed = 0
  local failures = {}
  local done = false
  fan.spawn(function()
    for worker = 1, N do
      fan.spawn(function()
        for request = 1, REQUESTS do
          local value = worker * 1000 + request
          local ok, err = live_pool:safe(function(ctx)
            T.not_nil(ctx.models.items)
            T.eq(ctx.items, ctx.models.items)
            local row = ctx.models.items.insert{worker = worker, value = value}
            T.eq(row.value, value)
            fan.sleep(0.01)
            local found = ctx.items.find_by{worker = worker, value = value}
            T.eq(found.value, value)
            return true
          end)
          if not ok then
            failures[#failures + 1] = "worker=" .. worker .. " request="
              .. request .. ": " .. tostring(err)
            return
          end
        end
        completed = completed + 1
        if completed + #failures == N then
          done = true
          fan.loopbreak()
        end
      end)
    end
  end)
  fan.spawn(function()
    local start = fan.gettime()
    while not done and fan.gettime() - start < 60 do fan.sleep(0.05) end
    if not done then fan.loopbreak() end
  end)
  fan.loop()

  T.eq(#failures, 0, table.concat(failures, " | "))
  T.eq(completed, N)
  local check = assert(mariadb.connect{
    unix_socket = root_opts.unix_socket,
    user = root_opts.user,
    password = root_opts.password,
    database = dbname,
  })
  local rows = assert(check:query("SELECT COUNT(*) AS n FROM items"))
  T.eq(tonumber(rows[1].n), N * REQUESTS)
  check:exec("DROP TABLE items")
  check:close()
  local cleanup = assert(mariadb.connect(root_opts))
  cleanup:exec("DROP DATABASE IF EXISTS `" .. dbname .. "`")
  cleanup:close()
  live_pool:close()
  _G.WORKDIR = old_workdir
  package.loaded["config"] = nil
  package.loaded["ctxpool"] = pool
end)

os.exit(T.run(s))
