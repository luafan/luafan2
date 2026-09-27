--[[
  test_connector.lua — fan.connector URL dispatch contract tests.

  Covers all four schemes (tcp / fifo / udp / popen) end-to-end plus
  URL-parsing edge cases and per-scheme error branches. Runs via ./fan.

  Design notes:
    * TCP + FIFO already have narrow sanity coverage in test_fifo.lua;
      the tests here focus on connector-level dispatch (URL shape, bad
      inputs, IPv6 bracket form, unsupported operations) rather than
      re-testing the underlying transports.
    * UDP scheme returns a facade (send/recv/close) for connect; a raw
      fan.udp sock for bind (so callers can also sendto other peers).
    * popen scheme has no bind: v1's connector never exposed one and the
      shape wouldn't make sense (a process has no accept queue).
]]
local T = require("test_framework")
local fan = require("fan")
local connector = require("fan.connector")

local s = T.suite("fan.connector (M14.C-f)")

local function with_loop(body)
  fan.spawn(function()
    local ok, err = pcall(body)
    fan.loopbreak()
    if not ok then error(err, 0) end
  end)
  fan.loop()
end

local function tmpname(tag)
  return "/tmp/fan2_conn_" .. tag .. "_" .. tostring(os.time())
                          .. "_" .. tostring(math.random(1, 1e6))
end

----------------------------------------------------------------------
-- URL-parsing sanity — before any I/O
----------------------------------------------------------------------

s:test("connect rejects malformed URLs (missing scheme, empty rest)", function()
  local c, err = connector.connect("no-scheme-here")
  T.is_nil(c); T.not_nil(err)
  T.truthy(tostring(err):find("invalid url"))
  -- popen with empty command
  c, err = connector.connect("popen:")
  T.is_nil(c); T.not_nil(err)
  T.truthy(tostring(err):find("command"))
end)

s:test("connect / bind reject unknown scheme with a helpful message", function()
  local c, err = connector.connect("wat://nope:1")
  T.is_nil(c); T.not_nil(err)
  T.truthy(tostring(err):find("unsupported scheme"))
  local b, berr = connector.bind("wat://nope:1", function() end)
  T.is_nil(b); T.not_nil(berr)
  T.truthy(tostring(berr):find("unsupported scheme"))
end)

s:test("tcp URL parsing: rejects missing port", function()
  local c, err = connector.connect("tcp://127.0.0.1")
  T.is_nil(c); T.not_nil(err)
  T.truthy(tostring(err):find("host:port"))
  local b, berr = connector.bind("tcp://127.0.0.1", function() end)
  T.is_nil(b); T.not_nil(berr)
end)

s:test("udp URL parsing: rejects missing port and out-of-range port", function()
  local c, err = connector.connect("udp://127.0.0.1")
  T.is_nil(c); T.not_nil(err)
  T.truthy(tostring(err):find("host:port"))
  c, err = connector.connect("udp://127.0.0.1:70000")
  T.is_nil(c); T.not_nil(err)
end)

----------------------------------------------------------------------
-- TCP dispatch (kept minimal; test_fifo already has round-trip)
----------------------------------------------------------------------

s:test("tcp://host:port round-trip via connector.connect / connector.bind", function()
  local PORT = 24401
  local got
  local server
  with_loop(function()
    server = assert(connector.bind("tcp://127.0.0.1:" .. PORT, function(conn)
      local d = conn:receive()
      if d then conn:send(d) end
      conn:close()
    end))
    local c = assert(connector.connect("tcp://127.0.0.1:" .. PORT))
    c:send("tcp-hello")
    got = c:receive()
    c:close()
  end)
  if server then server:close() end
  T.eq(got, "tcp-hello")
end)

----------------------------------------------------------------------
-- FIFO dispatch
----------------------------------------------------------------------

s:test("fifo:///path round-trip via connector.connect / connector.bind", function()
  local path = tmpname("fifo")
  local got
  with_loop(function()
    -- bind opens the reader end + spawns on_accept once with the fifo.
    connector.bind("fifo://" .. path, function(fifo)
      got = fifo:receive()
      fifo:close()
    end)
    fan.sleep(0.02)  -- let the reader coroutine park in receive()
    local w = assert(connector.connect("fifo://" .. path))
    w:send("fifo-hello")
    fan.sleep(0.05)
    w:close()
  end)
  os.remove(path)
  T.eq(got, "fifo-hello")
end)

s:test("fifo: alternate URL shapes (fifo:/path and fifo:path)", function()
  local path = tmpname("fifo-alt")
  local got
  with_loop(function()
    connector.bind("fifo:" .. path, function(fifo)
      got = fifo:receive()
      fifo:close()
    end)
    fan.sleep(0.02)
    local w = assert(connector.connect("fifo:" .. path))
    w:send("alt-shape")
    fan.sleep(0.05)
    w:close()
  end)
  os.remove(path)
  T.eq(got, "alt-shape")
end)

----------------------------------------------------------------------
-- UDP dispatch
----------------------------------------------------------------------

s:test("udp://host:port round-trip via connector.connect + connector.bind", function()
  local PORT = 24402
  local server_got, server_from_host, server_from_port
  local server
  with_loop(function()
    server = assert(connector.bind("udp://127.0.0.1:" .. PORT,
      function(sock, data, host, port)
        server_got, server_from_host, server_from_port = data, host, port
        sock:sendto("ack:" .. data, host, port)
      end))
    fan.sleep(0.02)
    local c = assert(connector.connect("udp://127.0.0.1:" .. PORT))
    c:send("udp-hello")
    local reply = c:recv()
    T.eq(reply, "ack:udp-hello")
    c:close()
  end)
  if server then server:close() end
  T.eq(server_got, "udp-hello")
  T.eq(server_from_host, "127.0.0.1")
  T.truthy(server_from_port and server_from_port > 0)
end)

s:test("udp connect: send/recv after close return (nil, 'closed')", function()
  with_loop(function()
    local c = assert(connector.connect("udp://127.0.0.1:24403"))
    c:close()
    local ok, err = c:send("late")
    T.is_nil(ok); T.eq(err, "closed")
    ok, err = c:recv()
    T.is_nil(ok); T.eq(err, "closed")
    -- double-close is a no-op.
    c:close()
  end)
end)

s:test("udp bind: on_accept errors are logged but the reader stays alive", function()
  local PORT = 24404
  local server
  local reply
  local second_ok = false
  local calls = 0
  with_loop(function()
    server = assert(connector.bind("udp://127.0.0.1:" .. PORT,
      function(sock, data, host, port)
        calls = calls + 1
        if calls == 1 then
          error("first-call-boom")   -- must not kill the reader
        end
        second_ok = (data == "second")
        sock:sendto("ok", host, port)
      end))
    fan.sleep(0.02)
    local c1 = assert(connector.connect("udp://127.0.0.1:" .. PORT))
    c1:send("first")
    fan.sleep(0.1)          -- give the on_accept coroutine time to error
    c1:close()
    local c2 = assert(connector.connect("udp://127.0.0.1:" .. PORT))
    c2:send("second")
    reply = c2:recv()
    c2:close()
  end)
  if server then server:close() end
  -- Both datagrams reached on_accept — the error on the first didn't
  -- tear the reader down. The stderr log from the first pcall is
  -- expected noise; the second call successfully echoes back.
  T.eq(calls, 2)
  T.truthy(second_ok)
  T.eq(reply, "ok")
end)

s:test("udp: IPv6 bracket URL parsing (::1)", function()
  -- Just parse & bind; if the platform doesn't have loopback IPv6 the
  -- bind will fail with a specific error string, which we accept.
  with_loop(function()
    local sock, err = connector.bind("udp://[::1]:24405", function() end)
    if sock then
      sock:close()
    else
      T.is_type(err, "string")
    end
  end)
end)

----------------------------------------------------------------------
-- POPEN dispatch
----------------------------------------------------------------------

s:test("popen:cmd runs a subprocess and pipes stdout via recv", function()
  local proc
  local out = {}
  with_loop(function()
    proc = assert(connector.connect("popen:/bin/echo hello-from-connector"))
    -- fan.popen.spawn without callbacks returns the raw C handle; use recv.
    while true do
      local data, which = proc:recv()
      if data == nil then break end
      if which == "stdout" then out[#out + 1] = data end
    end
  end)
  T.eq(table.concat(out), "hello-from-connector\n")
end)

s:test("popen:///cmd (triple-slash) is equivalent to popen:cmd", function()
  local out = {}
  with_loop(function()
    local proc = assert(connector.connect("popen:///bin/echo world"))
    while true do
      local data, which = proc:recv()
      if data == nil then break end
      if which == "stdout" then out[#out + 1] = data end
    end
  end)
  T.eq(table.concat(out), "world\n")
end)

s:test("popen: connector.bind is not supported (nil, err)", function()
  local b, err = connector.bind("popen:/bin/true", function() end)
  T.is_nil(b); T.not_nil(err)
  T.truthy(tostring(err):find("popen"))
end)

os.exit(T.run(s))
