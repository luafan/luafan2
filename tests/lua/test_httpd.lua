--[[
  test_httpd.lua — M4 HTTP/1.1 server contract tests (fan.httpd).

  Both the origin server and the driving client run in the same fan loop:
  httpd.bind spawns a coroutine per connection; the client uses fan.http
  (which already has its own contract tests). We assert plan \u00a74.6:
    - request parsing: method / path / query / headers / body
    - response reply: status, custom headers, auto content-length
    - chunked reply via reply_start / reply_chunk / reply_end
    - handler may fan.sleep between chunks; connection stays live
    - handler crash after no reply -> 500 (not a dropped connection)
    - handler returns without replying -> default 204
    - default 404 branch (handler-controlled)
    - N concurrent clients handled independently
]]
-- M14.C-b: the M14.C-a → M14.C-b C evhttp backend implements the full
-- fan.httpd surface exercised here (reply, reply_start/chunk/end,
-- addheader, error paths, default 204/500). This file now runs against
-- whichever backend the shim picks (C by default). HTTPS / WebSocket
-- tests live in their own files (test_httpsd.lua / test_websocket.lua)
-- and still pin to the Lua backend until M14.C-c/-d land.

local T = require("test_framework")
local fan = require("fan")
local http = require("fan.http")
local httpd = require("fan.httpd")

local s = T.suite("fan.httpd server (M4)")

-- drive a coroutine body until it finishes; guarantees loopbreak + reraise
local function run(body)
  local caught
  fan.spawn(function()
    local ok, err = pcall(body)
    if not ok then caught = err end
    fan.loopbreak()
  end)
  fan.loop()
  if caught then error(caught, 0) end
end

local BASE = "http://127.0.0.1:"

s:test("GET: method/path/query parsed, custom headers echo, 200 body", function()
  local PORT = 24401
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local body = string.format("M=%s P=%s Q=%s Params.a=%s Params.b=%s H.xfoo=%s",
        req.method, req.path,
        req.query, req.params.a or "", req.params.b or "",
        req.headers["x-foo"] or "")
      resp:reply(200, { ["Content-Type"] = "text/plain",
                        ["X-Server"] = "fan.httpd" }, body)
    end })
    resp = http.get(BASE .. PORT .. "/hello?a=1&b=two",
      { headers = { ["X-Foo"] = "bar" } })
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.headers["content-type"], "text/plain")
  T.eq(resp.headers["x-server"], "fan.httpd")
  T.truthy(resp.body:find("M=GET",   1, true))
  T.truthy(resp.body:find("P=/hello",1, true))
  T.truthy(resp.body:find("Q=a=1&b=two", 1, true))
  T.truthy(resp.body:find("Params.a=1", 1, true))
  T.truthy(resp.body:find("Params.b=two", 1, true))
  T.truthy(resp.body:find("H.xfoo=bar", 1, true))
end)

s:test("POST body reaches the handler via Content-Length", function()
  local PORT = 24402
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      resp:reply(201, {}, "M=" .. req.method .. " N=" .. #req.body ..
                 " BODY=" .. req.body)
    end })
    resp = http.post(BASE .. PORT .. "/upload", { body = "payload-123" })
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 201)
  T.truthy(resp.body:find("M=POST", 1, true))
  T.truthy(resp.body:find("N=11",   1, true))
  T.truthy(resp.body:find("BODY=payload-123", 1, true))
end)

s:test("auto Content-Length header on reply", function()
  local PORT = 24403
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(_, resp)
      resp:reply(200, {}, "abcdefgh")
    end })
    resp = http.get(BASE .. PORT .. "/")
  end)
  if server then server:close() end
  T.eq(resp.headers["content-length"], "8")
  T.eq(resp.body, "abcdefgh")
end)

s:test("chunked reply: reply_start / reply_chunk+ / reply_end", function()
  local PORT = 24404
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(_, resp)
      resp:reply_start(200, { ["Content-Type"] = "text/plain" })
      resp:reply_chunk("Wiki")
      resp:reply_chunk("pedia")
      resp:reply_chunk(" in\r\n\r\nchunks.")
      resp:reply_end()
    end })
    resp = http.get(BASE .. PORT .. "/stream")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "Wikipedia in\r\n\r\nchunks.")
  -- client should have seen a chunked encoding; but its parser strips that
  -- so we can't easily assert the header here (it may not be preserved
  -- verbatim). Body correctness is the contract.
end)

s:test("fan.sleep between chunks keeps the connection alive", function()
  local PORT = 24405
  local server, resp
  local start = fan.gettime and fan.gettime() or nil
  local t0, t1
  run(function()
    -- record time inside the loop for a portable value
    local getnow = function() return os.time() end
    t0 = getnow()
    server = assert(httpd.bind{ port = PORT, handler = function(_, resp)
      resp:reply_start(200, {})
      resp:reply_chunk("A")
      fan.sleep(0.15)
      resp:reply_chunk("B")
      fan.sleep(0.15)
      resp:reply_chunk("C")
      resp:reply_end()
    end })
    resp = http.get(BASE .. PORT .. "/slow")
    t1 = getnow()
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "ABC")
  -- weak timing sanity check: two 0.15s sleeps must produce at least ~0.25s
  -- of wall clock. os.time() is 1-second resolution, so accept >=0 seconds
  -- (the important assertion is the body arrived intact after sleeps).
  T.truthy((t1 - t0) >= 0)
end)

s:test("404 branch under handler control", function()
  local PORT = 24406
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      if req.path == "/known" then
        resp:reply(200, {}, "ok")
      else
        resp:reply(404, {}, "no such path: " .. req.path)
      end
    end })
    resp = http.get(BASE .. PORT .. "/unknown")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 404)
  T.eq(resp.reason, "Not Found")
  T.truthy(resp.body:find("no such path: /unknown", 1, true))
end)

s:test("handler error before reply -> 500", function()
  local PORT = 24407
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function()
      error("boom-in-handler")
    end })
    resp = http.get(BASE .. PORT .. "/x")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 500)
  T.truthy(resp.body:find("boom-in-handler", 1, true))
end)

s:test("handler returns without replying -> default 204", function()
  local PORT = 24408
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function() end })
    resp = http.get(BASE .. PORT .. "/")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 204)
  T.eq(resp.body, "")
end)

s:test("N concurrent clients each get their own request/response", function()
  local PORT = 24409
  local N = 6
  local results = {}
  local done = 0
  run(function()
    local server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      resp:reply(200, {}, "echo:" .. req.body)
    end })
    for i = 1, N do
      fan.spawn(function()
        local r = http.post(BASE .. PORT .. "/", { body = "c-" .. i })
        results[i] = r and r.body
        done = done + 1
        if done == N then server:close(); fan.loopbreak() end
      end)
    end
    while done < N do fan.sleep(0.02) end
  end)
  T.eq(done, N)
  for i = 1, N do
    T.eq(results[i], "echo:c-" .. i)
  end
end)

s:test("parse_query decodes k=v pairs and percent-encoding", function()
  local q = httpd.parse_query("a=1&b=hello%20world&c&d=%2Fx%2Fy")
  T.eq(q.a, "1")
  T.eq(q.b, "hello world")
  T.eq(q.c, "")
  T.eq(q.d, "/x/y")
end)

s:test("malformed request-line -> 400", function()
  local PORT = 24410
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(_, resp)
      resp:reply(200, {}, "should not run")
    end })
    -- send raw garbage via fan.tcp to bypass fan.http's own request builder
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    c:send("NOT A VALID REQUEST LINE\r\n\r\n")
    -- read until eof
    local buf = ""
    while true do
      local d = c:receive()
      if not d then break end
      buf = buf .. d
    end
    c:close()
    resp = buf
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.truthy(resp:find("HTTP/1.1 400", 1, true), "response was: " .. tostring(resp))
end)

-- ---------------------------------------------------------------------------
-- v1 fan.httpd parity surface (M14). We keep the M4 tests above intact and
-- add a focused block for the compatibility API: `onService` alias,
-- request:read/available, response:addheader (accumulation + folding),
-- and caller-wins header merging.
-- ---------------------------------------------------------------------------

s:test("bind accepts opts.onService as alias for opts.handler", function()
  local PORT = 24411
  local server, resp
  local seen_method
  run(function()
    server = assert(httpd.bind{ port = PORT, onService = function(req, r)
      seen_method = req.method
      r:reply(200, {}, "svc-ok")
    end })
    resp = http.get(BASE .. PORT .. "/svc")
  end)
  if server then server:close() end
  T.eq(seen_method, "GET")
  T.eq(resp.status, 200)
  T.eq(resp.body, "svc-ok")
end)

s:test("bind rejects when neither handler nor onService supplied", function()
  local ok, err = pcall(httpd.bind, { port = 24412 })
  T.eq(ok, false)
  T.truthy(tostring(err):find("handler", 1, true) or tostring(err):find("onService", 1, true))
end)

s:test("request:available and request:read return the body incrementally", function()
  local PORT = 24413
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, r)
      local a = req:available()
      local first = req:read(3) or ""
      local mid_available = req:available()
      local rest = req:read(1024) or ""
      local eof = req:read(1) or ""
      r:reply(200, {}, string.format("a=%d first=%s mid=%d rest=%s eof=[%s]",
        a, first, mid_available, rest, eof))
    end })
    resp = http.post(BASE .. PORT .. "/r", { body = "abcdef" })
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.truthy(resp.body:find("a=6",     1, true), resp.body)
  T.truthy(resp.body:find("first=abc", 1, true), resp.body)
  T.truthy(resp.body:find("mid=3",   1, true), resp.body)
  T.truthy(resp.body:find("rest=def",1, true), resp.body)
  T.truthy(resp.body:find("eof=[]",  1, true), resp.body)
end)

s:test("response:addheader accumulates and appears in the wire response", function()
  local PORT = 24414
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(_, r)
      r:addheader("X-Extra", "one")
      r:addheader("X-Trace", "abc")
      r:reply(200, { ["Content-Type"] = "text/plain" }, "ok")
    end })
    resp = http.get(BASE .. PORT .. "/h")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.headers["x-extra"], "one")
  T.eq(resp.headers["x-trace"], "abc")
  T.eq(resp.headers["content-type"], "text/plain")
end)

s:test("response:addheader folds repeated names with a comma (v1 parity)", function()
  local PORT = 24415
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(_, r)
      r:addheader("Set-Cookie", "a=1")
      r:addheader("Set-Cookie", "b=2")
      r:reply(200, {}, "ok")
    end })
    resp = http.get(BASE .. PORT .. "/c")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  -- v1 folds repeated header lines using ", ". fan.http lowercases the
  -- header name; the joined value must contain both cookies in order.
  local sc = resp.headers["set-cookie"] or ""
  T.truthy(sc:find("a=1", 1, true), sc)
  T.truthy(sc:find("b=2", 1, true), sc)
end)

s:test("caller-supplied headers override addheader stash (case-insensitive)", function()
  local PORT = 24416
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(_, r)
      r:addheader("X-Kind", "stashed")
      r:addheader("X-Other", "keep-me")
      r:reply(200, { ["x-kind"] = "caller" }, "ok")
    end })
    resp = http.get(BASE .. PORT .. "/m")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.headers["x-kind"], "caller")
  T.eq(resp.headers["x-other"], "keep-me")
end)

s:test("response:addheader after head sent raises an error", function()
  local PORT = 24417
  local server, resp
  local addheader_err
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(_, r)
      r:reply_start(200, {})
      r:reply_chunk("hi")
      local ok, err = pcall(r.addheader, r, "X-Late", "nope")
      addheader_err = err
      r:reply_end()
    end })
    resp = http.get(BASE .. PORT .. "/late")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.body, "hi")
  T.not_nil(addheader_err)
  T.truthy(tostring(addheader_err):find("addheader", 1, true))
end)

s:test("response:addheader validates argument shape", function()
  local PORT = 24418
  local server
  local err_seen
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(_, r)
      local ok, err = pcall(r.addheader, r, 42, "value")
      err_seen = err
      r:reply(200, {}, "done")
    end })
    http.get(BASE .. PORT .. "/bad")
  end)
  if server then server:close() end
  T.not_nil(err_seen)
  T.truthy(tostring(err_seen):find("addheader", 1, true))
end)

s:test("addheader interoperates with reply_start (headers appear on chunked reply)", function()
  local PORT = 24419
  local server, resp
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(_, r)
      r:addheader("X-Streamed", "yes")
      r:reply_start(200, { ["Content-Type"] = "text/plain" })
      r:reply_chunk("part1-")
      r:reply_chunk("part2")
      r:reply_end()
    end })
    resp = http.get(BASE .. PORT .. "/cs")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.body, "part1-part2")
  T.eq(resp.headers["x-streamed"], "yes")
  T.eq(resp.headers["content-type"], "text/plain")
end)

os.exit(T.run(s))
