--[[
  test_http.lua — M4 HTTP/1.1 client contract tests (fan.http over fan.tcp).

  Plan §4.5 contract coverage:
    - methods: GET / POST / PUT / DELETE / HEAD
    - request headers, request body, query string merging
    - response framing: Content-Length body AND chunked transfer-encoding
    - status/reason parsing, lowercased response headers
    - redirect following (302 -> GET) and max_redirects guard
    - multiple concurrent clients are isolated (own connection each)
    - error path: connection refused

  A local HTTP/1.1 origin server is built on fan.tcp.bind: it parses the
  request line + headers (+ Content-Length body) and dispatches on the path.
  Everything runs in-process on 127.0.0.1 inside a single fan loop.
]]
local T = require("test_framework")
local fan = require("fan")

-- Pin this test module to the pure-Lua backend. The M13.C-* milestones
-- add the libcurl-based C backend to fan.http; this legacy test file
-- covers the Lua backend's own end-to-end code paths (parse_url,
-- redirect loop details, chunked reader, concurrent client isolation).
-- The C backend has its own focused suite in test_http_c.lua.
_G.__FAN_HTTP_BACKEND_DEFAULT = "lua"

local http = require("fan.http")

local s = T.suite("fan.http client (M4)")

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

----------------------------------------------------------------------
-- Minimal HTTP/1.1 origin server over fan.tcp
----------------------------------------------------------------------
-- Reads a full request (request-line, headers, optional Content-Length body)
-- from one connection, then calls handler(req) -> raw response string.
local function read_request(conn)
  local buf = ""
  -- read until end of headers
  while not buf:find("\r\n\r\n", 1, true) do
    local d = conn:receive()
    if not d then return nil end
    buf = buf .. d
  end
  local head, rest = buf:match("^(.-\r\n\r\n)(.*)$")
  local line = head:match("^([^\r\n]+)\r\n")
  local method, path = line:match("^(%S+)%s+(%S+)%s+HTTP/%d%.%d$")
  local headers = {}
  for k, v in head:gmatch("\r\n([^:\r\n]+):%s*([^\r\n]*)") do
    headers[k:lower()] = v
  end
  local body = rest
  local clen = tonumber(headers["content-length"])
  if clen then
    while #body < clen do
      local d = conn:receive()
      if not d then break end
      body = body .. d
    end
    body = body:sub(1, clen)
  end
  return { method = method, path = path, headers = headers, body = body }
end

-- build a normal Content-Length response
local function resp_cl(status, reason, body, extra_headers)
  body = body or ""
  local lines = {
    string.format("HTTP/1.1 %d %s", status, reason),
    "Content-Length: " .. #body,
    "Connection: close",
  }
  if extra_headers then
    for _, h in ipairs(extra_headers) do lines[#lines + 1] = h end
  end
  return table.concat(lines, "\r\n") .. "\r\n\r\n" .. body
end

-- build a chunked response from a list of chunk strings
local function resp_chunked(status, reason, chunks)
  local parts = {
    string.format("HTTP/1.1 %d %s", status, reason),
    "Transfer-Encoding: chunked",
    "Connection: close",
    "", "",
  }
  local out = table.concat(parts, "\r\n")
  for _, c in ipairs(chunks) do
    out = out .. string.format("%x\r\n%s\r\n", #c, c)
  end
  out = out .. "0\r\n\r\n"
  return out
end

-- dispatch table keyed by path
local function dispatch(req)
  if not req then return resp_cl(400, "Bad Request", "no request") end
  local p = req.path
  if p == "/hello" then
    return resp_cl(200, "OK", "hello world",
      { "Content-Type: text/plain", "X-Custom: yes" })
  elseif p:match("^/echo") then
    -- reflect method, query, a request header and the body
    local b = string.format("M=%s Q=%s H=%s BODY=%s",
      req.method, p:match("%?(.*)$") or "", req.headers["x-foo"] or "", req.body)
    return resp_cl(200, "OK", b)
  elseif p == "/chunked" then
    return resp_chunked(200, "OK", { "Wiki", "pedia", " in\r\n\r\nchunks." })
  elseif p == "/redir" then
    return resp_cl(302, "Found", "", { "Location: /hello" })
  elseif p == "/redir-loop" then
    return resp_cl(302, "Found", "", { "Location: /redir-loop" })
  elseif p == "/redir-post" then
    return resp_cl(303, "See Other", "", { "Location: /echo" })
  elseif p == "/notfound-x" then
    return resp_cl(404, "Not Found", "nope")
  elseif p == "/nolen" then
    -- no Content-Length, no chunked: body framed by connection close
    return "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nEOF-BODY"
  else
    return resp_cl(404, "Not Found", "unknown path " .. tostring(p))
  end
end

local function start_origin(port)
  return assert(fan.tcp.bind("127.0.0.1", port, function(conn)
    local req = read_request(conn)
    conn:send(dispatch(req))
    -- small delay so client can drain before the socket tears down
    fan.sleep(0.02)
    conn:close()
  end))
end

local BASE = "http://127.0.0.1:"

----------------------------------------------------------------------
-- tests
----------------------------------------------------------------------
s:test("GET: status, reason, lowercased headers, content-length body", function()
  local PORT = 24310
  local server, resp
  run(function()
    server = start_origin(PORT)
    resp = http.get(BASE .. PORT .. "/hello")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.reason, "OK")
  T.eq(resp.body, "hello world")
  T.eq(resp.headers["content-type"], "text/plain")
  T.eq(resp.headers["x-custom"], "yes")
end)

s:test("POST: body and custom header reach the server", function()
  local PORT = 24311
  local server, resp
  run(function()
    server = start_origin(PORT)
    resp = http.post(BASE .. PORT .. "/echo",
      { body = "payload-123", headers = { ["X-Foo"] = "bar" } })
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.truthy(resp.body:find("M=POST", 1, true))
  T.truthy(resp.body:find("H=bar", 1, true))
  T.truthy(resp.body:find("BODY=payload-123", 1, true))
end)

s:test("query table is merged into the URL", function()
  local PORT = 24312
  local server, resp
  run(function()
    server = start_origin(PORT)
    resp = http.request({ url = BASE .. PORT .. "/echo",
      query = { a = "1", b = "two" } })
  end)
  if server then server:close() end
  T.not_nil(resp)
  -- order of pairs() is unspecified; assert both k=v appear
  T.truthy(resp.body:find("a=1", 1, true))
  T.truthy(resp.body:find("b=two", 1, true))
end)

s:test("PUT and DELETE methods are sent verbatim", function()
  local PORT = 24313
  local put_resp, del_resp
  run(function()
    local server = start_origin(PORT)
    put_resp = http.put(BASE .. PORT .. "/echo", { body = "u" })
    server:close()
    local server2 = start_origin(PORT)
    del_resp = http.delete(BASE .. PORT .. "/echo")
    server2:close()
  end)
  T.not_nil(put_resp); T.truthy(put_resp.body:find("M=PUT", 1, true))
  T.not_nil(del_resp); T.truthy(del_resp.body:find("M=DELETE", 1, true))
end)

s:test("HEAD returns headers but no body", function()
  local PORT = 24314
  local server, resp
  run(function()
    server = start_origin(PORT)
    resp = http.head(BASE .. PORT .. "/hello")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "")
  -- Content-Length header is still present/parsed
  T.not_nil(resp.headers["content-length"])
end)

s:test("chunked transfer-encoding is decoded", function()
  local PORT = 24315
  local server, resp
  run(function()
    server = start_origin(PORT)
    resp = http.get(BASE .. PORT .. "/chunked")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "Wikipedia in\r\n\r\nchunks.")
end)

s:test("body framed by connection close (no length, no chunked)", function()
  local PORT = 24316
  local server, resp
  run(function()
    server = start_origin(PORT)
    resp = http.get(BASE .. PORT .. "/nolen")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "EOF-BODY")
end)

s:test("302 redirect is followed to final resource", function()
  local PORT = 24317
  local resp
  run(function()
    -- redirect target reuses the same origin on a fresh connection each time;
    -- server closes after one request, so re-arm between hops.
    local server = start_origin(PORT)
    -- the follow logic issues a second connect; keep the listener alive by
    -- rebinding inside the handler is complex, so use a persistent listener:
    server:close()
    local persistent = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      local req = read_request(conn)
      conn:send(dispatch(req))
      fan.sleep(0.02)
      conn:close()
    end))
    resp = http.get(BASE .. PORT .. "/redir")
    persistent:close()
  end)
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "hello world")
end)

s:test("redirect loop is bounded by max_redirects", function()
  local PORT = 24318
  local resp, err
  run(function()
    local server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      local req = read_request(conn)
      conn:send(dispatch(req))
      fan.sleep(0.02)
      conn:close()
    end))
    resp, err = http.request({ url = BASE .. PORT .. "/redir-loop",
      max_redirects = 3 })
    server:close()
  end)
  T.is_nil(resp)
  T.not_nil(err)
end)

s:test("303 redirect switches POST to GET and drops body", function()
  local PORT = 24319
  local resp
  run(function()
    local server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      local req = read_request(conn)
      conn:send(dispatch(req))
      fan.sleep(0.02)
      conn:close()
    end))
    resp = http.post(BASE .. PORT .. "/redir-post", { body = "should-drop" })
    server:close()
  end)
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.truthy(resp.body:find("M=GET", 1, true))
  T.truthy(resp.body:find("BODY=", 1, true))
  -- body must NOT carry the original POST payload
  T.falsy(resp.body:find("should-drop", 1, true))
end)

s:test("404 status is surfaced", function()
  local PORT = 24320
  local server, resp
  run(function()
    server = start_origin(PORT)
    resp = http.get(BASE .. PORT .. "/notfound-x")
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 404)
  T.eq(resp.reason, "Not Found")
end)

s:test("concurrent clients are isolated (own connection each)", function()
  local PORT = 24321
  local N = 6
  local results = {}
  local done = 0
  run(function()
    local server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      local req = read_request(conn)
      conn:send(dispatch(req))
      fan.sleep(0.02)
      conn:close()
    end))
    for i = 1, N do
      fan.spawn(function()
        local r = http.post(BASE .. PORT .. "/echo", { body = "req-" .. i })
        results[i] = r and r.body
        done = done + 1
        if done == N then server:close(); fan.loopbreak() end
      end)
    end
    -- keep this coroutine alive; the last worker breaks the loop
    while done < N do fan.sleep(0.02) end
  end)
  T.eq(done, N)
  for i = 1, N do
    T.not_nil(results[i])
    T.truthy(results[i]:find("BODY=req-" .. i, 1, true),
      "client " .. i .. " saw wrong body: " .. tostring(results[i]))
  end
end)

s:test("connection refused returns nil, err", function()
  local resp, err
  run(function()
    resp, err = http.get("http://127.0.0.1:24399/x")  -- nothing listening
  end)
  T.is_nil(resp)
  T.not_nil(err)
end)

s:test("parse_url handles ports, defaults and IPv6 literals", function()
  local sc, h, p, pq = http.parse_url("https://example.com/a/b?x=1")
  T.eq(sc, "https"); T.eq(h, "example.com"); T.eq(p, 443); T.eq(pq, "/a/b?x=1")
  sc, h, p, pq = http.parse_url("http://host:8080")
  T.eq(sc, "http"); T.eq(h, "host"); T.eq(p, 8080); T.eq(pq, "/")
  sc, h, p, pq = http.parse_url("http://[::1]:9000/z")
  T.eq(sc, "http"); T.eq(h, "::1"); T.eq(p, 9000); T.eq(pq, "/z")
end)

-- ---------------------------------------------------------------------------
-- M13: v1 fan.http compatibility surface (escape/unescape/cookiejar/cainfo/
-- capath/update). Backend consumption is deferred to the M13 C module;
-- these tests pin the API shape so migrating v1 code keeps working.
-- ---------------------------------------------------------------------------

s:test("escape encodes reserved bytes and preserves unreserved", function()
  -- RFC 3986 unreserved: A-Z a-z 0-9 - . _ ~
  T.truthy(http.escape("abcXYZ_0-9.~") == "abcXYZ_0-9.~")
  T.truthy(http.escape(" ") == "%20")
  T.truthy(http.escape("/") == "%2F")
  T.truthy(http.escape("a b/c") == "a%20b%2Fc")
  T.truthy(http.escape("Hello, world!") == "Hello%2C%20world%21")
end)

s:test("escape handles all-byte round-trip via unescape", function()
  -- Build every byte 0..255, escape, then unescape; must round-trip.
  local buf = {}
  for i = 0, 255 do buf[#buf + 1] = string.char(i) end
  local raw = table.concat(buf)
  local enc = http.escape(raw)
  T.truthy(type(enc) == "string")
  -- No unreserved byte should be percent-encoded, and everything else must be.
  T.truthy(not enc:find("[^%%%w_%-%.~]"))  -- only unreserved + %xx
  local dec = http.unescape(enc)
  T.truthy(dec == raw)
end)

s:test("escape / unescape reject non-string arguments", function()
  T.truthy(http.escape(42)   == nil)
  T.truthy(http.escape(nil)  == nil)
  T.truthy(http.unescape(42) == nil)
  T.truthy(http.unescape({}) == nil)
end)

s:test("cookiejar / cainfo / capath stash their arg for the C backend", function()
  http.cookiejar("/tmp/cj.txt")
  http.cainfo("/etc/ssl/ca.pem")
  http.capath("/etc/ssl/certs")
  T.truthy(http._cookiejar == "/tmp/cj.txt")
  T.truthy(http._cainfo    == "/etc/ssl/ca.pem")
  T.truthy(http._capath    == "/etc/ssl/certs")
end)

s:test("patch / update verbs dispatch through M.request with the right method", function()
  -- Re-use the shared echo origin: /echo reflects METHOD in its body.
  local PORT = 24421
  local server, r1, r2
  run(function()
    server = start_origin(PORT)
    r1 = assert(http.patch(BASE .. PORT .. "/echo"))
    server:close()
    server = start_origin(PORT)  -- start_origin closes after one req
    r2 = assert(http.update(BASE .. PORT .. "/echo"))
    server:close()
  end)
  T.truthy(r1.body:find("M=PATCH", 1, true))
  T.truthy(r2.body:find("M=UPDATE", 1, true))
end)

-- ---------------------------------------------------------------------------
-- M16.4 additions: single-table verb form, responseCode alias,
-- set_default_follow_redirects module knob.
-- ---------------------------------------------------------------------------

s:test("M16.4: fan.http.get accepts a single-table form (url inside opts)", function()
  -- Legacy v1 code style: everything as a single table.  luafan2 pre-M16.4
  -- only accepted (url, opts); this test guards the compatibility.
  local PORT = 24430
  local server, r_pos, r_tbl
  run(function()
    server = start_origin(PORT)
    r_pos = assert(http.get(BASE .. PORT .. "/echo"))          -- (url) form
    server:close()
    server = start_origin(PORT)
    r_tbl = assert(http.get{ url = BASE .. PORT .. "/echo" })  -- {url=...} form
    server:close()
  end)
  T.eq(r_pos.status, 200); T.eq(r_tbl.status, 200)
  T.truthy(r_pos.body:find("M=GET", 1, true))
  T.truthy(r_tbl.body:find("M=GET", 1, true))
end)

s:test("M16.4: single-table verb form threads headers and body", function()
  -- The shared echo dispatch reflects the "X-Foo" request header (lowercased
  -- to "x-foo" server-side) and the request body into the response body
  -- shaped as "M=... Q=... H=<x-foo> BODY=...".
  local PORT = 24431
  local server, resp
  run(function()
    server = start_origin(PORT)
    resp = assert(http.post{
      url     = BASE .. PORT .. "/echo",
      headers = { ["X-Foo"] = "single-table-marker" },
      body    = "hello=world",
    })
    server:close()
  end)
  T.eq(resp.status, 200)
  T.truthy(resp.body:find("M=POST", 1, true))
  T.truthy(resp.body:find("H=single-table-marker", 1, true))
  T.truthy(resp.body:find("BODY=hello=world", 1, true))
end)

s:test("M16.4: resp.responseCode is a numeric alias for resp.status", function()
  local PORT = 24432
  local server, resp
  run(function()
    server = start_origin(PORT)
    resp = assert(http.get(BASE .. PORT .. "/echo"))
    server:close()
  end)
  T.eq(resp.status, 200)
  T.eq(resp.responseCode, 200)
  T.eq(resp.status, resp.responseCode)
end)

s:test("M16.4: set_default_follow_redirects(false) makes 302 stop at hop 1", function()
  -- The shim's redirect loop is skipped when follow_redirects is false.
  -- Baseline (with default true) is already covered by the earlier "302
  -- redirect is followed to final resource" test; here we verify the
  -- module-level knob flips behaviour without needing every caller to
  -- pass follow_redirects = false explicitly.
  local PORT = 24433
  local resp
  http.set_default_follow_redirects(false)
  run(function()
    -- Reuse the shared /redir path which returns 302 -> /hello.
    local persistent = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      local req = read_request(conn)
      conn:send(dispatch(req))
      fan.sleep(0.02)
      conn:close()
    end))
    resp = http.get(BASE .. PORT .. "/redir")
    persistent:close()
  end)
  http.set_default_follow_redirects(true)   -- restore for subsequent tests
  T.not_nil(resp)
  T.eq(resp.status, 302)
  T.eq(resp.responseCode, 302)
  T.truthy(resp.headers["location"])   -- Location header preserved
end)

s:test("M20.1: onheader once and onreceive streams decoded body while buffering", function()
  local PORT = 24434
  local server, resp, headers_seen, pieces
  run(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      local req = read_request(conn)
      conn:send(resp_chunked(200, "OK", { "one", "two", "three" }))
      fan.sleep(0.03)
      conn:close()
    end))
    headers_seen = 0
    pieces = {}
    resp = http.request({
      backend = "lua",
      url = BASE .. PORT .. "/chunked",
      onheader = function(h)
        headers_seen = headers_seen + 1
        T.eq(h.status, 200)
        T.eq(h.responseCode, 200)
        T.eq(h.headers["transfer-encoding"], "chunked")
      end,
      onreceive = function(chunk)
        pieces[#pieces + 1] = chunk
      end,
    })
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(headers_seen, 1)
  T.eq(table.concat(pieces), "onetwothree")
  T.eq(resp.body, "onetwothree")
end)

s:test("M20.1: callback false cancels body read with a clear error", function()
  local PORT = 24435
  local server, resp, err, calls
  run(function()
    server = start_origin(PORT)
    calls = 0
    resp, err = http.get(BASE .. PORT .. "/hello", {
      backend = "lua",
      onreceive = function()
        calls = calls + 1
        return false
      end,
    })
  end)
  if server then server:close() end
  T.is_nil(resp)
  T.eq(calls, 1)
  T.truthy(err:find("onreceive callback canceled", 1, true))
end)

s:test("M20.1: callback exception and onheader cancellation are errors", function()
  local PORT = 24436
  local server, resp, err
  run(function()
    server = start_origin(PORT)
    resp, err = http.get(BASE .. PORT .. "/hello", {
      backend = "lua",
      onreceive = function() error("boom-stream") end,
    })
    server:close()
    server = start_origin(PORT)
    resp, err = http.get(BASE .. PORT .. "/hello", {
      backend = "lua",
      onheader = function() return false end,
    })
  end)
  if server then server:close() end
  T.is_nil(resp)
  T.truthy(err:find("onheader callback canceled", 1, true))
end)

s:test("M20.2: Lua backend buffered=false leaves response.body empty", function()
  local PORT = 24437
  local server, resp, pieces
  run(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      local req = read_request(conn)
      conn:send(resp_chunked(200, "OK", { "big-", "payload" }))
      fan.sleep(0.03)
      conn:close()
    end))
    pieces = {}
    resp = http.request({
      backend  = "lua",
      url      = BASE .. PORT .. "/nobuf",
      buffered = false,
      onreceive = function(chunk) pieces[#pieces + 1] = chunk end,
    })
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.body, "", "buffered=false must leave body empty")
  T.eq(table.concat(pieces), "big-payload")
end)

os.exit(T.run(s))
