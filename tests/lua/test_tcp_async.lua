--[[
  test_tcp_async.lua — M17 callback-based fan.tcp.connect_async{...} client.

  M17-1 scope: connect_async happy path + close/reconnect + reject
  {worker=,callback_self_first=} + build-failure ondisconnected + close
  race exactly-once.  timeout / onsendready / SSL / evdns / pkcs12 land
  in M17-2, server side in M17-3.

  All tests run against an in-process echo server bound via the existing
  coroutine-yielding fan.tcp.bind (M17-3 will swap it for bind_async).
]]
local T   = require("test_framework")
local fan = require("fan")
local tcp = fan.tcp

local s = T.suite("fan.tcp.connect_async (M17-1)")

-- Drive a fan.spawn'd body inside fan.loop; the body calls fan.loopbreak
-- when it is done so the loop returns cleanly.
local function with_loop(body)
  fan.spawn(body)
  fan.loop()
end

-- ---------------------------------------------------------------------
-- happy path: onconnected -> send -> onread -> close -> ondisconnected
-- ---------------------------------------------------------------------
s:test("happy path: onconnected -> send -> onread -> ondisconnected", function()
  local PORT = 24301
  local seen = { connected = 0, read = nil, disc = nil }
  local server, conn
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(sc)
      local buf = sc:receive()
      if buf then sc:send("echo:" .. buf) end
      -- Give the client time to receive before we tear down.
      fan.sleep(0.05)
      sc:close()
    end))

    conn = tcp.connect_async{
      host = "127.0.0.1",
      port = PORT,
      onconnected = function(self)
        seen.connected = seen.connected + 1
        self:send("ping")
      end,
      onread = function(self, data)
        seen.read = data
      end,
      ondisconnected = function(self, reason)
        seen.disc = reason
        fan.loopbreak()
      end,
    }
    -- fan.loop keeps spinning until ondisconnected fires loopbreak.
  end)
  if server then server:close() end
  T.eq(seen.connected, 1, "onconnected must fire exactly once")
  T.eq(seen.read, "echo:ping")
  T.not_nil(seen.disc, "ondisconnected must fire")
end)

-- ---------------------------------------------------------------------
-- pre-connect send queue: :send before onconnected is buffered + flushed
-- ---------------------------------------------------------------------
s:test("pre-connect send queue: send() before onconnected is buffered", function()
  local PORT = 24302
  local seen = { got_on_server = nil, disc = nil }
  local server, conn
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(sc)
      -- Read up to 4 bytes so the server-side test is deterministic.
      seen.got_on_server = sc:receive(4)
      sc:close()
    end))

    conn = tcp.connect_async{
      host = "127.0.0.1",
      port = PORT,
      -- Deliberately do NOT set onconnected; we call send() from outside
      -- to prove the buffer-before-connect path.
      ondisconnected = function(self, reason)
        seen.disc = reason
        fan.loopbreak()
      end,
    }
    -- The bufferevent queues both writes; libevent flushes on socket
    -- writable regardless of when :send() was called relative to the
    -- CONNECTED event.
    conn:send("AB")
    conn:send("CD")
  end)
  if server then server:close() end
  T.eq(seen.got_on_server, "ABCD",
       "server must see the two writes in order after connect")
end)

-- ---------------------------------------------------------------------
-- Build failure path: bad port -> ondisconnected(reason) fires eventually,
-- onconnected never fires.
-- ---------------------------------------------------------------------
s:test("connection refused -> ondisconnected fires, onconnected does not", function()
  local seen = { connected = 0, disc = nil }
  local conn
  with_loop(function()
    conn = tcp.connect_async{
      host = "127.0.0.1",
      port = 1,   -- privileged port, essentially guaranteed closed
      onconnected = function() seen.connected = seen.connected + 1 end,
      ondisconnected = function(self, reason)
        seen.disc = reason
        fan.loopbreak()
      end,
    }
  end)
  T.eq(seen.connected, 0, "onconnected must NOT fire on connect failure")
  T.not_nil(seen.disc, "ondisconnected must fire on connect failure")
end)

-- ---------------------------------------------------------------------
-- close() fires ondisconnected(self,"closed") exactly once
-- ---------------------------------------------------------------------
s:test("close() fires ondisconnected(self,\"closed\") exactly once", function()
  local PORT = 24303
  local seen = { connected = 0, disc_count = 0, disc_reason = nil }
  local server, conn
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(sc)
      -- Keep the socket alive until the client tears down.
      fan.sleep(0.3)
      sc:close()
    end))

    conn = tcp.connect_async{
      host = "127.0.0.1",
      port = PORT,
      onconnected = function(self)
        seen.connected = seen.connected + 1
        self:close()   -- close right away; should fire disc exactly once
      end,
      ondisconnected = function(self, reason)
        seen.disc_count = seen.disc_count + 1
        seen.disc_reason = reason
        fan.loopbreak()
      end,
    }
  end)
  if server then server:close() end
  T.eq(seen.connected, 1)
  T.eq(seen.disc_count, 1, "ondisconnected must fire exactly once")
  T.eq(seen.disc_reason, "closed")
end)

-- ---------------------------------------------------------------------
-- close mid-connect: onconnected never fires, ondisconnected fires once.
-- ---------------------------------------------------------------------
s:test("close before onconnected -> onconnected never fires", function()
  local seen = { connected = 0, disc_count = 0 }
  local conn
  with_loop(function()
    -- Point at a real listener we then never accept from — using port 1
    -- means the connect will fail regardless.  Close mid-flight and
    -- assert the state machine still only fires disc once.
    conn = tcp.connect_async{
      host = "127.0.0.1",
      port = 1,
      onconnected = function() seen.connected = seen.connected + 1 end,
      ondisconnected = function(self, reason)
        seen.disc_count = seen.disc_count + 1
        fan.loopbreak()
      end,
    }
    conn:close()   -- immediately close; disc("closed") should be the ONE
    -- If any later event tries to re-fire disc, seen.disc_count would go up.
  end)
  T.eq(seen.connected, 0)
  T.eq(seen.disc_count, 1)
end)

-- ---------------------------------------------------------------------
-- reject unsupported v1 fields
-- ---------------------------------------------------------------------
s:test("worker= raises an error (v2 has no in-process worker bases)", function()
  local ok, err = pcall(tcp.connect_async, {
    host = "127.0.0.1", port = 24304, worker = 0,
  })
  T.eq(ok, false)
  T.truthy(err and err:find("worker", 1, true))
end)

s:test("callback_self_first= raises an error", function()
  local ok, err = pcall(tcp.connect_async, {
    host = "127.0.0.1", port = 24305, callback_self_first = false,
  })
  T.eq(ok, false)
  T.truthy(err and err:find("callback_self_first", 1, true))
end)

-- ---------------------------------------------------------------------
-- reconnect() after ondisconnected
-- ---------------------------------------------------------------------
s:test("reconnect() rebuilds bev and fires onconnected again", function()
  local PORT = 24306
  local seen = { connected = 0, disc_count = 0 }
  local server, conn
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(sc)
      fan.sleep(0.02)
      sc:close()
    end))

    local reconnected = false
    conn = tcp.connect_async{
      host = "127.0.0.1",
      port = PORT,
      onconnected = function(self)
        seen.connected = seen.connected + 1
        -- Do nothing; the server will drop the socket and trigger disc.
      end,
      ondisconnected = function(self, reason)
        seen.disc_count = seen.disc_count + 1
        if not reconnected then
          reconnected = true
          assert(self:reconnect())
        else
          fan.loopbreak()
        end
      end,
    }
  end)
  if server then server:close() end
  T.eq(seen.connected, 2, "onconnected must fire twice (before and after reconnect)")
  T.eq(seen.disc_count, 2, "ondisconnected must fire twice")
end)

-- ---------------------------------------------------------------------
-- fan.tcpd shim: require("fan.tcpd").connect resolves to connect_async
-- ---------------------------------------------------------------------
s:test("require('fan.tcpd').connect === fan.tcp.connect_async", function()
  local tcpd = require("fan.tcpd")
  T.eq(tcpd.connect, tcp.connect_async)
end)

-- ---------------------------------------------------------------------
-- Coverage: exercise the accessor / control methods on the async handle.
-- Each of these is a thin wrapper the M17-1 build otherwise leaves un-hit,
-- pulling the C coverage number below the 85% gate.
-- ---------------------------------------------------------------------
s:test("getpeername / getsockname / shutdown / pause_read / resume_read", function()
  local PORT = 24307
  local peer_ip, peer_port, sock_ip, sock_port
  local server, conn
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(sc)
      fan.sleep(0.1)
      sc:close()
    end))
    conn = tcp.connect_async{
      host = "127.0.0.1",
      port = PORT,
      onconnected = function(self)
        peer_ip, peer_port = self:getpeername()
        sock_ip, sock_port = self:getsockname()
        -- exercise pause / resume / shutdown paths
        self:pause_read()
        self:resume_read()
        self:shutdown()
        self:close()
      end,
      ondisconnected = function(self, reason)
        fan.loopbreak()
      end,
    }
  end)
  if server then server:close() end
  T.eq(peer_ip, "127.0.0.1")
  T.eq(peer_port, PORT)
  T.eq(sock_ip, "127.0.0.1")
  T.truthy(sock_port and sock_port > 0)
end)

s:test("getpeername on closed conn returns (nil, err)", function()
  local conn
  with_loop(function()
    conn = tcp.connect_async{
      host = "127.0.0.1",
      port = 1,
      ondisconnected = function() fan.loopbreak() end,
    }
    conn:close()
  end)
  local ip, port_or_err = conn:getpeername()
  T.eq(ip, nil)
  T.is_type(port_or_err, "string")
end)

s:test("send() on closed conn returns (nil, 'closed')", function()
  local conn
  with_loop(function()
    conn = tcp.connect_async{
      host = "127.0.0.1",
      port = 1,
      ondisconnected = function() fan.loopbreak() end,
    }
    conn:close()
  end)
  local ok, err = conn:send("bytes")
  T.eq(ok, nil)
  T.eq(err, "closed")
end)

s:test("onread absent: incoming bytes are drained silently", function()
  -- With no onread callback the readcb must still drain the input buffer
  -- so libevent does not spin on a readable-but-unread socket.  We can
  -- assert only indirectly: the ondisconnected(reason=eof) fires cleanly
  -- when the server sends bytes then closes.
  local PORT = 24308
  local disc_reason
  local server, conn
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(sc)
      sc:send("noise-that-goes-nowhere")
      fan.sleep(0.05)
      sc:close()
    end))
    conn = tcp.connect_async{
      host = "127.0.0.1",
      port = PORT,
      -- deliberately no onread
      ondisconnected = function(self, reason)
        disc_reason = reason
        fan.loopbreak()
      end,
    }
  end)
  if server then server:close() end
  T.not_nil(disc_reason)  -- either "eof" (clean close) or "closed" if we raced
end)

s:test("reconnect() on a closed conn returns (nil, err)", function()
  local conn
  with_loop(function()
    conn = tcp.connect_async{
      host = "127.0.0.1",
      port = 1,
      ondisconnected = function() fan.loopbreak() end,
    }
    conn:close()
  end)
  local ok, err = conn:reconnect()
  T.eq(ok, nil)
  T.is_type(err, "string")
end)

os.exit(T.run(s))
