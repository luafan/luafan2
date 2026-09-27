--[[
  test_integration.lua — M14.C-m: cross-module integration tests.

  Exercises real-production stacks where 3+ modules interoperate at the
  data-flow boundary. Each case is one end-to-end scenario that the
  single-module unit suites cannot cover, focused on the seams between
  modules (encode↔decode, request↔handler↔db, ws frame↔worker RPC).

  Cases:
    1. REST-full-stack: fan.httpd_c server -> fan.orm+fan.sqlite3 model
       -> fan.json body -> fan.http client round-trip (POST / GET / DELETE)
    2. tcp-stream-objectbuf-frame: length-prefixed (fan.stream U32) frames
       carrying fan.objectbuf-encoded Lua tables over fan.tcp
    3. ws-worker-json-dispatch: fan.httpd_c server + fan.websocket upgrade,
       JSON-framed messages dispatched to a fan.worker pool (3 slaves)
    4. mariadb-pool-orm-concurrent: N concurrent coroutines each pool:with
       one conn, run orm INSERT + SELECT; skips if no local MariaDB.

  Every case has a wall-clock watchdog (belt-and-braces on top of the
  run_tests.sh outer timeout) and bounded fan-out (<= 20 coroutines, <=
  200 sockets) so ASan/coverage runs stay within memory limits.

  Run via ./fan (normal / --asan / --coverage all should pass).
]]

local T   = require("test_framework")
local fan = require("fan")

local s = T.suite("integration (M14.C-m)")

-- ---- helpers --------------------------------------------------------------

local function with_loop(body)
  fan.spawn(body)
  fan.loop()
end

-- watchdog: if `pred()` hasn't become true by `deadline_sec`, force
-- loopbreak so the test fails with a clear signal.
local function watchdog(deadline_sec, pred, label)
  fan.spawn(function()
    local start = fan.gettime and fan.gettime() or os.time()
    while true do
      fan.sleep(0.05)
      if pred() then return end
      local now = fan.gettime and fan.gettime() or os.time()
      if now - start > deadline_sec then
        print(string.format("[watchdog] %s exceeded %.1fs — forcing loopbreak",
          label, deadline_sec))
        fan.loopbreak()
        return
      end
    end
  end)
end

-- ==========================================================================
-- integ-1: httpd_c + orm + sqlite3 + json + http — REST full stack
-- ==========================================================================

s:test("integ-1: REST /users CRUD end-to-end (httpd_c + orm + sqlite3 + json + http_c)", function()
  local httpd  = require("fan.httpd")
  local http   = require("fan.http")
  local sqlite = require("fan.sqlite3")
  local orm    = require("fan.orm")
  local json   = require("fan.json")

  local PORT = 24701
  local server
  local trace = {}       -- [event] = value for asserts
  local done = false

  with_loop(function()
    -- App-side DB: SQLite in-memory + ORM context
    local db  = sqlite.open(":memory:")
    local drv = orm.sqlite_driver(db)
    local ctx = orm.new_context(drv)
    local User = ctx:define("users", {
      id    = "INTEGER PRIMARY KEY AUTOINCREMENT",
      name  = "TEXT NOT NULL",
      email = "TEXT",
    })

    server = assert(httpd.bind{
      host = "127.0.0.1", port = PORT,
      onService = function(req, resp)
        local id = tonumber(req.path:match("^/users/(%d+)$"))
        if req.method == "POST" and req.path == "/users" then
          local ok, obj = pcall(json.decode, req.body)
          if not ok or type(obj) ~= "table" or type(obj.name) ~= "string" then
            resp:reply(400, {}, "bad json"); return
          end
          local uid = User.insert{ name = obj.name, email = obj.email }
          resp:reply(201, { ["Content-Type"] = "application/json" },
            json.encode{ id = uid })
        elseif req.method == "GET" and id then
          local u = User.find_by{ id = id }
          if not u then resp:reply(404, {}, "not found"); return end
          resp:reply(200, { ["Content-Type"] = "application/json" },
            json.encode{ id = u.id, name = u.name, email = u.email })
        elseif req.method == "DELETE" and id then
          local n = User.delete(id)
          if n == 0 then resp:reply(404, {}, "not found"); return end
          resp:reply(204, {}, "")
        else
          resp:reply(404, {}, "no route")
        end
      end,
    })

    watchdog(20, function() return done end, "integ-1 REST CRUD")

    fan.spawn(function()
      local base = "http://127.0.0.1:" .. PORT

      -- 1. POST /users {name="alice"} -> 201 {id=1}
      local r1 = http.post(base .. "/users",
        { body = json.encode{ name = "alice", email = "a@x" },
          headers = { ["Content-Type"] = "application/json" } })
      trace.post_status = r1 and r1.status
      trace.post_body   = r1 and r1.body
      local created = r1 and json.decode(r1.body) or {}
      local uid = created.id

      -- 2. POST /users {name="bob"} -> 201 {id=2}
      local r2 = http.post(base .. "/users",
        { body = json.encode{ name = "bob" },
          headers = { ["Content-Type"] = "application/json" } })
      trace.post2_status = r2 and r2.status
      local uid2 = r2 and json.decode(r2.body).id

      -- 3. GET /users/1 -> 200 {id=1, name="alice", email="a@x"}
      local r3 = http.get(base .. "/users/" .. tostring(uid))
      trace.get_status = r3 and r3.status
      trace.get_body   = r3 and json.decode(r3.body)

      -- 4. GET /users/999 -> 404
      local r4 = http.get(base .. "/users/999")
      trace.miss_status = r4 and r4.status

      -- 5. POST /users bad json -> 400
      local r5 = http.post(base .. "/users",
        { body = "not-json", headers = { ["Content-Type"] = "application/json" } })
      trace.badjson_status = r5 and r5.status

      -- 6. DELETE /users/2 -> 204
      local r6 = http.request{ url = base .. "/users/" .. tostring(uid2),
                                method = "DELETE" }
      trace.del_status = r6 and r6.status

      -- 7. GET /users/2 (deleted) -> 404
      local r7 = http.get(base .. "/users/" .. tostring(uid2))
      trace.del_miss_status = r7 and r7.status

      done = true
      fan.loopbreak()
    end)
  end)
  if server then server:close() end

  T.eq(trace.post_status, 201)
  T.eq(trace.post2_status, 201)
  T.eq(trace.get_status, 200)
  T.eq(trace.get_body.name, "alice")
  T.eq(trace.get_body.email, "a@x")
  T.eq(trace.miss_status, 404)
  T.eq(trace.badjson_status, 400)
  T.eq(trace.del_status, 204)
  T.eq(trace.del_miss_status, 404)
end)

-- ==========================================================================
-- integ-2: tcp + stream + objectbuf — length-prefixed binary frame protocol
-- ==========================================================================

s:test("integ-2: length-prefixed objectbuf frames over tcp (100 round-trips)", function()
  local stream    = require("fan.stream")
  local objectbuf = require("fan.objectbuf")

  local PORT = 24702
  local N    = 100
  local server
  local rt_ok = 0
  local server_seen = 0
  local client_done = false

  -- Helper: send one framed message: [U32 length][objectbuf payload]
  local function send_frame(conn, tbl)
    local payload = objectbuf.encode(tbl)
    local hdr = stream.new(); hdr:AddU32(#payload)
    conn:send(hdr:package() .. payload)
  end

  -- Read exactly n bytes from conn (accumulates until enough or EOF).
  local function recv_exact(conn, n)
    local buf = ""
    while #buf < n do
      local chunk, err = conn:receive(n - #buf)
      if not chunk then return nil, err end
      buf = buf .. chunk
    end
    return buf
  end

  local function recv_frame(conn)
    local hdr, err = recv_exact(conn, 4)
    if not hdr then return nil, err end
    local hs = stream.new(hdr); local len = hs:GetU32()
    if not len or len > 1024 * 1024 then return nil, "bad len" end
    local body, err2 = recv_exact(conn, len)
    if not body then return nil, err2 end
    return objectbuf.decode(body)
  end

  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      while true do
        local msg = recv_frame(conn)
        if not msg then break end
        server_seen = server_seen + 1
        -- echo the object back with a server-side stamp added
        msg.echoed = true
        send_frame(conn, msg)
      end
      conn:close()
    end))

    watchdog(20, function() return client_done end, "integ-2 tcp frames")

    fan.spawn(function()
      local c = assert(fan.tcp.connect("127.0.0.1", PORT))
      for i = 1, N do
        local req = { seq = i, name = "obj-" .. i, nested = { a = i, b = { i, i+1, i+2 } } }
        send_frame(c, req)
        local resp = recv_frame(c)
        if resp and resp.seq == i and resp.echoed == true
           and resp.nested.b[3] == i + 2 then
          rt_ok = rt_ok + 1
        end
      end
      c:close()
      client_done = true
      fan.sleep(0.05)   -- let server drain its receive() -> nil path
      fan.loopbreak()
    end)
  end)
  if server then server:close() end

  T.eq(rt_ok, N, "expected all " .. N .. " frames to round-trip, got " .. rt_ok)
  T.eq(server_seen, N, "server saw " .. server_seen .. "/" .. N .. " frames")
end)

-- ==========================================================================
-- integ-3: httpd_c + websocket + json + worker — dispatch WS msgs to workers
-- ==========================================================================

s:test("integ-3: WebSocket JSON messages dispatched to fan.worker pool (3 slaves)", function()
  local httpd     = require("fan.httpd")
  local websocket = require("fan.websocket")
  local worker    = require("fan.worker")
  local json      = require("fan.json")

  local PORT = 24703
  local N    = 10   -- 10 JSON messages over one WS conn
  local server
  local w
  local client_replies = {}
  local done = false

  with_loop(function()
    -- Master starts a worker pool with 3 slaves, exposing a `compute`
    -- function that any slave can execute. This is a plausible v1-style
    -- "web request handled by worker" setup.
    w = worker.new{
      slaves = 3,
      funcs  = {
        compute = function(kind, n)
          if kind == "square" then return n * n end
          if kind == "cube"   then return n * n * n end
          error("unknown kind: " .. tostring(kind))
        end,
      },
    }

    server = assert(httpd.bind{
      host = "127.0.0.1", port = PORT,
      onService = function(req, resp)
        if not req:is_websocket_upgrade() then
          resp:reply(400, {}, "expected ws"); return
        end
        local ws, err = req:websocket_accept()
        if not ws then resp:reply(500, {}, "accept: " .. tostring(err)); return end
        while true do
          local msg, kind = ws:recv()
          if not msg then break end
          -- Parse JSON, dispatch to worker, send back JSON result
          local ok, obj = pcall(json.decode, msg)
          if not ok or type(obj) ~= "table" then
            ws:send(json.encode{ error = "bad json" }); break
          end
          local w_ok, res = w:call("compute", obj.kind, obj.n)
          if w_ok then
            ws:send(json.encode{ seq = obj.seq, result = res })
          else
            ws:send(json.encode{ seq = obj.seq, error = tostring(res) })
          end
        end
        -- Intentionally do NOT ws:close() here: this covers the
        -- "handler forgets to close" path, which used to SIGSEGV in
        -- coverage-mode reruns before the M14.C-m fix in
        -- src/net/httpd.c (evcon detach at websocket_accept).
      end,
    })

    watchdog(30, function() return done end, "integ-3 ws + worker")

    fan.spawn(function()
      -- Raw WS client (fan.tcp + hand-rolled frames), same style as
      -- test_httpd_c.lua's raw_ws_client. Keeps this file self-contained.
      local c = assert(fan.tcp.connect("127.0.0.1", PORT))
      local key = "dGhlIHNhbXBsZSBub25jZQ=="  -- static test key
      c:send(
        "GET /ws HTTP/1.1\r\n" ..
        "Host: 127.0.0.1\r\n" ..
        "Upgrade: websocket\r\n" ..
        "Connection: Upgrade\r\n" ..
        "Sec-WebSocket-Key: " .. key .. "\r\n" ..
        "Sec-WebSocket-Version: 13\r\n\r\n")
      -- Drain handshake response headers up to \r\n\r\n
      local buf = ""
      while not buf:find("\r\n\r\n", 1, true) do
        local chunk = assert(c:receive())
        buf = buf .. chunk
      end
      local rest = buf:sub(buf:find("\r\n\r\n", 1, true) + 4)

      -- Send N text frames (masked) and read N responses.
      local function encode_client_frame(payload)
        local hdr = string.char(0x81)          -- FIN + TEXT
        local n = #payload
        local mask = "\0\0\0\0"                -- zero mask keeps payload unchanged
        if n < 126 then
          hdr = hdr .. string.char(0x80 | n)
        elseif n < 65536 then
          hdr = hdr .. string.char(0x80 | 126) .. string.pack(">I2", n)
        else
          hdr = hdr .. string.char(0x80 | 127) .. string.pack(">I8", n)
        end
        return hdr .. mask .. payload
      end

      local function read_server_frame()
        -- Read enough bytes to parse a server frame header (no mask on server side)
        local function need(k)
          while #rest < k do
            local chunk, err = c:receive()
            if not chunk then return nil, err end
            rest = rest .. chunk
          end
          return true
        end
        if not need(2) then return nil end
        local b1, b2 = rest:byte(1), rest:byte(2)
        local opcode = b1 & 0x0f
        local n = b2 & 0x7f
        local off = 3
        if n == 126 then
          if not need(4) then return nil end
          n = string.unpack(">I2", rest, 3); off = 5
        elseif n == 127 then
          if not need(10) then return nil end
          n = string.unpack(">I8", rest, 3); off = 11
        end
        if not need(off - 1 + n) then return nil end
        local payload = rest:sub(off, off + n - 1)
        rest = rest:sub(off + n)
        return payload, opcode
      end

      for i = 1, N do
        local req = json.encode{ seq = i, kind = (i % 2 == 0) and "square" or "cube", n = i }
        c:send(encode_client_frame(req))
        local resp = read_server_frame()
        if resp then
          local ok, obj = pcall(json.decode, resp)
          if ok and type(obj) == "table" then
            client_replies[i] = obj
          end
        end
      end
      c:close()
      done = true
      -- Terminate the worker pool from inside a coroutine — its reap
      -- loop calls fan.sleep(), which needs a coroutine context.
      w:terminate()
      w = nil
      fan.loopbreak()
    end)
  end)
  if server then server:close() end

  T.eq(#client_replies, N, "got only " .. #client_replies .. "/" .. N .. " replies")
  for i = 1, N do
    local r = client_replies[i]
    T.eq(r.seq, i, "reply " .. i .. " has wrong seq")
    if i % 2 == 0 then
      T.eq(r.result, i * i, "square(" .. i .. ") wrong")
    else
      T.eq(r.result, i * i * i, "cube(" .. i .. ") wrong")
    end
  end
end)

-- ==========================================================================
-- integ-4: mariadb pool + orm + N concurrent — skip if no local mariadb
-- ==========================================================================

s:test("integ-4: fan.mariadb.pool + fan.orm — N concurrent orm queries via pool:with",
function()
  local ok_load, mariadb = pcall(require, "fan.mariadb")
  if not ok_load then
    print("[SKIP] integ-4: fan.mariadb module not loaded (" .. tostring(mariadb) .. ")")
    return
  end
  local mpool = require("fan.mariadb.pool")
  local orm   = require("fan.orm")

  -- Probe for a local server; skip if unavailable.
  local base_opts = {
    unix_socket = "/run/mysqld/mysqld.sock",
    user        = "root",
    password    = "",
  }
  local probe, perr = mariadb.connect(base_opts)
  if not probe then
    print("[SKIP] integ-4: no local MariaDB (" .. tostring(perr) .. ")")
    return
  end
  local DBNAME = "lf2_integ_" .. tostring(os.time()) .. "_" .. tostring(math.random(1e6))
  probe:exec("CREATE DATABASE IF NOT EXISTS `" .. DBNAME .. "`")
  probe:close()

  local N = 5   -- 5 concurrent coroutines competing for pool conns (max_size=3)
  local completed = 0
  local values = {}
  local run_done = false

  with_loop(function()
    -- One-time setup: create table via a dedicated conn (not from pool
    -- so we don't block a slot). Uses the same DB.
    do
      local setup = assert(mariadb.connect{
        unix_socket = base_opts.unix_socket,
        user = base_opts.user, password = base_opts.password,
        database = DBNAME,
      })
      setup:exec("DROP TABLE IF EXISTS items")
      setup:exec("CREATE TABLE items (id INT AUTO_INCREMENT PRIMARY KEY, val INT)")
      setup:close()
    end

    local p = assert(mpool.new{
      unix_socket = base_opts.unix_socket,
      user        = base_opts.user,
      password    = base_opts.password,
      database    = DBNAME,
      max_size    = 3,
    })

    watchdog(30, function() return run_done end, "integ-4 mariadb pool + orm")

    for i = 1, N do
      fan.spawn(function()
        local rv, err = p:with(function(db)
          local drv = orm.mariadb_driver(db)
          local ctx = orm.new_context(drv)
          local Item = ctx:define("items", {
            id  = "INT AUTO_INCREMENT PRIMARY KEY",
            val = "INT",
          })
          Item.insert{ val = i * 10 }
          local row = Item.find_by{ val = i * 10 }
          return row and row.val
        end)
        if rv then
          values[i] = rv
          completed = completed + 1
        else
          print("[integ-4] pool:with failed at i=" .. i .. ": " .. tostring(err))
        end
        if completed == N then
          run_done = true
          -- drop the temp DB via a fresh conn (pool may still hold live conns)
          local cleanup = mariadb.connect(base_opts)
          if cleanup then
            cleanup:exec("DROP DATABASE IF EXISTS `" .. DBNAME .. "`")
            cleanup:close()
          end
          p:close()
          fan.loopbreak()
        end
      end)
    end
  end)

  T.eq(completed, N, "only " .. completed .. "/" .. N .. " coroutines completed")
  for i = 1, N do
    T.eq(values[i], i * 10, "value mismatch at i=" .. i)
  end
end)

os.exit(T.run(s))
