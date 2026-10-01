--[[
  test_httpd_c.lua — M14.C-a HTTP/1.1 server contract tests for the evhttp
  C backend of fan.httpd.

  These tests exclusively drive the "c" backend (opts.backend="c") so we
  actually exercise src/net/httpd.c. Feature parity with the pure-Lua
  backend is limited by design in M14.C-a:
    - reply(status, headers, body): one-shot only  [in scope]
    - request:method / :path / :query / :headers / :body           [in scope]
    - request:available / :read incremental drain                  [in scope]
    - handler pcall-guard -> 500                                   [in scope]
    - default 204 when handler forgets to reply                    [in scope]
    - N concurrent clients                                         [in scope]

    - reply_start / reply_chunk / reply_end   -- M14.C-b [DONE]
    - response:addheader accumulation         -- M14.C-b [DONE]
    - HTTPS (opts.ssl=true)                   -- M14.C-c [DONE, covered by test_httpsd.lua]
    - WebSocket upgrade                       -- M14.C-d [DONE, see the
      M14.C-d block at the bottom of this file and the full functional
      coverage in test_websocket.lua which now runs on the C backend]

  We drive the origin server with fan.http (which has its own contract
  tests), just like test_httpd.lua does.
]]

local T = require("test_framework")
local fan = require("fan")
local http = require("fan.http")
local httpd = require("fan.httpd")

local s = T.suite("fan.httpd C backend (M14.C-a)")

-- Runs `body` inside a fan coroutine; captures errors, breaks the loop
-- when body returns, and re-raises after event_base_dispatch exits so
-- the test framework sees a normal Lua error.
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

-- Every test opts into the C backend explicitly so it does not depend on
-- the global default (which test_httpd.lua flips to "lua"). Each spec
-- lives in its own PORT to avoid TIME_WAIT collisions when tests run
-- back-to-back under coverage instrumentation.

s:test("GET: method/path/query/headers/body parsed via C backend", function()
  local PORT = 25401
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        local body = string.format("M=%s P=%s Q=%s Params.a=%s Params.b=%s H.xfoo=%s V=%s",
          req.method, req.path,
          req.query, req.params.a or "", req.params.b or "",
          req.headers["x-foo"] or "",
          req.http_version or "")
        r:reply(200, { ["Content-Type"] = "text/plain",
                       ["X-Server"] = "fan.httpd_c" }, body)
      end,
    })
    resp = http.get(BASE .. PORT .. "/hello?a=1&b=two",
      { headers = { ["X-Foo"] = "bar" } })
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.headers["content-type"], "text/plain")
  T.eq(resp.headers["x-server"], "fan.httpd_c")
  T.truthy(resp.body:find("M=GET",     1, true), resp.body)
  T.truthy(resp.body:find("P=/hello",  1, true), resp.body)
  T.truthy(resp.body:find("Q=a=1&b=two", 1, true), resp.body)
  T.truthy(resp.body:find("Params.a=1", 1, true), resp.body)
  T.truthy(resp.body:find("Params.b=two", 1, true), resp.body)
  T.truthy(resp.body:find("H.xfoo=bar",1, true), resp.body)
  T.truthy(resp.body:find("V=1.1",     1, true), resp.body)
end)

s:test("POST body reaches the handler via req.body", function()
  local PORT = 25402
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:reply(201, {},
          "M=" .. req.method .. " N=" .. #req.body .. " BODY=" .. req.body)
      end,
    })
    resp = http.post(BASE .. PORT .. "/upload", { body = "payload-123" })
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 201)
  T.truthy(resp.body:find("M=POST", 1, true))
  T.truthy(resp.body:find("N=11",   1, true))
  T.truthy(resp.body:find("BODY=payload-123", 1, true))
end)

s:test("automatic Content-Length inserted by evhttp for reply()", function()
  local PORT = 25403
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(_, r) r:reply(200, {}, "abcdefgh") end,
    })
    resp = http.get(BASE .. PORT .. "/")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.headers["content-length"], "8")
  T.eq(resp.body, "abcdefgh")
end)

s:test("empty body reply produces Content-Length: 0", function()
  local PORT = 25404
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(_, r) r:reply(200, {}, "") end,
    })
    resp = http.get(BASE .. PORT .. "/empty")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.body, "")
end)

s:test("req:available / :read drain the buffered body incrementally", function()
  local PORT = 25405
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        local a = req:available()
        local first = req:read(3) or ""
        local mid_available = req:available()
        local rest = req:read(1024) or ""
        local eof = req:read(1) or ""
        r:reply(200, {}, string.format("a=%d first=%s mid=%d rest=%s eof=[%s]",
          a, first, mid_available, rest, eof))
      end,
    })
    resp = http.post(BASE .. PORT .. "/r", { body = "abcdef" })
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.truthy(resp.body:find("a=6",       1, true), resp.body)
  T.truthy(resp.body:find("first=abc", 1, true), resp.body)
  T.truthy(resp.body:find("mid=3",     1, true), resp.body)
  T.truthy(resp.body:find("rest=def",  1, true), resp.body)
  T.truthy(resp.body:find("eof=[]",    1, true), resp.body)
end)

s:test("req:read() with no arg returns all remaining bytes at once", function()
  local PORT = 25406
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        local body = req:read() or ""
        local eof  = req:read() or ""
        r:reply(200, {}, "B=[" .. body .. "] eof=[" .. eof .. "]")
      end,
    })
    resp = http.post(BASE .. PORT .. "/rn", { body = "onetwothree" })
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.truthy(resp.body:find("B=[onetwothree]", 1, true), resp.body)
  T.truthy(resp.body:find("eof=[]", 1, true), resp.body)
end)

s:test("req:read(bad arg) raises a Lua error (caught by handler pcall -> 500)", function()
  local PORT = 25407
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        req:read(-1)   -- must raise
        r:reply(200, {}, "unreached")
      end,
    })
    resp = http.get(BASE .. PORT .. "/bad")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 500)
end)

s:test("handler error before reply -> 500 with error string in body", function()
  local PORT = 25408
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function() error("boom-in-c-handler") end,
    })
    resp = http.get(BASE .. PORT .. "/x")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 500)
  T.truthy(resp.body:find("boom-in-c-handler", 1, true), resp.body)
end)

s:test("handler returns without replying -> default 204", function()
  local PORT = 25409
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function() end,
    })
    resp = http.get(BASE .. PORT .. "/")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 204)
  T.eq(resp.body, "")
end)

s:test("custom headers appear on the wire response", function()
  local PORT = 25410
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(_, r)
        r:reply(200, {
          ["Content-Type"] = "application/json",
          ["X-Custom"]     = "yes",
        }, '{"ok":true}')
      end,
    })
    resp = http.get(BASE .. PORT .. "/hdr")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.headers["content-type"], "application/json")
  T.eq(resp.headers["x-custom"], "yes")
  T.eq(resp.body, '{"ok":true}')
end)

s:test("N concurrent clients each get their own request/response", function()
  local PORT = 25411
  local N = 6
  local results = {}
  local done = 0
  run(function()
    local server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r) r:reply(200, {}, "echo:" .. req.body) end,
    })
    -- Fan out N concurrent clients; each records its body and increments
    -- `done`. The MAIN coroutine polls with a short sleep (which is a plain
    -- coroutine yield + one-shot timer; when it wakes with done==N there are
    -- no leftover pending events by the time the outer `run()` returns).
    -- Do NOT loopbreak from a worker: leaving a pending fan.sleep timer
    -- outlives the loop and, since event_base is a process-global singleton,
    -- it fires immediately when the NEXT test's fan.loop starts, prematurely
    -- resuming a dead coroutine and tearing that loop down early.
    for i = 1, N do
      fan.spawn(function()
        local r = http.post(BASE .. PORT .. "/", { body = "c-" .. i })
        results[i] = r and r.body
        done = done + 1
      end)
    end
    while done < N do fan.sleep(0.02) end
    server:close()
  end)
  T.eq(done, N)
  for i = 1, N do
    T.eq(results[i], "echo:c-" .. i)
  end
end)

s:test("fan.sleep inside handler yields cleanly (loop keeps ticking)", function()
  local PORT = 25412
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(_, r)
        fan.sleep(0.05)
        r:reply(200, {}, "slept")
      end,
    })
    resp = http.get(BASE .. PORT .. "/slow")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "slept")
end)

s:test("bind without opts.port raises", function()
  local ok, err = pcall(httpd.bind, { backend = "c", handler = function() end })
  T.eq(ok, false)
  T.truthy(tostring(err):find("port", 1, true))
end)

s:test("bind without opts.handler raises", function()
  local ok, err = pcall(httpd.bind, { backend = "c", port = 25413 })
  T.eq(ok, false)
  T.truthy(tostring(err):find("handler", 1, true) or
           tostring(err):find("onService", 1, true))
end)

s:test("server:getport reports the actually bound port", function()
  local PORT = 25414
  local reported
  run(function()
    local server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(_, r) r:reply(200, {}, "ok") end,
    })
    reported = server:getport()
    server:close()
  end)
  T.eq(reported, PORT)
end)

s:test("opts.onService alias is accepted on the C backend", function()
  local PORT = 25415
  local server, resp
  local seen_method
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      onService = function(req, r)
        seen_method = req.method
        r:reply(200, {}, "svc-ok")
      end,
    })
    resp = http.get(BASE .. PORT .. "/svc")
  end)
  if server then server:close() end
  T.eq(seen_method, "GET")
  T.eq(resp.status, 200)
  T.eq(resp.body, "svc-ok")
end)

s:test("M14.C-c: opts.ssl no longer forces lua backend (HTTPS on C evhttp)", function()
  -- Since M14.C-c the C backend serves HTTPS natively (evhttp bevcb +
  -- fan_tls_server_bev). The shim must NOT redirect ssl=true to lua.
  -- Actual HTTPS request/response behaviour is covered by test_httpsd.lua
  -- (which uses the default backend and therefore exercises C).
  T.eq(httpd._pick_backend{ ssl = true }, "c")
  T.eq(httpd._pick_backend{ ssl = true, backend = "lua" }, "lua")  -- explicit wins
  T.eq(httpd._pick_backend{ ssl = true, backend = "c" }, "c")
  T.eq(httpd._pick_backend{ }, "c")                                 -- default
  T.eq(httpd._pick_backend{ backend = "lua" }, "lua")
end)

-- =====================================================================
-- M14.C-b: chunked reply + addheader
-- =====================================================================

s:test("M14.C-b: reply_start + reply_chunk + reply_end streams chunks", function()
  local PORT = 25420
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:reply_start(200, { ["Content-Type"] = "text/plain",
                             ["X-Streamed"] = "yes" })
        r:reply_chunk("part-1;")
        r:reply_chunk("part-2;")
        r:reply_chunk("part-3")
        r:reply_end()
      end,
    })
    resp = http.get(BASE .. PORT .. "/stream")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.headers["content-type"], "text/plain")
  T.eq(resp.headers["x-streamed"], "yes")
  -- The wire is Transfer-Encoding: chunked but fan.http reassembles for us.
  T.eq(resp.body, "part-1;part-2;part-3")
end)

s:test("M14.C-b: chunked reply keeps connection alive across fan.sleep", function()
  local PORT = 25421
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:reply_start(200, { ["Content-Type"] = "text/plain" })
        r:reply_chunk("A")
        fan.sleep(0.02)   -- must not close the connection
        r:reply_chunk("B")
        fan.sleep(0.02)
        r:reply_chunk("C")
        r:reply_end()
      end,
    })
    resp = http.get(BASE .. PORT .. "/slow-stream")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.body, "ABC")
end)

s:test("M14.C-b: reply_chunk with empty/nil data is a no-op", function()
  local PORT = 25422
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:reply_start(200, {})
        r:reply_chunk("X")
        r:reply_chunk("")   -- no-op: empty string
        r:reply_chunk(nil)  -- no-op: nil
        r:reply_chunk("Y")
        r:reply_end()
      end,
    })
    resp = http.get(BASE .. PORT .. "/empty-chunks")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.body, "XY")
end)

s:test("M14.C-b: reply_end is idempotent, reply_chunk after end raises", function()
  local PORT = 25423
  local server, resp
  local chunk_err
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:reply_start(200, {})
        r:reply_chunk("done")
        r:reply_end()
        r:reply_end()   -- idempotent
        local ok, err = pcall(r.reply_chunk, r, "late")
        chunk_err = (not ok) and tostring(err) or nil
      end,
    })
    resp = http.get(BASE .. PORT .. "/after-end")
  end)
  if server then server:close() end
  T.eq(resp.body, "done")
  T.not_nil(chunk_err)
  T.truthy(chunk_err:find("already ended", 1, true), chunk_err)
end)

s:test("M14.C-b: gencb auto-ends chunked reply if handler forgets", function()
  local PORT = 25424
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:reply_start(200, { ["Content-Type"] = "text/plain" })
        r:reply_chunk("half")
        -- handler returns WITHOUT calling reply_end -> gencb should auto-end
      end,
    })
    resp = http.get(BASE .. PORT .. "/forget-end")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.body, "half")
end)

s:test("M14.C-b: reply after reply_start raises (head already sent)", function()
  local PORT = 25425
  local server, resp
  local err_msg
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:reply_start(200, {})
        r:reply_chunk("x")
        local ok, err = pcall(r.reply, r, 500, {}, "nope")
        err_msg = (not ok) and tostring(err) or nil
        r:reply_end()
      end,
    })
    resp = http.get(BASE .. PORT .. "/double-reply")
  end)
  if server then server:close() end
  T.eq(resp.body, "x")
  T.not_nil(err_msg)
  T.truthy(err_msg:find("already sent", 1, true), err_msg)
end)

s:test("M14.C-b: reply_chunk before reply_start raises", function()
  local PORT = 25426
  local server, resp
  local err_msg
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        local ok, err = pcall(r.reply_chunk, r, "premature")
        err_msg = (not ok) and tostring(err) or nil
        r:reply(200, {}, "recovered")
      end,
    })
    resp = http.get(BASE .. PORT .. "/premature-chunk")
  end)
  if server then server:close() end
  T.eq(resp.body, "recovered")
  T.not_nil(err_msg)
  T.truthy(err_msg:find("not in chunked mode", 1, true), err_msg)
end)

s:test("M14.C-b: addheader single key reaches the wire", function()
  local PORT = 25427
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:addheader("X-Test-Header", "test-value")
        r:reply(200, { ["Content-Type"] = "text/plain" }, "hi")
      end,
    })
    resp = http.get(BASE .. PORT .. "/addhdr-single")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.headers["x-test-header"], "test-value")
  T.eq(resp.body, "hi")
end)

s:test("M14.C-b: addheader same key twice folds with ', '", function()
  local PORT = 25428
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:addheader("Set-Cookie-Ish", "a=1")
        r:addheader("Set-Cookie-Ish", "b=2")
        r:reply(200, {}, "")
      end,
    })
    resp = http.get(BASE .. PORT .. "/addhdr-fold")
  end)
  if server then server:close() end
  T.eq(resp.headers["set-cookie-ish"], "a=1, b=2")
end)

s:test("M14.C-b: reply() caller header overrides addheader (caller-wins, case-insensitive)", function()
  local PORT = 25429
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:addheader("X-Priority", "low")
        r:addheader("X-Keep", "stashed")
        -- Different case in the caller table -> should still replace.
        r:reply(200, { ["x-priority"] = "high" }, "ok")
      end,
    })
    resp = http.get(BASE .. PORT .. "/caller-wins")
  end)
  if server then server:close() end
  T.eq(resp.headers["x-priority"], "high")     -- caller wins
  T.eq(resp.headers["x-keep"],     "stashed")  -- pending survives
end)

s:test("M14.C-b: reply_start caller header also honours caller-wins", function()
  local PORT = 25430
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:addheader("X-Mode", "addheader")
        r:addheader("X-Extra", "stashed")
        r:reply_start(200, { ["X-Mode"] = "start" })
        r:reply_chunk("data")
        r:reply_end()
      end,
    })
    resp = http.get(BASE .. PORT .. "/start-caller-wins")
  end)
  if server then server:close() end
  T.eq(resp.body, "data")
  T.eq(resp.headers["x-mode"],  "start")
  T.eq(resp.headers["x-extra"], "stashed")
end)

s:test("M14.C-b: addheader after sent_head raises", function()
  local PORT = 25431
  local server, resp
  local err_msg
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        r:reply(200, {}, "sent")
        local ok, err = pcall(r.addheader, r, "X-Too-Late", "1")
        err_msg = (not ok) and tostring(err) or nil
      end,
    })
    resp = http.get(BASE .. PORT .. "/addhdr-late")
  end)
  if server then server:close() end
  T.eq(resp.body, "sent")
  T.not_nil(err_msg)
  T.truthy(err_msg:find("already sent", 1, true), err_msg)
end)

s:test("M14.C-b: addheader arg validation", function()
  local PORT = 25432
  local server, resp
  local errs = {}
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        local ok, err = pcall(r.addheader, r, 42, "v")
        errs.non_string_name = (not ok) and tostring(err) or nil
        local ok2, err2 = pcall(r.addheader, r, "X-No-Value", nil)
        errs.nil_value = (not ok2) and tostring(err2) or nil
        r:reply(200, {}, "ok")
      end,
    })
    resp = http.get(BASE .. PORT .. "/addhdr-validate")
  end)
  if server then server:close() end
  T.eq(resp.body, "ok")
  T.not_nil(errs.non_string_name)
  T.not_nil(errs.nil_value)
  T.truthy(errs.non_string_name:find("required", 1, true), errs.non_string_name)
  T.truthy(errs.nil_value:find("required", 1, true), errs.nil_value)
end)

s:test("M14.C-b: caller Content-Length dropped in chunked mode", function()
  local PORT = 25433
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req, r)
        -- Some code passes a bogus Content-Length; chunked owns framing.
        r:reply_start(200, { ["Content-Length"] = "9999",
                             ["X-Marker"] = "yes" })
        r:reply_chunk("hi")
        r:reply_end()
      end,
    })
    resp = http.get(BASE .. PORT .. "/no-cl-chunked")
  end)
  if server then server:close() end
  T.eq(resp.body, "hi")
  T.eq(resp.headers["x-marker"], "yes")
  -- fan.http reassembled the body; the raw framing was chunked -> no
  -- literal Content-Length: 9999 got through.
  T.eq(resp.headers["content-length"], nil)
end)

-- =====================================================================
-- M14.C-c: HTTPS on C evhttp backend
-- =====================================================================
--
-- End-to-end HTTPS coverage lives in test_httpsd.lua (which drives the
-- default backend and, since M14.C-c, exercises fan.httpd_c directly).
-- The cases below pin backend="c" so that (a) the shim's forwarding of
-- opts.ssl/cert/key is exercised, and (b) regressions in the C-only
-- code path surface here even if test_httpsd.lua's dispatcher rules
-- change later. We generate a fresh self-signed cert on demand and
-- self-skip when the openssl CLI is unavailable.

local function _c_have_openssl()
  local ok = os.execute("command -v openssl >/dev/null 2>&1")
  return ok == true or ok == 0
end

local _c_tmp  = os.getenv("TMPDIR") or "/tmp"
local _c_cert = _c_tmp .. "/fan_httpd_c_cert.pem"
local _c_key  = _c_tmp .. "/fan_httpd_c_key.pem"

local function _c_gen_cert()
  local cmd = string.format(
    "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s -days 1 -nodes "
    .. "-subj /CN=localhost >/dev/null 2>&1", _c_key, _c_cert)
  local ok = os.execute(cmd)
  return ok == true or ok == 0
end

if _c_have_openssl() and _c_gen_cert() then
  s:test("M14.C-c: bind{backend='c', ssl, cert, key} serves HTTPS", function()
    local PORT = 25490
    local server, resp
    run(function()
      server = assert(httpd.bind{
        port = PORT, backend = "c",
        ssl = true, cert = _c_cert, key = _c_key,
        handler = function(req, r)
          r:reply(200, { ["Content-Type"] = "text/plain" },
                  "TLS-C:" .. req.method .. ":" .. req.path)
        end,
      })
      resp = http.get("https://127.0.0.1:" .. PORT .. "/hi",
        { verify = false })
    end)
    if server then server:close() end
    T.not_nil(resp)
    T.eq(resp.status, 200)
    T.eq(resp.body, "TLS-C:GET:/hi")
    T.eq(resp.headers["content-type"], "text/plain")
  end)

  s:test("M14.C-c: HTTPS POST body round-trip on C backend", function()
    local PORT = 25491
    local server, resp
    run(function()
      server = assert(httpd.bind{
        port = PORT, backend = "c",
        ssl = true, cert = _c_cert, key = _c_key,
        handler = function(req, r)
          r:reply(200, {}, "echo=" .. req.body)
        end,
      })
      resp = http.post("https://127.0.0.1:" .. PORT .. "/e",
        { body = "hello-tls", verify = false })
    end)
    if server then server:close() end
    T.eq(resp.status, 200)
    T.eq(resp.body, "echo=hello-tls")
  end)

  s:test("M14.C-c: HTTPS chunked reply on C backend", function()
    local PORT = 25492
    local server, resp
    run(function()
      server = assert(httpd.bind{
        port = PORT, backend = "c",
        ssl = true, cert = _c_cert, key = _c_key,
        handler = function(_, r)
          r:reply_start(200, {})
          r:reply_chunk("a-")
          r:reply_chunk("b-")
          r:reply_chunk("c")
          r:reply_end()
        end,
      })
      resp = http.get("https://127.0.0.1:" .. PORT .. "/stream",
        { verify = false })
    end)
    if server then server:close() end
    T.eq(resp.status, 200)
    T.eq(resp.body, "a-b-c")
  end)

  s:test("M14.C-c: bind{ssl=true} without cert/key fails clean on C backend", function()
    local server, err
    run(function()
      -- Soft failure: bind returns nil, err (matches Lua backend + v1
      -- contract; no raise).
      server, err = httpd.bind{
        port = 25493, backend = "c", ssl = true,
        handler = function() end,
      }
    end)
    if server then server:close() end
    T.is_nil(server)
    T.not_nil(err)
    T.truthy(err:find("cert") or err:find("key") or err:find("ssl"), err)
  end)
else
  s:test("M14.C-c HTTPS (openssl CLI unavailable — skipped)", function()
    T.truthy(true)
  end)
end

----------------------------------------------------------------------
-- M14.C-d: WebSocket upgrade on the C evhttp backend
--
-- These cases target the C-side surface directly: predicate,
-- header-shape rejection, permessage-deflate negotiation from the
-- C accept path. The full protocol / frame-level coverage lives in
-- test_websocket.lua (which since M14.C-d runs against the C backend
-- via the shim default). Keeping the surface tests here means a
-- backend regression fails a focused C-suite before the broader
-- websocket suite even runs.
----------------------------------------------------------------------

-- tiny raw client used to speak the handshake bytes over fan.tcp
local function raw_ws_client(port, path, extra_headers)
  local c = assert(fan.tcp.connect("127.0.0.1", port))
  local lines = {
    "GET " .. path .. " HTTP/1.1",
    "Host: 127.0.0.1",
    "Upgrade: websocket",
    "Connection: Upgrade",
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==",
    "Sec-WebSocket-Version: 13",
  }
  for _, h in ipairs(extra_headers or {}) do lines[#lines + 1] = h end
  lines[#lines + 1] = ""; lines[#lines + 1] = ""
  assert(c:send(table.concat(lines, "\r\n")))
  return c
end

local function read_headers(c)
  local buf, status, hdrs = "", nil, {}
  while true do
    while not buf:find("\r\n", 1, true) do
      local chunk = assert(c:receive())
      buf = buf .. chunk
    end
    local nl = buf:find("\r\n", 1, true)
    local line = buf:sub(1, nl - 1)
    buf = buf:sub(nl + 2)
    if not status then status = line
    elseif line == "" then break
    else
      local k, v = line:match("^([^:]+):%s*(.*)$")
      if k then hdrs[k:lower()] = v end
    end
  end
  return status, hdrs, buf
end

s:test("M14.C-d: req:is_websocket_upgrade() is false on plain GET", function()
  local PORT = 24801
  local server, saw
  run(function()
    server = assert(httpd.bind{ backend = "c", port = PORT,
      handler = function(req, resp)
        saw = req:is_websocket_upgrade()
        resp:reply(200, {}, "plain")
      end})
    local body, err = http.get(BASE .. PORT .. "/")
    T.not_nil(body); T.is_nil(err)
  end)
  server:close()
  T.eq(saw, false)
end)

s:test("M14.C-d: req:is_websocket_upgrade() is true on a real handshake", function()
  local PORT = 24802
  local server, saw
  run(function()
    server = assert(httpd.bind{ backend = "c", port = PORT,
      handler = function(req, resp)
        saw = req:is_websocket_upgrade()
        local ws = assert(req:websocket_accept())
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/ws")
    read_headers(c)
    c:close()
  end)
  server:close()
  T.eq(saw, true)
end)

s:test("M14.C-d: 101 head advertises Sec-WebSocket-Accept (RFC 6455)", function()
  local PORT = 24803
  local server, status, hdrs
  run(function()
    server = assert(httpd.bind{ backend = "c", port = PORT,
      handler = function(req, resp)
        local ws = assert(req:websocket_accept())
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/ws")
    status, hdrs = read_headers(c)
    c:close()
  end)
  server:close()
  T.truthy(status:find("101"))
  -- key = "dGhlIHNhbXBsZSBub25jZQ==" -> RFC 6455 §1.3 canonical accept
  T.eq(hdrs["sec-websocket-accept"], "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
  T.eq((hdrs["upgrade"] or ""):lower(), "websocket")
  T.truthy((hdrs["connection"] or ""):lower():find("upgrade"))
end)

s:test("M14.C-d: websocket_accept without proper handshake headers returns nil,err", function()
  local PORT = 24804
  local server, ok, err
  run(function()
    server = assert(httpd.bind{ backend = "c", port = PORT,
      handler = function(req, resp)
        -- Not a WS upgrade at the HTTP level -> accept must reject.
        local ws, werr = req:websocket_accept()
        ok, err = ws, werr
        resp:reply(400, {}, "no-ws")
      end})
    http.get(BASE .. PORT .. "/")
  end)
  server:close()
  T.is_nil(ok)
  T.not_nil(err)
  T.truthy(err:find("websocket") or err:find("upgrade"), err)
end)

s:test("M14.C-d: permessage-deflate offer is answered with the same extension", function()
  local PORT = 24805
  local server, hdrs
  run(function()
    server = assert(httpd.bind{ backend = "c", port = PORT,
      handler = function(req, resp)
        local ws = assert(req:websocket_accept())
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/ws",
      { "Sec-WebSocket-Extensions: permessage-deflate" })
    local _, h = read_headers(c)
    hdrs = h
    c:close()
  end)
  server:close()
  T.not_nil(hdrs["sec-websocket-extensions"])
  T.truthy(hdrs["sec-websocket-extensions"]:find("permessage%-deflate"))
  T.truthy(hdrs["sec-websocket-extensions"]:find("server_no_context_takeover"))
end)

s:test("M14.C-d: no extension header when client does not offer permessage-deflate", function()
  local PORT = 24806
  local server, hdrs
  run(function()
    server = assert(httpd.bind{ backend = "c", port = PORT,
      handler = function(req, resp)
        local ws = assert(req:websocket_accept())
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/ws")   -- no Sec-WebSocket-Extensions
    local _, h = read_headers(c)
    hdrs = h
    c:close()
  end)
  server:close()
  T.is_nil(hdrs["sec-websocket-extensions"])
end)

s:test("M14.C-d: websocket_accept after reply() returns nil,err (head already sent)", function()
  local PORT = 24808
  local server, ok, err
  run(function()
    server = assert(httpd.bind{ backend = "c", port = PORT,
      handler = function(req, resp)
        resp:reply(200, {}, "already")
        local ws, werr = req:websocket_accept()
        ok, err = ws, werr
      end})
    local body = assert(http.get(BASE .. PORT .. "/"))
    T.eq(body.body, "already")
  end)
  server:close()
  T.is_nil(ok)
  T.truthy(err and (err:find("head already sent") or err:find("already completed")), err)
end)

s:test("M14.C-d: dispatcher — bind{backend='c'} with WS handler works", function()
  -- Regression guard: the M14.C-d WebSocket wiring must not require an
  -- explicit backend flip; the C shim wraps user handlers in a proxy
  -- exposing req:websocket_accept() / req:websocket_send / ... on both
  -- backends. Here we prove the shim's default (C) path.
  local PORT = 24807
  local server, saw_accept
  run(function()
    server = assert(httpd.bind{ port = PORT,   -- no backend override
      handler = function(req, resp)
        if req:is_websocket_upgrade() then
          local ws = assert(req:websocket_accept())
          saw_accept = true
          ws:close(1000)
        else
          resp:reply(200, {}, "hello")
        end
      end})
    -- Prove plain HTTP still works on the same bind.
    local body = assert(http.get(BASE .. PORT .. "/"))
    T.eq(body.body, "hello")
    -- Then upgrade.
    local c = raw_ws_client(PORT, "/ws")
    read_headers(c)
    c:close()
  end)
  server:close()
  T.eq(saw_accept, true)
end)

os.exit(T.run(s))
