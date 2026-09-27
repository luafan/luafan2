--[[
  test_httpd_lua.lua — pin fan.httpd to the pure-Lua backend and exercise
  the paths that used to be hit by test_httpd.lua before M14.C-b flipped
  the default to the C evhttp backend.

  Rationale: test_httpd.lua now runs against whichever backend the shim
  picks (C by default), which means the Lua backend's chunked-reply,
  addheader, and error-path code in lua/fan/httpd_lua.lua would otherwise
  only be reached by HTTPS tests (test_httpsd.lua, which still forces
  the Lua backend until M14.C-c) — and HTTPS tests are focused on the
  TLS layer, not on the chunked/addheader/error branches. This file
  keeps the Lua backend well-covered so the coverage floor is preserved
  as long as httpd_lua.lua exists.
]]

-- Pin every httpd.bind call in this file to the pure-Lua backend.
_G.__FAN_HTTPD_BACKEND_DEFAULT = "lua"

local T = require("test_framework")
local fan = require("fan")
local http = require("fan.http")
local httpd = require("fan.httpd")

local s = T.suite("fan.httpd server (Lua backend re-pinned)")

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

s:test("Lua backend: pick_backend honours the global pin", function()
  T.eq(httpd._pick_backend{ }, "lua")
end)

s:test("Lua backend: GET method/path/query/headers/body round-trip", function()
  local PORT = 25601
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        r:reply(200, { ["Content-Type"] = "text/plain" },
          req.method .. " " .. req.path .. " a=" .. (req.query.a or ""))
      end,
    })
    resp = http.get(BASE .. PORT .. "/x?a=42")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.body, "GET /x a=42")
end)

s:test("Lua backend: chunked reply_start/chunk/end streams data", function()
  local PORT = 25602
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        r:reply_start(200, { ["Content-Type"] = "text/plain" })
        r:reply_chunk("one-")
        r:reply_chunk("two-")
        r:reply_chunk("three")
        r:reply_end()
      end,
    })
    resp = http.get(BASE .. PORT .. "/stream")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.body, "one-two-three")
end)

s:test("Lua backend: fan.sleep between chunks holds the connection open", function()
  local PORT = 25603
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        r:reply_start(200, {})
        r:reply_chunk("A")
        fan.sleep(0.02)
        r:reply_chunk("B")
        r:reply_end()
      end,
    })
    resp = http.get(BASE .. PORT .. "/slow")
  end)
  if server then server:close() end
  T.eq(resp.body, "AB")
end)

s:test("Lua backend: addheader accumulates and appears on the wire", function()
  local PORT = 25604
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        r:addheader("X-Header-A", "1")
        r:addheader("X-Header-B", "2")
        r:reply(200, {}, "")
      end,
    })
    resp = http.get(BASE .. PORT .. "/addhdr")
  end)
  if server then server:close() end
  T.eq(resp.headers["x-header-a"], "1")
  T.eq(resp.headers["x-header-b"], "2")
end)

s:test("Lua backend: addheader same key twice folds with ', '", function()
  local PORT = 25605
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        r:addheader("X-Fold", "a")
        r:addheader("X-Fold", "b")
        r:reply(200, {}, "")
      end,
    })
    resp = http.get(BASE .. PORT .. "/fold")
  end)
  if server then server:close() end
  T.eq(resp.headers["x-fold"], "a, b")
end)

s:test("Lua backend: caller headers override addheader (caller-wins, case-insensitive)", function()
  local PORT = 25606
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        r:addheader("X-Priority", "low")
        r:addheader("X-Extra", "stash")
        r:reply(200, { ["x-priority"] = "high" }, "ok")
      end,
    })
    resp = http.get(BASE .. PORT .. "/caller-wins")
  end)
  if server then server:close() end
  T.eq(resp.headers["x-priority"], "high")
  T.eq(resp.headers["x-extra"],    "stash")
end)

s:test("Lua backend: reply_start caller header wins over addheader", function()
  local PORT = 25607
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        r:addheader("X-Mode", "stashed")
        r:reply_start(200, { ["X-Mode"] = "start" })
        r:reply_chunk("data")
        r:reply_end()
      end,
    })
    resp = http.get(BASE .. PORT .. "/start-caller-wins")
  end)
  if server then server:close() end
  T.eq(resp.body, "data")
  T.eq(resp.headers["x-mode"], "start")
end)

s:test("Lua backend: addheader after sent_head raises", function()
  local PORT = 25608
  local server, resp
  local err_msg
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        r:reply(200, {}, "done")
        local ok, err = pcall(r.addheader, r, "X-Late", "1")
        err_msg = (not ok) and tostring(err) or nil
      end,
    })
    resp = http.get(BASE .. PORT .. "/late")
  end)
  if server then server:close() end
  T.eq(resp.body, "done")
  T.not_nil(err_msg)
  T.truthy(err_msg:find("already sent", 1, true), err_msg)
end)

s:test("Lua backend: handler error before reply -> 500", function()
  local PORT = 25609
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        error("boom-in-lua-backend")
      end,
    })
    resp = http.get(BASE .. PORT .. "/boom")
  end)
  if server then server:close() end
  T.eq(resp.status, 500)
  T.truthy(resp.body:find("boom-in-lua-backend", 1, true), resp.body)
end)

s:test("Lua backend: handler forgets to reply -> default 204", function()
  local PORT = 25610
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r) end,
    })
    resp = http.get(BASE .. PORT .. "/silent")
  end)
  if server then server:close() end
  T.eq(resp.status, 204)
end)

s:test("Lua backend: handler forgets reply_end -> serve_one auto-ends", function()
  local PORT = 25611
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        r:reply_start(200, {})
        r:reply_chunk("half")
        -- returns without reply_end -> serve_one() should auto-close the stream
      end,
    })
    resp = http.get(BASE .. PORT .. "/forget-end")
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(resp.body, "half")
end)

s:test("Lua backend: req:available / req:read incremental drain", function()
  local PORT = 25612
  local server, resp
  local trace = {}
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        trace.total = req:available()
        local part = req:read(4)
        trace.part = part
        trace.rem  = req:available()
        local rest = req:read()   -- no arg -> remaining
        trace.rest = rest
        trace.eof  = req:read()   -- nil at EOF
        r:reply(200, {}, "ok")
      end,
    })
    resp = http.post(BASE .. PORT .. "/drain",
      { headers = { ["Content-Type"] = "text/plain" }, body = "abcdefghij" })
  end)
  if server then server:close() end
  T.eq(resp.status, 200)
  T.eq(trace.total, 10)
  T.eq(trace.part,  "abcd")
  T.eq(trace.rem,   6)
  T.eq(trace.rest,  "efghij")
  T.eq(trace.eof,   nil)
end)

s:test("Lua backend: req:read(bad-arg) raises", function()
  local PORT = 25613
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        req:read(-1)  -- should raise -> caught by serve_one's pcall -> 500
      end,
    })
    resp = http.post(BASE .. PORT .. "/bad-read",
      { headers = { ["Content-Type"] = "text/plain" }, body = "xxx" })
  end)
  if server then server:close() end
  T.eq(resp.status, 500)
  T.truthy(resp.body:find("positive number", 1, true), resp.body)
end)

s:test("Lua backend: addheader arg validation (nil value, non-string name)", function()
  local PORT = 25614
  local server, resp
  local errs = {}
  run(function()
    server = assert(httpd.bind{
      port = PORT,
      handler = function(req, r)
        local ok1, e1 = pcall(r.addheader, r, "X-Nil", nil)
        errs.nil_value = (not ok1) and tostring(e1) or nil
        local ok2, e2 = pcall(r.addheader, r, 42, "v")
        errs.non_string_name = (not ok2) and tostring(e2) or nil
        r:reply(200, {}, "ok")
      end,
    })
    resp = http.get(BASE .. PORT .. "/addhdr-validate")
  end)
  if server then server:close() end
  T.eq(resp.body, "ok")
  T.not_nil(errs.nil_value)
  T.not_nil(errs.non_string_name)
end)

----------------------------------------------------------------------
-- M14.C-d guard: keep the pure-Lua WebSocket accept path exercised.
--
-- test_websocket.lua migrated to the C backend at M14.C-d. Without a
-- pinned-lua counterpart, the Lua backend's :websocket_accept and
-- fan.websocket.accept(req, resp) fall out of coverage, and the Lua
-- backend's WS branches (used by anyone opting back into the Lua
-- backend for now-legacy reasons) would silently rot.
----------------------------------------------------------------------

local websocket = require("fan.websocket")

-- tiny raw client for the handshake bytes
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

local function read_headers_lua(c)
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
  -- buf now holds any bytes past the header terminator (\r\n\r\n) — the
  -- server may have already written frame bytes into the socket before we
  -- got a chance to read past the header, so caller MUST propagate this
  -- leftover into subsequent frame reads.
  return status, hdrs, buf
end

s:test("Lua backend: fan.websocket.accept round-trips the 101 handshake", function()
  local PORT = 25701
  local server, status, hdrs
  run(function()
    server = assert(httpd.bind{ port = PORT,
      handler = function(req, resp)
        local ws = assert(websocket.accept(req, resp))
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/ws")
    status, hdrs = read_headers_lua(c)
    c:close()
  end)
  server:close()
  T.truthy(status:find("101"))
  T.eq(hdrs["sec-websocket-accept"], "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
end)

s:test("Lua backend: req:websocket_accept + websocket_send round-trip", function()
  local PORT = 25702
  local server, echoed
  run(function()
    server = assert(httpd.bind{ port = PORT,
      handler = function(req, resp)
        local ws = assert(req:websocket_accept())
        -- Read one masked client frame and echo it back.
        local msg = ws:recv()
        if msg then req:websocket_send("echo:" .. msg) end
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/echo")
    read_headers_lua(c)
    -- masked client -> server text frame "hi"
    local payload = "hi"
    local mask = "\1\2\3\4"
    local masked = {}
    for i = 1, #payload do
      masked[i] = string.char(payload:byte(i) ~ mask:byte(((i - 1) % 4) + 1))
    end
    local frame = "\x81" .. string.char(0x80 + #payload) .. mask .. table.concat(masked)
    assert(c:send(frame))
    -- Read server frame: unmasked text, len < 126
    local buf = ""
    while #buf < 2 do buf = buf .. assert(c:receive()) end
    local b0, b1 = buf:byte(1), buf:byte(2)
    T.eq(b0 & 0x0F, 0x1)   -- text opcode
    local n = b1 & 0x7F
    while #buf < 2 + n do buf = buf .. assert(c:receive()) end
    echoed = buf:sub(3, 2 + n)
    c:close()
  end)
  server:close()
  T.eq(echoed, "echo:hi")
end)

s:test("Lua backend: fan.websocket.accept negotiates permessage-deflate", function()
  local PORT = 25704
  local server, hdrs
  run(function()
    server = assert(httpd.bind{ port = PORT,
      handler = function(req, resp)
        local ws = assert(websocket.accept(req, resp))
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/pmd",
      { "Sec-WebSocket-Extensions: permessage-deflate" })
    local _, h = read_headers_lua(c)
    hdrs = h
    c:close()
  end)
  server:close()
  T.not_nil(hdrs["sec-websocket-extensions"])
  T.truthy(hdrs["sec-websocket-extensions"]:find("permessage%-deflate"))
end)

s:test("Lua backend: fan.websocket.accept rejects a plain GET with 400", function()
  local PORT = 25703
  local server, resp_line
  run(function()
    server = assert(httpd.bind{ port = PORT,
      handler = function(req, resp)
        local ws, werr = websocket.accept(req, resp)
        T.is_nil(ws); T.not_nil(werr)
      end})
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    assert(c:send("GET /ws HTTP/1.1\r\nHost: x\r\n\r\n"))
    local buf = ""
    while not buf:find("\r\n", 1, true) do
      buf = buf .. assert(c:receive())
    end
    resp_line = buf:match("^([^\r]*)\r\n")
    c:close()
  end)
  server:close()
  T.truthy(resp_line:find("400"))
end)
----------------------------------------------------------------------
-- Frame-layer coverage for the Lua backend
--
-- The four tests above exercise the handshake + a single text
-- round-trip; the WS:recv() code paths for control frames (ping/close)
-- and continuation frames — plus WS:state(), WS:pong(), and the
-- deflate inflate branch — need dedicated coverage now that
-- test_websocket.lua drives the C backend end-to-end.
----------------------------------------------------------------------

local function read_n_lua(c, buf, n)
  while #buf < n do
    local d = c:receive()
    if not d then return nil, buf end
    buf = buf .. d
  end
  return buf:sub(1, n), buf:sub(n + 1)
end

local function encode_masked_frame(fin, opcode, payload, rsv1)
  local b1 = (fin and 0x80 or 0) | (rsv1 and 0x40 or 0) | (opcode & 0x0f)
  local n = #payload
  local hdr
  if n < 126 then
    hdr = string.char(b1, 0x80 | n)
  else
    hdr = string.char(b1, 0x80 | 126) .. string.pack(">I2", n)
  end
  local mask = "\x11\x22\x33\x44"
  local out = {}
  for i = 1, n do
    out[i] = string.char(payload:byte(i) ~ mask:byte(((i - 1) % 4) + 1))
  end
  return hdr .. mask .. table.concat(out)
end

local function read_server_frame_lua(c, buf)
  local hdr; hdr, buf = read_n_lua(c, buf, 2)
  if not hdr then return nil, "eof" end
  local b1, b2 = hdr:byte(1, 2)
  local fin  = (b1 & 0x80) ~= 0
  local rsv1 = (b1 & 0x40) ~= 0
  local op   = b1 & 0x0f
  local plen = b2 & 0x7f
  if plen == 126 then
    local ext; ext, buf = read_n_lua(c, buf, 2); plen = string.unpack(">I2", ext)
  end
  local payload = ""
  if plen > 0 then payload, buf = read_n_lua(c, buf, plen) end
  return { fin = fin, opcode = op, rsv1 = rsv1, payload = payload or "" }, buf
end

-- Silence read_headers_lua leftover on the buffer: after status+headers
-- the caller wants any pipelined ws bytes back. Our raw_ws_client returns
-- a fresh conn without leftover, so we start with an empty buf.
s:test("Lua backend: WS:recv answers PING with PONG and echoes payload", function()
  local PORT = 25705
  local server, saw_close
  run(function()
    server = assert(httpd.bind{ port = PORT,
      handler = function(req, resp)
        local ws = assert(websocket.accept(req, resp))
        -- recv() must return "closed" when the peer's CLOSE arrives
        -- after the automatic PONG has been sent.
        local msg, err = ws:recv()
        saw_close = (msg == nil and err == "closed")
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/ping")
    local _, _, buf = read_headers_lua(c)
    -- send PING with payload "abcd", then CLOSE
    assert(c:send(encode_masked_frame(true, 0x9, "abcd")))
    local f; f, buf = read_server_frame_lua(c, buf)
    T.eq(f.opcode, 0xA)          -- PONG
    T.eq(f.payload, "abcd")
    assert(c:send(encode_masked_frame(true, 0x8, string.pack(">I2", 1000))))
    -- server should echo close
    f, buf = read_server_frame_lua(c, buf)
    T.eq(f.opcode, 0x8)          -- CLOSE
    c:close()
  end)
  server:close()
  T.eq(saw_close, true)
end)

s:test("Lua backend: fragmented data frames are reassembled in WS:recv", function()
  local PORT = 25706
  local server, got
  run(function()
    server = assert(httpd.bind{ port = PORT,
      handler = function(req, resp)
        local ws = assert(websocket.accept(req, resp))
        local msg = ws:recv()
        got = msg
        if msg then ws:send("<" .. msg .. ">") end
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/frag")
    local _, _, buf = read_headers_lua(c)
    -- 3 fragments: TEXT (fin=0) "he" + CONT (fin=0) "llo" + CONT (fin=1) "!"
    assert(c:send(encode_masked_frame(false, 0x1, "he")))
    assert(c:send(encode_masked_frame(false, 0x0, "llo")))
    assert(c:send(encode_masked_frame(true,  0x0, "!")))
    local f; f, buf = read_server_frame_lua(c, buf)
    T.eq(f.opcode, 0x1)
    T.eq(f.payload, "<hello!>")
    c:close()
  end)
  server:close()
  T.eq(got, "hello!")
end)

s:test("Lua backend: WS:state transitions open -> closing -> closed", function()
  local PORT = 25707
  local server, states
  run(function()
    states = {}
    server = assert(httpd.bind{ port = PORT,
      handler = function(req, resp)
        local ws = assert(websocket.accept(req, resp))
        states[#states + 1] = ws:state()          -- "open"
        -- Simulate the "closing" observation window: set close_sent
        -- manually (as if send_close_frame had run but :close hadn't
        -- flipped closed=true yet), record state, then finish close.
        ws.close_sent = true
        states[#states + 1] = ws:state()          -- "closing"
        ws:close(1000, "bye")
        states[#states + 1] = ws:state()          -- "closed"
      end})
    local c = raw_ws_client(PORT, "/st")
    read_headers_lua(c)
    c:close()
  end)
  server:close()
  T.eq(states[1], "open")
  T.eq(states[2], "closing")
  T.eq(states[3], "closed")
end)

s:test("Lua backend: WS:pong (proactive) reaches the peer", function()
  local PORT = 25708
  local server, saw
  run(function()
    server = assert(httpd.bind{ port = PORT,
      handler = function(req, resp)
        local ws = assert(websocket.accept(req, resp))
        ws:pong("hi-pong")
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/pong")
    local _, _, buf = read_headers_lua(c)
    local f; f, buf = read_server_frame_lua(c, buf)
    T.eq(f.opcode, 0xA)          -- PONG opcode
    T.eq(f.payload, "hi-pong")
    saw = true
    c:close()
  end)
  server:close()
  T.eq(saw, true)
end)

s:test("Lua backend: WS.receive is an alias for WS:recv", function()
  local PORT = 25709
  local server, got
  run(function()
    server = assert(httpd.bind{ port = PORT,
      handler = function(req, resp)
        local ws = assert(websocket.accept(req, resp))
        got = websocket -- keep upvalue live
        local msg = ws.receive(ws)   -- dot-call alias, self passed manually
        if msg then ws:send("A:" .. msg) end
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/recv")
    local _, _, buf = read_headers_lua(c)
    assert(c:send(encode_masked_frame(true, 0x1, "z")))
    local f; f, buf = read_server_frame_lua(c, buf)
    T.eq(f.payload, "A:z")
    c:close()
  end)
  server:close()
  T.truthy(got)
end)

s:test("Lua backend: permessage-deflate round-trip via WS:send + recv", function()
  local PORT = 25710
  if not fan.zlib or not (fan.zlib.available and fan.zlib.available()) then
    return  -- zlib not compiled in; skip silently
  end
  local server, got
  run(function()
    server = assert(httpd.bind{ port = PORT,
      handler = function(req, resp)
        local ws = assert(websocket.accept(req, resp))
        local msg = ws:recv()
        got = msg
        if msg then ws:send("R:" .. msg) end
        ws:close(1000)
      end})
    local c = raw_ws_client(PORT, "/pmd",
      { "Sec-WebSocket-Extensions: permessage-deflate" })
    local _, hdrs, buf = read_headers_lua(c)
    T.truthy((hdrs["sec-websocket-extensions"] or ""):find("permessage%-deflate"))
    -- Send a deflated text frame: deflate "helloworld", strip sync trailer,
    -- send with RSV1=1.
    local zlib = fan.zlib
    local d = assert(zlib.deflate_raw("helloworld", nil, true))
    if d:sub(-4) == "\0\0\xff\xff" then d = d:sub(1, -5) end
    assert(c:send(encode_masked_frame(true, 0x1, d, true)))
    local f; f, buf = read_server_frame_lua(c, buf)
    T.truthy(f.rsv1)   -- server responded with deflated frame too
    -- inflate the server's response and confirm
    local body = f.payload .. "\0\0\xff\xff"
    local reply = assert(zlib.inflate_raw(body))
    T.eq(reply, "R:helloworld")
    c:close()
  end)
  server:close()
  T.eq(got, "helloworld")
end)

os.exit(T.run(s))
