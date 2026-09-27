--[[
  test_websocket.lua — M4 WebSocket server contract tests (fan.websocket).

  We drive the server (fan.httpd + websocket.accept) with a raw fan.tcp
  client, because we do not ship a client-side WS library in this iteration.
  The raw client:
    - completes the HTTP Upgrade handshake and verifies the accept key
    - sends masked client -> server data / control frames
    - reads unmasked server -> client frames

  Plan \u00a74.6 WS coverage:
    - handshake success (Sec-WebSocket-Accept computed correctly)
    - handshake failure (missing / wrong headers -> 400)
    - text frame round-trip
    - binary frame round-trip
    - ping -> automatic pong echo
    - close handshake (client CLOSE -> server echoes CLOSE, recv reports closed)
    - large payloads: 126 boundary (7+16 length), and > 65535 (7+64 length)
    - fragmented data frames are reassembled by ws:recv()
    - cross-coroutine ws:send() is serialised (no interleaved frames)
]]
-- M14.C-d: WebSocket upgrade wired through C evhttp. This file now
-- exercises whichever backend the shim's default picks (currently the
-- C backend). test_httpd_lua.lua keeps dedicated pin-lua coverage of
-- the pure-Lua handshake path.

local T = require("test_framework")
local fan = require("fan")
local httpd = require("fan.httpd")
local websocket = require("fan.websocket")

local s = T.suite("fan.websocket server (M4)")

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
-- Tiny raw WS client over fan.tcp (test helper only, no library value)
----------------------------------------------------------------------
local function read_n(c, buf, n)
  while #buf < n do
    local d = c:receive()
    if not d then return nil, buf end
    buf = buf .. d
  end
  return buf:sub(1, n), buf:sub(n + 1)
end

local function read_line(c, buf)
  while true do
    local nl = buf:find("\r\n", 1, true)
    if nl then return buf:sub(1, nl - 1), buf:sub(nl + 2) end
    local d = c:receive()
    if not d then return nil, buf end
    buf = buf .. d
  end
end

-- perform the HTTP Upgrade handshake; returns the leftover receive buffer
-- and the server's accept + extensions header values. `extra_headers` is
-- an optional list of raw request header lines to include.
local function do_handshake(c, path, key, extra_headers)
  key = key or "dGhlIHNhbXBsZSBub25jZQ=="
  local lines = {
    "GET " .. path .. " HTTP/1.1",
    "Host: 127.0.0.1",
    "Upgrade: websocket",
    "Connection: Upgrade",
    "Sec-WebSocket-Key: " .. key,
    "Sec-WebSocket-Version: 13",
  }
  if extra_headers then
    for _, h in ipairs(extra_headers) do lines[#lines + 1] = h end
  end
  lines[#lines + 1] = ""; lines[#lines + 1] = ""
  local req = table.concat(lines, "\r\n")
  assert(c:send(req))
  local buf = ""
  local status, hdr_accept, hdr_ext
  while true do
    local line
    line, buf = read_line(c, buf)
    if not line then error("connection closed during handshake") end
    if not status then status = line end
    if line == "" then break end
    local lk = line:match("^([^:]+):%s*(.*)$")
    if lk then
      local lower = lk:lower()
      if lower == "sec-websocket-accept" then
        hdr_accept = line:match(":%s*(.*)$")
      elseif lower == "sec-websocket-extensions" then
        hdr_ext = line:match(":%s*(.*)$")
      end
    end
  end
  return buf, status, hdr_accept, hdr_ext
end

local function encode_client_frame(fin, opcode, payload, rsv1)
  local b1 = (fin and 0x80 or 0) | (rsv1 and 0x40 or 0) | (opcode & 0x0f)
  local n = #payload
  local hdr
  if n < 126 then
    hdr = string.char(b1, 0x80 | n)
  elseif n < 65536 then
    hdr = string.char(b1, 0x80 | 126) .. string.pack(">I2", n)
  else
    hdr = string.char(b1, 0x80 | 127) .. string.pack(">I8", n)
  end
  local mask = "\x11\x22\x33\x44"
  local out = {}
  for i = 1, n do
    out[i] = string.char(payload:byte(i) ~ mask:byte(((i - 1) % 4) + 1))
  end
  return hdr .. mask .. table.concat(out)
end

local function read_server_frame(c, buf)
  local hdr; hdr, buf = read_n(c, buf, 2)
  if not hdr then return nil, "eof" end
  local b1, b2 = hdr:byte(1, 2)
  local fin  = (b1 & 0x80) ~= 0
  local rsv1 = (b1 & 0x40) ~= 0
  local op   = b1 & 0x0f
  local masked = (b2 & 0x80) ~= 0
  local plen = b2 & 0x7f
  if plen == 126 then
    local ext; ext, buf = read_n(c, buf, 2); plen = string.unpack(">I2", ext)
  elseif plen == 127 then
    local ext; ext, buf = read_n(c, buf, 8); plen = string.unpack(">I8", ext)
  end
  if masked then
    local _; _, buf = read_n(c, buf, 4)  -- shouldn't happen server->client but tolerate
  end
  local payload = ""
  if plen > 0 then payload, buf = read_n(c, buf, plen) end
  return { fin = fin, opcode = op, rsv1 = rsv1, payload = payload or "" }, buf
end

----------------------------------------------------------------------
-- Tests
----------------------------------------------------------------------
s:test("compute_accept matches the SHA1+base64 contract", function()
  -- OpenSSL/independent verification: sha1(key .. GUID) then base64
  -- We compare against our own re-implementation on a different key to guard
  -- against regressions in either half.
  local a1 = websocket.compute_accept("dGhlIHNhbXBsZSBub25jZQ==")
  T.eq(a1, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")   -- RFC 6455 §1.3 canonical
  -- key of length 22 (no '=' padding in input) should also work
  local a2 = websocket.compute_accept("x3JJHMbDL1EzLkh9GBhXDw==")
  T.eq(a2, "HSmrc0sMlYUkAGmm5OPpG2HaGWk=")
end)

s:test("handshake success: 101 with correct Sec-WebSocket-Accept", function()
  local PORT = 24501
  local server, ok_status, ok_accept
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws, werr = websocket.accept(req, resp)
      if not ws then error(werr) end
      ws:close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local _, status, accept = do_handshake(c, "/ws")
    ok_status = status; ok_accept = accept
    c:close()
  end)
  server:close()
  T.truthy(ok_status:find("101"))
  T.eq(ok_accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
end)

s:test("handshake failure: missing Upgrade header -> 400", function()
  local PORT = 24502
  local server, resp_line
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws, werr = websocket.accept(req, resp)
      -- accept must reply 400 itself; handler ends without further reply
      T.is_nil(ws); T.not_nil(werr)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    c:send("GET /ws HTTP/1.1\r\nHost: x\r\n\r\n")
    local buf = ""
    resp_line, buf = read_line(c, buf)
    c:close()
  end)
  server:close()
  T.truthy(resp_line:find("400"))
end)

s:test("text frame round-trip (client -> server -> echo)", function()
  local PORT = 24503
  local server, got
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws = assert(websocket.accept(req, resp))
      local msg, op = ws:recv()
      if msg then ws:send("echo:" .. msg) end
      ws:close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/echo")
    c:send(encode_client_frame(true, 0x1, "hello"))
    local f; f, buf = read_server_frame(c, buf)
    got = f
    c:close()
  end)
  server:close()
  T.not_nil(got); T.eq(got.opcode, 0x1); T.eq(got.payload, "echo:hello")
end)

s:test("binary frame round-trip", function()
  local PORT = 24504
  local server, got
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws = assert(websocket.accept(req, resp))
      local msg, op = ws:recv()
      if msg and op == "binary" then ws:send_binary(msg:reverse()) end
      ws:close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/bin")
    c:send(encode_client_frame(true, 0x2, "\x01\x02\x03\x04"))
    local f; f, buf = read_server_frame(c, buf)
    got = f
    c:close()
  end)
  server:close()
  T.not_nil(got); T.eq(got.opcode, 0x2); T.eq(got.payload, "\x04\x03\x02\x01")
end)

s:test("ping is answered with pong (payload echoed)", function()
  local PORT = 24505
  local server, got
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws = assert(websocket.accept(req, resp))
      -- keep the recv loop live long enough to answer the ping
      ws:recv()  -- will unblock on client CLOSE
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/p")
    c:send(encode_client_frame(true, 0x9, "PINGDATA"))
    local f; f, buf = read_server_frame(c, buf)
    got = f
    -- clean shutdown: send close so the server's recv returns
    c:send(encode_client_frame(true, 0x8, ""))
    c:close()
  end)
  server:close()
  T.not_nil(got); T.eq(got.opcode, 0xA); T.eq(got.payload, "PINGDATA")
end)

s:test("close handshake: client CLOSE -> server echoes CLOSE, recv -> closed", function()
  local PORT = 24506
  local server, saw_close_frame, recv_ret
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws = assert(websocket.accept(req, resp))
      local _, err = ws:recv()
      recv_ret = err
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/c")
    c:send(encode_client_frame(true, 0x8, string.pack(">I2", 1000) .. "bye"))
    local f; f, buf = read_server_frame(c, buf)
    saw_close_frame = f
    c:close()
  end)
  server:close()
  T.not_nil(saw_close_frame); T.eq(saw_close_frame.opcode, 0x8)
  T.eq(recv_ret, "closed")
end)

s:test("large payload uses 16-bit extended length (n=200)", function()
  local PORT = 24507
  local server, got
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws = assert(websocket.accept(req, resp))
      local msg = ws:recv()
      if msg then ws:send(msg) end
      ws:close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/big16")
    local payload = string.rep("A", 200)
    c:send(encode_client_frame(true, 0x1, payload))
    local f; f, buf = read_server_frame(c, buf)
    got = f
    c:close()
  end)
  server:close()
  T.not_nil(got); T.eq(#got.payload, 200)
end)

s:test("very large payload uses 64-bit extended length (n=70000)", function()
  local PORT = 24508
  local server, got_len
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws = assert(websocket.accept(req, resp))
      local msg = ws:recv()
      if msg then ws:send(msg) end
      ws:close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/big64")
    local payload = string.rep("B", 70000)
    c:send(encode_client_frame(true, 0x1, payload))
    local f; f, buf = read_server_frame(c, buf)
    got_len = f and #f.payload
    c:close()
  end)
  server:close()
  T.eq(got_len, 70000)
end)

s:test("fragmented data frames are reassembled into one message", function()
  local PORT = 24509
  local server, echoed
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws = assert(websocket.accept(req, resp))
      local msg = ws:recv()
      if msg then ws:send("<" .. msg .. ">") end
      ws:close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/frag")
    -- 3 fragments: TEXT (fin=0) "hel" + CONT (fin=0) "lo," + CONT (fin=1) "world"
    c:send(encode_client_frame(false, 0x1, "hel"))
    c:send(encode_client_frame(false, 0x0, "lo,"))
    c:send(encode_client_frame(true,  0x0, "world"))
    local f; f, buf = read_server_frame(c, buf)
    echoed = f and f.payload
    c:close()
  end)
  server:close()
  T.eq(echoed, "<hello,world>")
end)

s:test("cross-coroutine ws:send is serialised (no interleaved frames)", function()
  local PORT = 24510
  local server, frames = nil, {}
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws = assert(websocket.accept(req, resp))
      -- spawn two senders + one that closes after both have run
      local finished = 0
      fan.spawn(function()
        for i = 1, 3 do ws:send("A" .. i); fan.sleep(0.005) end
        finished = finished + 1
      end)
      fan.spawn(function()
        for i = 1, 3 do ws:send("B" .. i); fan.sleep(0.005) end
        finished = finished + 1
      end)
      while finished < 2 do fan.sleep(0.01) end
      ws:close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/mux")
    -- expect at least 6 data frames + 1 close frame
    for _ = 1, 7 do
      local f; f, buf = read_server_frame(c, buf)
      if not f then break end
      frames[#frames + 1] = f
      if f.opcode == 0x8 then break end
    end
    c:close()
  end)
  server:close()
  -- 6 data frames delivered whole (each payload is exactly "An" or "Bn"),
  -- and the last frame is a CLOSE. No frame boundary was straddled because
  -- send_locked serialises calls.
  local data_ok = 0
  for _, f in ipairs(frames) do
    if f.opcode == 0x1 and #f.payload == 2 and
       (f.payload:sub(1,1) == "A" or f.payload:sub(1,1) == "B") then
      data_ok = data_ok + 1
    end
  end
  T.eq(data_ok, 6)
end)

s:test("permessage-deflate: handshake advertises the extension when offered", function()
  local PORT = 24511
  local server, status, accept, ext
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws = assert(websocket.accept(req, resp))
      -- read+echo a single message so the client can complete its round-trip
      local msg, op = ws:recv()
      if msg then ws:send(msg, op) end
      ws:close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf
    buf, status, accept, ext = do_handshake(c, "/deflate", nil,
      { "Sec-WebSocket-Extensions: permessage-deflate" })
    -- send an uncompressed frame so the server can echo it (server may
    -- deflate its reply); we just verify the handshake here.
    c:send(encode_client_frame(true, 0x1, "plain"))
    -- drain one server frame so it fully unwinds; ignore contents
    read_server_frame(c, buf)
    c:close()
  end)
  server:close()
  T.truthy(status:find("101"))
  T.not_nil(ext)
  T.truthy(ext:lower():find("permessage%-deflate"))
end)

s:test("permessage-deflate: server-compressed reply round-trips via inflate", function()
  local PORT = 24512
  local server, echoed, saw_rsv1
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws = assert(websocket.accept(req, resp))
      local msg, op = ws:recv()
      if msg then ws:send(msg:rep(20), op) end   -- highly compressible reply
      ws:close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/rt", nil,
      { "Sec-WebSocket-Extensions: permessage-deflate" })

    -- client sends an uncompressed text frame (allowed even under deflate)
    c:send(encode_client_frame(true, 0x1, "hi"))

    local f; f, buf = read_server_frame(c, buf)
    saw_rsv1 = f and f.rsv1
    if f then
      if f.rsv1 then
        -- append the sync flush trailer and inflate
        local inflated = assert(fan.zlib.inflate_raw(f.payload .. "\0\0\xff\xff"))
        echoed = inflated
      else
        echoed = f.payload
      end
    end
    c:close()
  end)
  server:close()
  T.truthy(saw_rsv1, "server should have compressed this reply (RSV1 set)")
  T.eq(echoed, ("hi"):rep(20))
end)

s:test("permessage-deflate: client-compressed request is inflated by the server", function()
  local PORT = 24513
  local server, echoed
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ws = assert(websocket.accept(req, resp))
      local msg, op = ws:recv()
      if msg then ws:send(">> " .. msg, op) end
      ws:close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/req", nil,
      { "Sec-WebSocket-Extensions: permessage-deflate" })

    -- deflate + strip sync trailer + set RSV1
    local raw = "abc-abc-abc-abc-abc-abc"
    local d = assert(fan.zlib.deflate_raw(raw, nil, true))  -- sync-flush
    if d:sub(-4) == "\0\0\xff\xff" then d = d:sub(1, -5) end
    c:send(encode_client_frame(true, 0x1, d, true))

    local f; f, buf = read_server_frame(c, buf)
    if f then
      if f.rsv1 then
        local inflated = assert(fan.zlib.inflate_raw(f.payload .. "\0\0\xff\xff"))
        echoed = inflated
      else
        echoed = f.payload
      end
    end
    c:close()
  end)
  server:close()
  T.eq(echoed, ">> abc-abc-abc-abc-abc-abc")
end)

-- ---------------------------------------------------------------------------
-- v1 fan.httpd WebSocket parity surface (M15). The M4 block above already
-- exercises fan.websocket.accept; here we exercise the request-object
-- surface (is_websocket_upgrade / websocket_accept / websocket_send / ...
-- websocket_state) that v1 code expects, plus WS:pong / :state / :receive.
-- ---------------------------------------------------------------------------

s:test("v1: req:is_websocket_upgrade() returns true on a real handshake", function()
  local PORT = 24601
  local server, saw_upgrade
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      saw_upgrade = req:is_websocket_upgrade()
      if saw_upgrade then
        local ws = assert(req:websocket_accept())
        local m = ws:recv(); ws:send(m or "")
        ws:close(1000)
      else
        resp:reply(400, {}, "no")
      end
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/")
    c:send(encode_client_frame(true, 0x1, "hello"))
    local f
    f, buf = read_server_frame(c, buf)
    T.eq(f.payload, "hello")
    c:close()
  end)
  server:close()
  T.eq(saw_upgrade, true)
end)

s:test("v1: req:is_websocket_upgrade() returns false on a plain HTTP request", function()
  local PORT = 24602
  local server, saw_upgrade
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      saw_upgrade = req:is_websocket_upgrade()
      resp:reply(200, {}, "ok")
    end })
    local http = require("fan.http")
    local r = http.get("http://127.0.0.1:" .. PORT .. "/")
    T.eq(r.status, 200)
  end)
  server:close()
  T.eq(saw_upgrade, false)
end)

s:test("v1: req:websocket_send / receive round-trip", function()
  local PORT = 24603
  local server
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req)
      assert(req:websocket_accept())
      local msg = req:websocket_receive()
      req:websocket_send("echo:" .. tostring(msg))
      req:websocket_close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/wsv1")
    c:send(encode_client_frame(true, 0x1, "hi-there"))
    local f
    f, buf = read_server_frame(c, buf)
    T.eq(f.payload, "echo:hi-there")
    c:close()
  end)
  server:close()
end)

s:test("v1: req:websocket_ping / pong exchange (client answers server ping)", function()
  local PORT = 24604
  local server, pong_seen
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req)
      assert(req:websocket_accept())
      -- Send a ping from server -> client, then read the pong echo.
      req:websocket_ping("ping-payload")
      local msg = req:websocket_receive()  -- driven-close terminates the loop
      req:websocket_close(1000)
      return msg
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/pw")
    -- expect a PING frame (opcode 0x9) from the server
    local f
    f, buf = read_server_frame(c, buf)
    T.eq(f.opcode, 0x9)
    T.eq(f.payload, "ping-payload")
    pong_seen = true
    -- reply with a PONG so the server sees traffic
    c:send(encode_client_frame(true, 0xA, "ping-payload"))
    -- close normally
    c:send(encode_client_frame(true, 0x8, string.pack(">I2", 1000)))
    c:close()
  end)
  server:close()
  T.eq(pong_seen, true)
end)

s:test("v1: req:websocket_pong (proactive) reaches the peer", function()
  local PORT = 24608
  local server, saw_pong
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req)
      assert(req:websocket_accept())
      -- Send an unsolicited PONG from server; then wait for client CLOSE.
      req:websocket_pong("proactive-pong")
      req:websocket_receive()
      req:websocket_close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/pong")
    local f
    f, buf = read_server_frame(c, buf)
    T.eq(f.opcode, 0xA)   -- PONG
    T.eq(f.payload, "proactive-pong")
    saw_pong = true
    c:send(encode_client_frame(true, 0x8, string.pack(">I2", 1000)))
    c:close()
  end)
  server:close()
  T.eq(saw_pong, true)
end)

s:test("v1: req:websocket_state transitions connecting -> open -> closed", function()
  local PORT = 24605
  local server, states, done
  run(function()
    states = {}
    server = assert(httpd.bind{ port = PORT, handler = function(req)
      states[#states + 1] = req:websocket_state()  -- "connecting"
      assert(req:websocket_accept())
      states[#states + 1] = req:websocket_state()  -- "open"
      req:websocket_receive()
      req:websocket_close(1000)
      states[#states + 1] = req:websocket_state()  -- "closed"
      done = true
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/st")
    c:send(encode_client_frame(true, 0x1, "x"))
    -- wait for the server's CLOSE frame so we know the handler ran past
    -- websocket_close before we tear down the loop.
    local f
    f, buf = read_server_frame(c, buf)
    T.eq(f.opcode, 0x8)
    c:close()
    -- pump one more tick so the handler's line after websocket_close
    -- (states[3] = ...) is guaranteed to have run.
    while not done do fan.sleep(0.005) end
  end)
  server:close()
  T.eq(states[1], "connecting")
  T.eq(states[2], "open")
  T.eq(states[3], "closed")
end)

s:test("v1: websocket_send before accept -> nil, err (not a websocket)", function()
  local PORT = 24606
  local server, err_seen
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req, resp)
      local ok, err = req:websocket_send("x")
      err_seen = err
      resp:reply(400, {}, "bad")
    end })
    local http = require("fan.http")
    http.get("http://127.0.0.1:" .. PORT .. "/")
  end)
  server:close()
  T.truthy(err_seen)
  T.truthy(tostring(err_seen):find("websocket_send", 1, true))
end)

s:test("v1: WS:pong (proactive) is transported to the peer", function()
  local PORT = 24607
  local server, saw
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req)
      local ws = assert(req:websocket_accept())
      ws:pong("hello-pong")
      req:websocket_close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/p")
    local f
    f, buf = read_server_frame(c, buf)
    -- The first server-initiated frame must be a PONG (opcode 0xA).
    T.eq(f.opcode, 0xA)
    T.eq(f.payload, "hello-pong")
    saw = true
    c:close()
  end)
  server:close()
  T.eq(saw, true)
end)

s:test("v1: WS:state reflects the local view of the close handshake", function()
  -- fan.ws.conn is now an opaque userdata (no user-value stash), so the
  -- v1-style monkey-patch of `ws.close` is no longer possible — nor
  -- meaningful, since the C backend's :close() commits the local view of
  -- the close (WS_ST_EOF) synchronously before the deferred teardown
  -- fires. Instead, exercise the observable state contract directly:
  --   * before close       -> "open"
  --   * after ws:close(..) -> "closed" (local view; TCP FIN in flight)
  local PORT = 24608
  local server, states
  run(function()
    states = {}
    server = assert(httpd.bind{ port = PORT, handler = function(req)
      local ws = assert(req:websocket_accept())
      states[#states + 1] = ws:state()          -- "open"
      ws:close(1000, "bye")
      states[#states + 1] = ws:state()          -- "closed"
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    do_handshake(c, "/cs")
    c:close()
  end)
  server:close()
  T.eq(states[1], "open")
  T.eq(states[2], "closed")
end)

s:test("v1: WS.receive is an alias for WS:recv", function()
  local PORT = 24609
  local server
  run(function()
    server = assert(httpd.bind{ port = PORT, handler = function(req)
      local ws = assert(req:websocket_accept())
      local msg = ws:receive()                  -- v1 name
      ws:send(">>" .. tostring(msg))
      ws:close(1000)
    end })
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    local buf = do_handshake(c, "/r")
    c:send(encode_client_frame(true, 0x1, "abc"))
    local f
    f, buf = read_server_frame(c, buf)
    T.eq(f.payload, ">>abc")
    c:close()
  end)
  server:close()
end)

os.exit(T.run(s))
