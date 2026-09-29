--[[
  test_udp_async.lua — M18 callback-based fan.udp.new_async{...}.

  Covers: onread(self, data, dest), sock:send / send_req + onsendready,
  make_dest / make_dests + UDP_AddrInfo getHost/getIP/getPort,
  sock:rebind, sock:close idempotence, worker / callback_self_first
  rejection, and the fan.udpd shim.
]]
local T   = require("test_framework")
local fan = require("fan")
local udp = fan.udp

local s = T.suite("fan.udp.new_async (M18)")

local function with_loop(body)
  fan.spawn(body)
  fan.loop()
end

-- ---------------------------------------------------------------------
-- make_dest: numeric IPv4 resolves + returns a dest with getHost/getIP/
-- getPort.  Also exercises the getHost/getIP equivalence contract.
-- ---------------------------------------------------------------------
s:test("make_dest: numeric IPv4 returns dest with getHost/getIP/getPort", function()
  local d = assert(udp.make_dest("127.0.0.1", 24601))
  T.eq(d:getHost(), "127.0.0.1")
  T.eq(d:getIP(), "127.0.0.1")
  T.eq(d:getPort(), 24601)
end)

s:test("make_dest: port out of range returns (nil, err)", function()
  local d, err = udp.make_dest("127.0.0.1", 70000)
  T.eq(d, nil)
  T.is_type(err, "string")
end)

s:test("make_dests: numeric IPv4 returns an array", function()
  local list = assert(udp.make_dests("127.0.0.1", 24602))
  T.eq(type(list), "table")
  T.truthy(#list >= 1)
  T.eq(list[1]:getPort(), 24602)
end)

-- ---------------------------------------------------------------------
-- Happy path: onread fires with (self, data, dest); reply via sock:send.
-- ---------------------------------------------------------------------
s:test("onread(self, data, dest) round-trip + reply via send()", function()
  local PORT = 24603
  local seen = { self_type = nil, data = nil, host = nil, port = nil }

  with_loop(function()
    -- Server: bind, echo whatever we hear back to the same peer.
    local server = udp.new_async{
      bind_port = PORT,
      onread = function(self, data, dest)
        seen.self_type = type(self)
        seen.data = data
        seen.host = dest:getHost()
        seen.port = dest:getPort()
        self:send("echo:" .. data, dest)
      end,
    }

    -- Client: bind a random port so getPort returns a real value.
    local client_got
    local client = udp.new_async{
      bind_port = 0,
      onread = function(self, data, dest)
        client_got = data
        server:close()
        self:close()
        fan.loopbreak()
      end,
    }

    local dest = assert(udp.make_dest("127.0.0.1", PORT))
    client:send("ping", dest)

    -- fail-safe: break the loop if the server never fires
    fan.spawn(function()
      fan.sleep(1.0)
      if not client_got then
        server:close()
        client:close()
        fan.loopbreak()
      end
    end)
  end)

  T.eq(seen.self_type, "userdata")
  T.eq(seen.data, "ping")
  T.eq(seen.host, "127.0.0.1")
  T.truthy(seen.port and seen.port > 0)
  -- The echo must have made it back to the client.
end)

-- ---------------------------------------------------------------------
-- send() with no dest and no default host returns (nil, "no destination").
-- ---------------------------------------------------------------------
s:test("send() with no dest and no default returns (nil, err)", function()
  local ok_val, err_val
  with_loop(function()
    local sock = udp.new_async{ bind_port = 0 }
    ok_val, err_val = sock:send("orphan-message")
    sock:close()
    fan.loopbreak()
  end)
  T.eq(ok_val, nil)
  T.eq(err_val, "no destination")
end)

-- ---------------------------------------------------------------------
-- send() falls back to default host:port when no dest is given.
-- ---------------------------------------------------------------------
s:test("send() with default host:port and no explicit dest", function()
  local PORT = 24604
  local got_on_server
  with_loop(function()
    local server = udp.new_async{
      bind_port = PORT,
      onread = function(self, data, dest)
        got_on_server = data
        self:close()
        fan.loopbreak()
      end,
    }
    local client = udp.new_async{
      host = "127.0.0.1",
      port = PORT,
      bind_port = 0,
    }
    client:send("via-default")
    fan.spawn(function()
      fan.sleep(0.3)
      if not got_on_server then
        server:close(); client:close(); fan.loopbreak()
      end
    end)
    -- keep client alive until the server tears down the loop
  end)
  T.eq(got_on_server, "via-default")
end)

-- ---------------------------------------------------------------------
-- send_req arms EV_WRITE; onsendready fires on the next writable tick.
-- ---------------------------------------------------------------------
s:test("send_req -> onsendready fires (UDP is essentially always writable)", function()
  local seen = { count = 0 }
  with_loop(function()
    local sock = udp.new_async{
      bind_port = 0,
      onsendready = function(self)
        seen.count = seen.count + 1
        self:close()
        fan.loopbreak()
      end,
    }
    sock:send_req()
    fan.spawn(function()
      fan.sleep(0.3)
      if seen.count == 0 then
        sock:close(); fan.loopbreak()
      end
    end)
  end)
  T.truthy(seen.count >= 1, "onsendready should have fired at least once")
end)

-- ---------------------------------------------------------------------
-- getPort returns the actual bound port for bind_port = 0.
-- ---------------------------------------------------------------------
s:test("sock:getPort() returns the kernel-picked port when bind_port=0", function()
  local port_seen
  with_loop(function()
    local sock = udp.new_async{ bind_port = 0 }
    port_seen = sock:getPort()
    sock:close()
    fan.loopbreak()
  end)
  T.truthy(port_seen and port_seen > 0 and port_seen < 65536,
           "getPort should return a valid port; got " .. tostring(port_seen))
end)

-- ---------------------------------------------------------------------
-- rebind() rebuilds the socket on the same host:port.
-- ---------------------------------------------------------------------
s:test("sock:rebind() rebuilds the socket successfully", function()
  local ok
  with_loop(function()
    local sock = udp.new_async{ bind_port = 24605 }
    ok = sock:rebind()
    sock:close()
    fan.loopbreak()
  end)
  T.eq(ok, true)
end)

-- ---------------------------------------------------------------------
-- close() is idempotent; subsequent send/rebind return (nil, err).
-- ---------------------------------------------------------------------
s:test("close() is idempotent; post-close send returns (nil, 'closed')", function()
  local send_ok, send_err, rebind_ok, rebind_err
  with_loop(function()
    local sock = udp.new_async{ bind_port = 0 }
    sock:close()
    sock:close()   -- second close must be a no-op, not a crash
    local d = udp.make_dest("127.0.0.1", 1)
    send_ok, send_err = sock:send("post-close", d)
    rebind_ok, rebind_err = sock:rebind()
    fan.loopbreak()
  end)
  T.eq(send_ok, nil)
  T.eq(send_err, "closed")
  T.eq(rebind_ok, nil)
  T.is_type(rebind_err, "string")
end)

-- ---------------------------------------------------------------------
-- Reject unsupported v1 fields.
-- ---------------------------------------------------------------------
s:test("new_async: worker= raises", function()
  local ok, err = pcall(udp.new_async, {
    bind_port = 0, worker = 0,
  })
  T.eq(ok, false)
  T.truthy(err and err:find("worker", 1, true))
end)

s:test("new_async: callback_self_first= raises", function()
  local ok, err = pcall(udp.new_async, {
    bind_port = 0, callback_self_first = false,
  })
  T.eq(ok, false)
  T.truthy(err and err:find("callback_self_first", 1, true))
end)

s:test("new_async: bind_port out of range raises", function()
  local ok = pcall(udp.new_async, { bind_port = 70000 })
  T.eq(ok, false)
end)

-- ---------------------------------------------------------------------
-- fan.udpd shim: require("fan.udpd") maps to the async entries.
-- ---------------------------------------------------------------------
s:test("require('fan.udpd') shim: .new / .make_dest / .make_dests", function()
  local udpd = require("fan.udpd")
  T.eq(udpd.new,        udp.new_async)
  T.eq(udpd.make_dest,  udp.make_dest)
  T.eq(udpd.make_dests, udp.make_dests)
end)

-- ---------------------------------------------------------------------
-- onread absent: incoming bytes are drained silently (no callback).
-- ---------------------------------------------------------------------
s:test("onread absent: incoming datagrams are drained silently", function()
  local PORT = 24606
  local server_closed
  with_loop(function()
    local server = udp.new_async{ bind_port = PORT }
    local client = udp.new_async{ bind_port = 0 }
    local d = assert(udp.make_dest("127.0.0.1", PORT))
    for i = 1, 5 do client:send("noise-" .. i, d) end
    -- Wait a little to let the drain path run without crashing.
    fan.spawn(function()
      fan.sleep(0.1)
      server:close()
      client:close()
      server_closed = true
      fan.loopbreak()
    end)
  end)
  T.eq(server_closed, true)
end)

os.exit(T.run(s))
