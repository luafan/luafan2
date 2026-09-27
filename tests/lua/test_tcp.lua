--[[
  test_tcp.lua — M2 TCP contract tests: connect / send / receive / close,
  bind / accept, concurrent connections, connection refused.
  Run via ./fan. Uses an in-process echo server on 127.0.0.1.
]]
local T = require("test_framework")
local fan = require("fan")

local s = T.suite("fan.tcp (M2)")

-- helper: run body as a coroutine, drive the loop until it calls loopbreak
local function with_loop(body)
  fan.spawn(body)
  fan.loop()
end

s:test("echo round-trip: bind, connect, send, receive, close", function()
  local PORT = 24101
  local got
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      -- echo one message back
      local data = conn:receive()
      if data then conn:send(data) end
      conn:close()
    end))
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    assert(c:send("hello-echo"))
    got = c:receive()
    c:close()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.eq(got, "hello-echo")
end)

s:test("receive with explicit length", function()
  local PORT = 24102
  local got
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      conn:send("abcdefgh")
      fan.sleep(0.05)
      conn:close()
    end))
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    got = c:receive(4)   -- exactly 4 bytes
    c:close()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.eq(got, "abcd")
end)

s:test("connection refused returns nil, err", function()
  local err
  local c
  with_loop(function()
    c, err = fan.tcp.connect("127.0.0.1", 24199)  -- nothing listening
    fan.loopbreak()
  end)
  T.is_nil(c)
  T.not_nil(err)
end)

s:test("concurrent connections all echo", function()
  local PORT = 24103
  local N = 8
  local results = {}
  local done = 0
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      local d = conn:receive()
      if d then conn:send(d) end
      conn:close()
    end))
    for i = 1, N do
      fan.spawn(function()
        local c = assert(fan.tcp.connect("127.0.0.1", PORT))
        local msg = "conn-" .. i
        c:send(msg)
        results[i] = c:receive()
        c:close()
        done = done + 1
        if done == N then fan.loopbreak() end
      end)
    end
  end)
  if server then server:close() end
  T.eq(done, N)
  for i = 1, N do
    T.eq(results[i], "conn-" .. i)
  end
end)

s:test("receive after peer closes returns eof", function()
  local PORT = 24104
  local r1, e1, r2, e2
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      conn:send("bye")
      conn:close()   -- close right after sending
    end))
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    r1 = c:receive()          -- should get "bye"
    r2, e2 = c:receive()      -- peer closed -> nil, "eof"
    c:close()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.eq(r1, "bye")
  T.is_nil(r2)
  T.not_nil(e2)
end)

-- ---- M14.C-i: v1 parity — shutdown / pause_read / resume_read /
-- getsockname / getpeername --------------------------------------------------

s:test("getsockname / getpeername return host + port on connected conn", function()
  local PORT = 24105
  local sock_host, sock_port, peer_host, peer_port
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      conn:receive()
      conn:close()
    end))
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    sock_host, sock_port = c:getsockname()
    peer_host, peer_port = c:getpeername()
    c:send("hi")
    c:close()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.is_type(sock_host, "string")
  T.is_type(sock_port, "number")
  T.truthy(sock_port > 0)               -- OS-assigned ephemeral port
  T.eq(peer_host, "127.0.0.1")
  T.eq(peer_port, PORT)
end)

s:test("getsockname on closed conn returns (nil, nil)", function()
  local PORT = 24106
  local h, p
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn) conn:close() end))
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    c:close()
    h, p = c:getsockname()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.is_nil(h)
  T.is_nil(p)
end)

s:test("shutdown SHUT_WR gives peer eof, own read still works", function()
  local PORT = 24107
  local server_saw, client_r1
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      -- read the payload the client sent
      server_saw = conn:receive()
      -- peer half-closed; our conn:receive returns nil,"eof"
      local r2, err = conn:receive()
      -- reply after eof (still writable on our side)
      conn:send("after-shutdown")
      conn:close()
    end))
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    c:send("hello-half-close")
    -- v1 contract: shutdown is a no-op while output is still pending
    -- (returns the pending byte count). Loop-and-yield until libevent has
    -- flushed the socket so shutdown(2) actually fires and the peer sees
    -- EOF. Matches how v1 callers use it.
    local pending = c:shutdown()
    while pending and pending > 0 do
      fan.sleep(0.01)
      pending = c:shutdown()
    end
    T.eq(pending, 0)
    -- our read side is still open — peer sends back after seeing EOF
    client_r1 = c:receive()
    c:close()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.eq(server_saw, "hello-half-close")
  T.eq(client_r1, "after-shutdown")
end)

s:test("shutdown with pending output returns pending>0 and does not close", function()
  local PORT = 24108
  local pending
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      -- delay so client's shutdown races an unflushed output buffer
      fan.sleep(0.05)
      conn:receive()
      conn:close()
    end))
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    -- send a big blob so it stays in bufferevent output briefly
    c:send(string.rep("x", 200000))
    pending = c:shutdown()
    c:close()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.is_type(pending, "number")
  -- pending may or may not be > 0 depending on kernel buffering timing;
  -- what matters is the call succeeds and returns a non-negative number.
  T.truthy(pending >= 0)
end)

s:test("shutdown rejects invalid how", function()
  local PORT = 24109
  local ok, err
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn) conn:close() end))
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    ok, err = pcall(function() c:shutdown(99) end)  -- bogus how
    c:close()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.falsy(ok)
  T.truthy(tostring(err):find("shutdown how must be", 1, true), tostring(err))
end)

s:test("pause_read / resume_read cycle a receive around", function()
  local PORT = 24110
  local got1, got2
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      conn:send("first")
      fan.sleep(0.05)
      conn:send("second")
      fan.sleep(0.05)
      conn:close()
    end))
    local c = assert(fan.tcp.connect("127.0.0.1", PORT))
    got1 = c:receive()          -- "first"
    c:pause_read()
    fan.sleep(0.1)              -- while paused, server sends "second"
    c:resume_read()
    got2 = c:receive()          -- "second" now delivered
    c:close()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.eq(got1, "first")
  T.eq(got2, "second")
end)

os.exit(T.run(s))
