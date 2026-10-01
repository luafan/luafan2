--[[
  test_http_c.lua — M13.C-a HTTP client contract tests for the libcurl-based
  C backend of fan.http.

  These tests exclusively drive the "c" backend (opts.backend="c") so we
  actually exercise src/net/http.c. Origin servers are stood up with the
  pure-Lua httpd backend (fan.httpd_lua) because it's fully-featured and
  its own test suite exercises it; this file's subject-under-test is the
  HTTP *client*.

  M13.C-a scope:
    - request(): GET / POST / PUT / DELETE / HEAD / PATCH / UPDATE (custom)
    - query table merged into URL
    - custom request headers reach the origin
    - response: status, reason, headers (lowercased), body
    - chunked transfer-encoding response body assembled correctly
    - non-2xx (404) still returns a response (not an error)
    - error path: connection refused -> nil, err
    - escape / unescape byte-for-byte parity with the Lua-backend variant
    - multiple concurrent clients don't cross-contaminate

  Redirect handling, cookiejar/cainfo/capath deep verification, and
  HTTPS with real cert verification come in later M13.C-* milestones.
]]

local T = require("test_framework")
local fan = require("fan")

-- Explicitly clear any global default a sibling test file might have set
-- so the C backend picker isn't shadowed. Each request in this file also
-- passes opts.backend = "c" to be safe.
_G.__FAN_HTTP_BACKEND_DEFAULT = nil

local http = require("fan.http")
local httpd_lua = require("fan.httpd_lua")   -- origin server

-- Sanity: the C backend must be compiled in for this test file to be
-- meaningful. If it isn't, fail hard with a clear message.
assert(fan.http_c ~= nil and fan.http_c.available ~= false,
  "fan.http_c not available (build with -DFAN_WITH_CURL=ON)")

local s = T.suite("fan.http C backend (M13.C-a)")

-- Runs `body` inside a fan coroutine; captures errors, breaks the loop
-- when body returns, and re-raises after event_base_dispatch exits.
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

-- Each test picks its own port to sidestep TIME_WAIT collisions when
-- tests run back-to-back under coverage instrumentation.

s:test("GET: status/reason/headers/body via C backend", function()
  local PORT = 25501
  local server, resp
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply(200, { ["Content-Type"] = "text/plain",
                       ["X-Server"] = "test-origin" }, "hello world")
      end,
    })
    resp = http.request{
      backend = "c",
      url = BASE .. PORT .. "/hi",
      method = "GET",
    }
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.reason, "OK")
  T.eq(resp.headers["content-type"], "text/plain")
  T.eq(resp.headers["x-server"], "test-origin")
  T.eq(resp.body, "hello world")
  -- M16.4: C backend also exposes v1-compatible `responseCode` alias
  T.eq(resp.responseCode, 200)
  T.eq(resp.responseCode, resp.status)
end)

s:test("GET: query table merged into URL", function()
  local PORT = 25502
  local server, resp, seen_target
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        -- req.target is the raw request line target incl. query; req.query
        -- is the v1-compatible raw query string and req.params is the map.
        seen_target = req.target
        r:reply(200, { ["Content-Type"] = "text/plain" },
          "a=" .. tostring(req.params.a) .. ";b=" .. tostring(req.params.b))
      end,
    })
    resp = http.request{
      backend = "c",
      url = BASE .. PORT .. "/q",
      query = { a = 1, b = "two" },
    }
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  -- Both orderings possible; either is fine so long as both keys are there.
  T.truthy(seen_target == "/q?a=1&b=two" or seen_target == "/q?b=two&a=1",
    "unexpected request target: " .. tostring(seen_target))
  T.truthy(resp.body:find("a=1", 1, true))
  T.truthy(resp.body:find("b=two", 1, true))
end)

s:test("GET: custom request header reaches origin", function()
  local PORT = 25503
  local server, resp
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply(200, { ["Content-Type"] = "text/plain" },
          "xfoo=" .. tostring(req.headers["x-foo"]))
      end,
    })
    resp = http.request{
      backend = "c",
      url = BASE .. PORT .. "/h",
      headers = { ["X-Foo"] = "bar-baz" },
    }
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "xfoo=bar-baz")
end)

s:test("POST: sends body, origin echoes it", function()
  local PORT = 25504
  local server, resp
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply(200, { ["Content-Type"] = "text/plain" },
          "M=" .. req.method .. " B=" .. (req.body or ""))
      end,
    })
    resp = http.request{
      backend = "c",
      url = BASE .. PORT .. "/echo",
      method = "POST",
      headers = { ["Content-Type"] = "application/octet-stream" },
      body = "the payload",
    }
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "M=POST B=the payload")
end)

s:test("PUT/DELETE/PATCH via C backend", function()
  local PORT = 25505
  local server, r_put, r_del, r_patch
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply(200, { ["Content-Type"] = "text/plain" },
          "M=" .. req.method .. ";B=" .. (req.body or ""))
      end,
    })
    r_put   = http.put(BASE .. PORT .. "/x",
      { backend = "c", body = "putbody" })
    r_del   = http.delete(BASE .. PORT .. "/x", { backend = "c" })
    r_patch = http.patch(BASE .. PORT .. "/x",
      { backend = "c", body = "patchbody" })
  end)
  if server then server:close() end
  T.not_nil(r_put);   T.eq(r_put.status, 200);   T.eq(r_put.body,   "M=PUT;B=putbody")
  T.not_nil(r_del);   T.eq(r_del.status, 200);   T.eq(r_del.body,   "M=DELETE;B=")
  T.not_nil(r_patch); T.eq(r_patch.status, 200); T.eq(r_patch.body, "M=PATCH;B=patchbody")
end)

s:test("HEAD: response has no body but status+headers", function()
  local PORT = 25506
  local server, resp
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        -- reply(...) with a body; for HEAD the origin still writes
        -- Content-Length but the caller must not see the body.
        r:reply(200, { ["Content-Type"] = "text/plain" }, "should-not-see")
      end,
    })
    resp = http.head(BASE .. PORT .. "/h", { backend = "c" })
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "")
  T.eq(resp.headers["content-type"], "text/plain")
end)

s:test("404 non-2xx still returns a response (not an error)", function()
  local PORT = 25507
  local server, resp
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply(404, { ["Content-Type"] = "text/plain" }, "nope")
      end,
    })
    resp = http.request{
      backend = "c",
      url = BASE .. PORT .. "/missing",
    }
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 404)
  T.eq(resp.body, "nope")
end)

s:test("Duplicate response headers folded with ', '", function()
  local PORT = 25508
  local server, resp
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        -- addheader appends a second Set-Cookie: the C backend's header
        -- callback must fold duplicates into a single lowercased entry.
        r:addheader("Set-Cookie", "a=1")
        r:addheader("Set-Cookie", "b=2")
        r:reply(200, { ["Content-Type"] = "text/plain" }, "ok")
      end,
    })
    resp = http.request{
      backend = "c",
      url = BASE .. PORT .. "/cookies",
    }
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  -- Both cookies present, in original order, separated by ", ".
  local sc = resp.headers["set-cookie"]
  T.not_nil(sc)
  T.truthy(sc:find("a=1", 1, true), "missing a=1 in: " .. tostring(sc))
  T.truthy(sc:find("b=2", 1, true), "missing b=2 in: " .. tostring(sc))
  T.truthy(sc:find("a=1, b=2", 1, true) or sc:find("a=1,b=2", 1, true),
    "expected fold with ', ': " .. tostring(sc))
end)

s:test("Chunked transfer-encoding response is assembled", function()
  local PORT = 25509
  local server, resp
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply_start(200, { ["Content-Type"] = "text/plain" })
        r:reply_chunk("part-1;")
        r:reply_chunk("part-2;")
        r:reply_chunk("end")
        r:reply_end()
      end,
    })
    resp = http.request{
      backend = "c",
      url = BASE .. PORT .. "/stream",
    }
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "part-1;part-2;end")
end)

s:test("Connection refused returns nil, err", function()
  -- Port 1 is essentially guaranteed to be closed for an unprivileged
  -- process on Linux; the client should surface a curl-shaped error.
  local resp, err
  run(function()
    resp, err = http.request{
      backend = "c",
      url = "http://127.0.0.1:1/",
      timeout = 2,
    }
  end)
  T.is_nil(resp)
  T.is_type(err, "string")
  T.truthy(#err > 0, "empty error message")
end)

s:test("escape/unescape byte-parity with Lua backend", function()
  local http_lua = require("fan.http_lua")
  -- Cover: unreserved passthrough, reserved encoding, high bytes, space,
  -- ASCII edge cases.
  local samples = {
    "",
    "abcXYZ0189-._~",
    "hello world!",
    "a b+c/d?e=f&g#h",
    "\x00\x01\x7f\x80\xff",
    "line1\nline2\r\ntab\there",
    "%25 is percent",
  }
  for i, sample in ipairs(samples) do
    local c_enc   = fan.http_c.escape(sample)
    local lua_enc = http_lua.escape(sample)
    T.eq(c_enc, lua_enc,
      string.format("escape mismatch sample #%d %q: c=%q lua=%q",
        i, sample, tostring(c_enc), tostring(lua_enc)))
    -- Round-trip: unescape(escape(x)) == x on both.
    T.eq(fan.http_c.unescape(c_enc), sample,
      "c unescape round-trip failed for sample #" .. i)
    T.eq(http_lua.unescape(lua_enc), sample,
      "lua unescape round-trip failed for sample #" .. i)
  end
  -- Cross-decode: C-encoded string decodes cleanly through the Lua
  -- unescape and vice-versa (byte-for-byte identical output).
  local mixed = "a b+c/d?e=f&g#h"
  T.eq(http_lua.unescape(fan.http_c.escape(mixed)), mixed)
  T.eq(fan.http_c.unescape(http_lua.escape(mixed)), mixed)
end)

s:test("Multiple concurrent C-backend requests are isolated", function()
  local PORT = 25510
  local N = 5
  local server
  local resps = {}
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        -- Slot from decoded params; req.query remains the raw string.
        local slot = req.params.slot or "?"
        r:reply(200, { ["Content-Type"] = "text/plain" },
          "slot=" .. slot)
      end,
    })
    -- Kick off N concurrent client coroutines. Each parks on its own
    -- req userdata; the shared multi handle must not cross-wire them.
    local pending = N
    for i = 1, N do
      fan.spawn(function()
        local r, err = http.request{
          backend = "c",
          url = BASE .. PORT .. "/slot?slot=" .. i,
        }
        resps[i] = r or ("ERR:" .. tostring(err))
        pending = pending - 1
      end)
    end
    -- Yield until all N complete. fan.sleep(0) is a simple yield helper
    -- that returns to the scheduler.
    while pending > 0 do fan.sleep(0.01) end
  end)
  if server then server:close() end
  for i = 1, N do
    T.is_type(resps[i], "table",
      "slot " .. i .. " did not get a response: " .. tostring(resps[i]))
    T.eq(resps[i].status, 200)
    T.eq(resps[i].body, "slot=" .. i)
  end
end)

s:test("verb helpers respect explicit backend=c", function()
  local PORT = 25511
  local server, r_get, r_post
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply(200, { ["Content-Type"] = "text/plain" },
          req.method .. ":" .. req.path)
      end,
    })
    r_get  = http.get(BASE .. PORT .. "/g", { backend = "c" })
    r_post = http.post(BASE .. PORT .. "/p", { backend = "c", body = "x" })
  end)
  if server then server:close() end
  T.not_nil(r_get);  T.eq(r_get.status, 200);  T.eq(r_get.body,  "GET:/g")
  T.not_nil(r_post); T.eq(r_post.status, 200); T.eq(r_post.body, "POST:/p")
end)

-- ---------------------------------------------------------------------------
-- M20.2: streaming callbacks on the C/curl backend.
--
-- Origin uses fan.httpd_lua chunked replies so libcurl actually decodes
-- transfer-encoding and delivers each chunk to WRITEFUNCTION. The tests
-- assert:
--   * onheader fires exactly once with a v1-shaped {status,responseCode,
--     reason,headers} table containing the lower-cased headers, BEFORE
--     onreceive.
--   * onreceive fires at least once per chunk and never with header
--     bytes (only decoded body).
--   * response.body is still fully aggregated by default (buffered=true).
--   * buffered=false skips aggregation while still delivering callbacks.
--   * onreceive returning false / raising / onheader returning false /
--     raising all abort with an explicit, dedicated error message; the
--     transfer fails cleanly with response.body absent.
--   * Non-function onreceive/onheader values are rejected up-front.
-- ---------------------------------------------------------------------------

s:test("M20.2: C onheader fires once, onreceive streams while buffering", function()
  local PORT = 25520
  local server, resp, headers_seen, header_info, chunk_count, agg
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply_start(200, { ["Content-Type"] = "text/plain",
                             ["X-Streaming"] = "yes" })
        r:reply_chunk("alpha-")
        r:reply_chunk("beta-")
        r:reply_chunk("gamma")
        r:reply_end()
      end,
    })
    headers_seen = 0
    chunk_count  = 0
    agg = {}
    resp = http.request{
      backend  = "c",
      url      = BASE .. PORT .. "/stream",
      onheader = function(info)
        headers_seen = headers_seen + 1
        header_info  = info
      end,
      onreceive = function(chunk)
        chunk_count = chunk_count + 1
        agg[#agg + 1] = chunk
      end,
    }
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  -- onheader
  T.eq(headers_seen, 1, "onheader fired more than once")
  T.eq(header_info.status, 200)
  T.eq(header_info.responseCode, 200)
  T.eq(header_info.reason, "OK")
  T.eq(header_info.headers["content-type"], "text/plain")
  T.eq(header_info.headers["x-streaming"], "yes")
  -- onreceive: at least one callback, and the concatenation matches body.
  T.truthy(chunk_count >= 1, "onreceive never fired")
  T.eq(table.concat(agg), "alpha-beta-gamma",
    "onreceive aggregate mismatch")
  -- Buffered by default: response.body is still the full body.
  T.eq(resp.body, "alpha-beta-gamma")
end)

s:test("M20.2: buffered=false skips response.body accumulation", function()
  local PORT = 25521
  local server, resp, chunks
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply_start(200, { ["Content-Type"] = "text/plain" })
        r:reply_chunk("large-")
        r:reply_chunk("payload")
        r:reply_end()
      end,
    })
    chunks = {}
    resp = http.request{
      backend   = "c",
      url       = BASE .. PORT .. "/",
      buffered  = false,
      onreceive = function(chunk) chunks[#chunks + 1] = chunk end,
    }
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "", "buffered=false must leave response.body empty")
  T.eq(table.concat(chunks), "large-payload",
    "streaming aggregate mismatch")
end)

s:test("M20.2: onreceive returning false aborts with explicit error", function()
  local PORT = 25522
  local server, resp, err, calls
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply_start(200, { ["Content-Type"] = "text/plain" })
        r:reply_chunk("first")
        r:reply_chunk("second")
        r:reply_end()
      end,
    })
    calls = 0
    resp, err = http.request{
      backend   = "c",
      url       = BASE .. PORT .. "/",
      onreceive = function()
        calls = calls + 1
        return false
      end,
    }
  end)
  if server then server:close() end
  T.is_nil(resp)
  T.is_type(err, "string")
  T.eq(calls, 1, "callback fired past cancellation")
  T.truthy(err:find("onreceive callback canceled", 1, true),
    "expected 'onreceive callback canceled', got: " .. tostring(err))
end)

s:test("M20.2: onreceive exception surfaces as callback error", function()
  local PORT = 25523
  local server, resp, err
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply(200, { ["Content-Type"] = "text/plain" }, "some-body")
      end,
    })
    resp, err = http.request{
      backend   = "c",
      url       = BASE .. PORT .. "/",
      onreceive = function() error("boom-c-stream") end,
    }
  end)
  if server then server:close() end
  T.is_nil(resp)
  T.is_type(err, "string")
  T.truthy(err:find("onreceive callback error", 1, true), err)
  T.truthy(err:find("boom-c-stream", 1, true), err)
end)

s:test("M20.2: onheader returning false aborts the request", function()
  local PORT = 25524
  local server, resp, err, chunk_called
  run(function()
    server = assert(httpd_lua.bind{
      port = PORT,
      onService = function(req, r)
        r:reply(200, { ["Content-Type"] = "text/plain" }, "body")
      end,
    })
    chunk_called = false
    resp, err = http.request{
      backend   = "c",
      url       = BASE .. PORT .. "/",
      onheader  = function() return false end,
      onreceive = function() chunk_called = true end,
    }
  end)
  if server then server:close() end
  T.is_nil(resp)
  T.is_type(err, "string")
  T.truthy(err:find("onheader callback canceled", 1, true),
    "expected 'onheader callback canceled', got: " .. tostring(err))
  T.falsy(chunk_called, "onreceive must not fire after onheader cancels")
end)

s:test("M20.2: non-function callback types are rejected up-front", function()
  local ok_receive = pcall(function()
    http.request{ backend = "c", url = BASE .. "1/", onreceive = 42 }
  end)
  T.falsy(ok_receive, "non-function onreceive must luaL_error")
  local ok_header = pcall(function()
    http.request{ backend = "c", url = BASE .. "1/", onheader = "not-fn" }
  end)
  T.falsy(ok_header, "non-function onheader must luaL_error")
end)

s:test("pick_backend selection rules", function()
  -- backend override wins
  T.eq(http._pick_backend{ backend = "lua" }, "lua")
  T.eq(http._pick_backend{ backend = "c"   }, "c")
  -- global default overrides auto-pick
  _G.__FAN_HTTP_BACKEND_DEFAULT = "lua"
  T.eq(http._pick_backend{}, "lua")
  _G.__FAN_HTTP_BACKEND_DEFAULT = nil
  -- With C backend available (asserted at file top), default is "c".
  T.eq(http._pick_backend{}, "c")
end)

os.exit(T.run(s))
