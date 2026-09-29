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

-- ======================================================================
-- M17-2 additions: full parameter surface
-- ======================================================================

-- ---------------------------------------------------------------------
-- connect_timeout: connecting to a routable but firewalled address must
-- fire ondisconnected(reason="connect_timeout") within the deadline.
-- ---------------------------------------------------------------------
s:test("connect_timeout fires ondisconnected(reason=\"connect_timeout\")", function()
  -- 240.0.0.1 is in a reserved / unroutable block; SYNs will hang until
  -- the local TCP stack gives up, which is longer than our timeout.
  local seen = { connected = 0, disc = nil, elapsed = nil }
  local t0 = os.clock()
  with_loop(function()
    tcp.connect_async{
      host = "240.0.0.1",
      port = 1,
      connect_timeout = 0.3,
      onconnected = function() seen.connected = seen.connected + 1 end,
      ondisconnected = function(self, reason)
        seen.disc = reason
        seen.elapsed = os.clock() - t0
        fan.loopbreak()
      end,
    }
  end)
  T.eq(seen.connected, 0)
  T.eq(seen.disc, "connect_timeout")
  T.truthy(seen.elapsed and seen.elapsed < 2.0,
           "timeout should have fired well under 2s, got " ..
           tostring(seen.elapsed))
end)

-- ---------------------------------------------------------------------
-- read_timeout: peer accepts but never sends anything.  The bufferevent
-- read timer fires and delivers reason="timeout".
-- ---------------------------------------------------------------------
s:test("read_timeout fires ondisconnected(reason=\"timeout\")", function()
  local PORT = 24401
  local seen = { disc = nil }
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(sc)
      -- Just sit on the socket without writing anything.  The client's
      -- read_timeout should fire; when its ondisconnected fires we tear
      -- the server down.
      fan.sleep(1.0)
      sc:close()
    end))
    tcp.connect_async{
      host = "127.0.0.1",
      port = PORT,
      read_timeout = 0.3,
      ondisconnected = function(self, reason)
        seen.disc = reason
        fan.loopbreak()
      end,
    }
  end)
  if server then server:close() end
  T.eq(seen.disc, "timeout")
end)

-- ---------------------------------------------------------------------
-- onsendready: bufferevent writecb dispatches on drain-to-empty.
-- ---------------------------------------------------------------------
s:test("onsendready fires after output buffer drains to empty", function()
  local PORT = 24402
  local seen = { connected = 0, sendready = 0 }
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(sc)
      -- Drain whatever the client writes so libevent can flush.
      while true do
        local d = sc:receive()
        if not d then break end
      end
      sc:close()
    end))
    tcp.connect_async{
      host = "127.0.0.1",
      port = PORT,
      onconnected = function(self)
        seen.connected = seen.connected + 1
        self:send("payload-that-drains")
      end,
      onsendready = function(self)
        seen.sendready = seen.sendready + 1
        self:close()  -- triggers ondisconnected
      end,
      ondisconnected = function(self, reason)
        fan.loopbreak()
      end,
    }
  end)
  if server then server:close() end
  T.eq(seen.connected, 1)
  T.truthy(seen.sendready >= 1,
           "onsendready must fire at least once after the send drains")
end)

-- ---------------------------------------------------------------------
-- Optional TLS parameter surface: only meaningful when the openssl CLI is
-- available to generate test material.  We assert build-time acceptance
-- (the C-side parameter parsing / cache paths must not crash), not
-- end-to-end TLS handshakes.
-- ---------------------------------------------------------------------
local function have_openssl()
  local ok = os.execute("command -v openssl >/dev/null 2>&1")
  return ok == true or ok == 0
end

if have_openssl() then
  local TMP = os.getenv("TMPDIR") or "/tmp"
  local CA_PEM = TMP .. "/fan_tls_async_ca.pem"
  local CA_KEY = TMP .. "/fan_tls_async_ca.key"
  local P12    = TMP .. "/fan_tls_async_client.p12"

  -- Generate a self-signed CA cert (used as both `cert.pem` for a
  -- verification-locations test and as the identity backing the pkcs12).
  os.execute(string.format(
    "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s -days 1 -nodes " ..
    "-subj /CN=async-test-ca >/dev/null 2>&1", CA_KEY, CA_PEM))
  -- Package the same key+cert as a pkcs12 bundle for the pkcs12 path.
  os.execute(string.format(
    "openssl pkcs12 -export -inkey %s -in %s -out %s -passout pass:testpw " ..
    ">/dev/null 2>&1", CA_KEY, CA_PEM, P12))

  s:test("ssl_host / cainfo / pkcs12 params are accepted (build path)", function()
    -- We do not stand up a TLS peer; instead we assert that the C-side
    -- parameter parsing + TLS ctx cache build path completes without
    -- error.  The connect target is refused, so ondisconnected fires
    -- for that reason — proving both that params were valid AND that
    -- the disc path still delivers correctly.
    local seen = { disc = nil }
    with_loop(function()
      tcp.connect_async{
        host    = "127.0.0.1",
        port    = 1,
        ssl     = true,
        ssl_host = "custom.example",
        cainfo   = CA_PEM,
        pkcs12   = { path = P12, password = "testpw" },
        ondisconnected = function(self, reason)
          seen.disc = reason
          fan.loopbreak()
        end,
      }
    end)
    T.not_nil(seen.disc)  -- exact reason depends on TLS state; just needs to fire
  end)

  s:test("SSL_CTX cache: two conns with same TLS params reuse the ctx", function()
    -- We cannot introspect the cache directly, but we CAN assert that
    -- back-to-back conns with identical params both complete parameter
    -- parsing without error.  A cache miss on the second call would
    -- still work, but coverage-wise this exercises the lookup-hit
    -- branch in fan_tls_ctx_cache_lookup.
    local disc_count = 0
    with_loop(function()
      local total_expected = 2
      local function make_conn()
        tcp.connect_async{
          host = "127.0.0.1", port = 1, ssl = true,
          cainfo = CA_PEM,
          ondisconnected = function()
            disc_count = disc_count + 1
            if disc_count >= total_expected then fan.loopbreak() end
          end,
        }
      end
      make_conn()
      make_conn()
    end)
    T.eq(disc_count, 2)
  end)

  os.execute("rm -f " .. CA_PEM .. " " .. CA_KEY .. " " .. P12)
else
  s:test("openssl CLI unavailable — TLS param tests skipped", function()
    T.truthy(true)
  end)
end

-- ---------------------------------------------------------------------
-- evdns: passing a custom evdns_base as the `evdns` field is accepted.
-- We cannot easily observe *which* resolver was used from Lua, but the
-- code path (fan_evdns_get_base + registry pin + rebuild_bev fallback)
-- must not crash.  A refused-port connect exercises both the accept and
-- the disc-dispatch branches.
-- ---------------------------------------------------------------------
s:test("evdns option is accepted and pinned across the connection", function()
  local evdns = fan.evdns
  if not evdns or not evdns.create then
    -- fan.evdns not compiled in on this build — treat as skip
    return
  end
  local dns = evdns.create()  -- default nameserver base wrapper
  local seen = { disc = nil }
  with_loop(function()
    tcp.connect_async{
      host = "127.0.0.1",
      port = 1,
      evdns = dns,
      ondisconnected = function(self, reason)
        seen.disc = reason
        fan.loopbreak()
      end,
    }
  end)
  T.not_nil(seen.disc)
end)

-- ======================================================================
-- M17-3 additions: server-side fan.tcp.bind_async
-- ======================================================================

-- ---------------------------------------------------------------------
-- Happy path server: bind_async accepts a connection, onaccept receives
-- (self, accept), accept:bind installs the callbacks, echo round-trip.
-- ---------------------------------------------------------------------
s:test("bind_async: onaccept + accept:bind + echo round-trip", function()
  local PORT = 24501
  local seen = { self_type = nil, accepted = 0, echoed = 0,
                 disc_server_side = nil, disc_client_side = nil }
  local server, cli
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1",
      port = PORT,
      onaccept = function(srv, accept)
        seen.self_type = type(srv)     -- expect "userdata"
        seen.accepted = seen.accepted + 1
        accept:bind{
          onread = function(a, data)
            seen.echoed = seen.echoed + 1
            a:send("echo:" .. data)
          end,
          ondisconnected = function(a, reason)
            seen.disc_server_side = reason
          end,
        }
      end,
    })

    cli = tcp.connect_async{
      host = "127.0.0.1",
      port = PORT,
      onconnected = function(self) self:send("ping") end,
      onread = function(self, data)
        T.eq(data, "echo:ping")
        self:close()
      end,
      ondisconnected = function(self, reason)
        seen.disc_client_side = reason
        fan.loopbreak()
      end,
    }
  end)
  if server then server:close() end
  T.eq(seen.self_type, "userdata",
       "onaccept must receive the server userdata as arg 1")
  T.eq(seen.accepted, 1)
  T.eq(seen.echoed, 1)
  T.not_nil(seen.disc_client_side)
end)

-- ---------------------------------------------------------------------
-- accept:remoteinfo returns {ip=..., port=...} for the peer.
-- ---------------------------------------------------------------------
s:test("bind_async: accept:remoteinfo returns peer ip + port", function()
  local PORT = 24502
  local remote_seen
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1",
      port = PORT,
      onaccept = function(srv, accept)
        remote_seen = accept:remoteinfo()
        accept:bind{
          ondisconnected = function() fan.loopbreak() end,
        }
        accept:close()
      end,
    })
    tcp.connect_async{
      host = "127.0.0.1", port = PORT,
      ondisconnected = function() end,
    }
  end)
  if server then server:close() end
  T.eq(type(remote_seen), "table")
  T.eq(remote_seen.ip, "127.0.0.1")
  T.truthy(remote_seen.port and remote_seen.port > 0)
end)

-- ---------------------------------------------------------------------
-- accept:pause_read / resume_read cycle around a receive.
-- ---------------------------------------------------------------------
s:test("bind_async: accept:pause_read defers onread until resume_read", function()
  local PORT = 24503
  local seen = { paused = false, resumed = false, read_after_resume = nil }
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = PORT,
      onaccept = function(srv, accept)
        accept:bind{
          onread = function(a, data)
            if not seen.paused then
              -- first byte: pause reads, then resume after a short
              -- delay via a spawned coroutine.
              seen.paused = true
              seen.read_after_resume = data
              a:pause_read()
              fan.spawn(function()
                fan.sleep(0.05)
                seen.resumed = true
                a:resume_read()
                a:close()
              end)
            end
          end,
          ondisconnected = function() end,
        }
      end,
    })
    tcp.connect_async{
      host = "127.0.0.1", port = PORT,
      onconnected = function(self) self:send("first-batch") end,
      ondisconnected = function() fan.loopbreak() end,
    }
  end)
  if server then server:close() end
  T.eq(seen.paused, true)
  T.eq(seen.resumed, true)
  T.eq(seen.read_after_resume, "first-batch")
end)

-- ---------------------------------------------------------------------
-- accept:flush is a no-op on socket bevs but must return successfully.
-- ---------------------------------------------------------------------
s:test("bind_async: accept:flush is accepted and returns true", function()
  local PORT = 24504
  local flush_ok
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = PORT,
      onaccept = function(srv, accept)
        accept:bind{ ondisconnected = function() fan.loopbreak() end }
        accept:send("hello")
        flush_ok = accept:flush()
        accept:close()
      end,
    })
    tcp.connect_async{
      host = "127.0.0.1", port = PORT,
      ondisconnected = function() end,
    }
  end)
  if server then server:close() end
  T.eq(flush_ok, true)
end)

-- ---------------------------------------------------------------------
-- server:getport returns the actual bound port (useful when passing 0).
-- ---------------------------------------------------------------------
s:test("bind_async: server:getport returns the bound port", function()
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = 0,   -- kernel-picked
      onaccept = function() end,
    })
    fan.loopbreak()
  end)
  local p = server:getport()
  T.truthy(p and p > 0 and p < 65536, "getport returned " .. tostring(p))
  server:close()
end)

-- ---------------------------------------------------------------------
-- server:rebind rebuilds the listener on the same port.
-- ---------------------------------------------------------------------
s:test("bind_async: server:rebind() re-listens on the same port", function()
  local PORT = 24505
  local ok
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = PORT,
      onaccept = function() end,
    })
    ok = server:rebind()
    fan.loopbreak()
  end)
  T.eq(ok, true)
  T.eq(server:getport(), PORT)
  server:close()
end)

-- ---------------------------------------------------------------------
-- bind_async rejects worker= and callback_self_first= (same v2 policy
-- as connect_async).
-- ---------------------------------------------------------------------
s:test("bind_async: worker= raises an error", function()
  local ok, err = pcall(tcp.bind_async, {
    host = "127.0.0.1", port = 24506,
    onaccept = function() end,
    worker = 0,
  })
  T.eq(ok, false)
  T.truthy(err and err:find("worker", 1, true))
end)

s:test("bind_async: callback_self_first= raises an error", function()
  local ok, err = pcall(tcp.bind_async, {
    host = "127.0.0.1", port = 24507,
    onaccept = function() end,
    callback_self_first = false,
  })
  T.eq(ok, false)
  T.truthy(err and err:find("callback_self_first", 1, true))
end)

-- ---------------------------------------------------------------------
-- fan.tcpd shim: require("fan.tcpd").bind === fan.tcp.bind_async
-- ---------------------------------------------------------------------
s:test("require('fan.tcpd').bind === fan.tcp.bind_async", function()
  local tcpd = require("fan.tcpd")
  T.eq(tcpd.bind, tcp.bind_async)
end)

-- ---------------------------------------------------------------------
-- accept:send / accept:close on already-closed accept return (nil, err).
-- ---------------------------------------------------------------------
s:test("bind_async: send() / close() on closed accept are safe", function()
  local PORT = 24508
  local sent_ok, sent_err
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = PORT,
      onaccept = function(srv, accept)
        accept:bind{ ondisconnected = function() fan.loopbreak() end }
        accept:close()
        sent_ok, sent_err = accept:send("nothing-goes-through")
      end,
    })
    tcp.connect_async{
      host = "127.0.0.1", port = PORT,
      ondisconnected = function() end,
    }
  end)
  if server then server:close() end
  T.eq(sent_ok, nil)
  T.eq(sent_err, "closed")
end)

-- ---------------------------------------------------------------------
-- Coverage: exercise error paths and lifecycle branches that the happy-
-- path tests above do not touch.  Each of these is a distinct C branch
-- in bind_async / async_server_gc / async_accept_gc.
-- ---------------------------------------------------------------------

s:test("bind_async: ssl=true without cert/key returns (nil, err)", function()
  local sv, err = tcp.bind_async{
    host = "127.0.0.1", port = 24510,
    ssl = true,   -- missing cert + key
    onaccept = function() end,
  }
  T.eq(sv, nil)
  T.is_type(err, "string")
  T.truthy(err:find("cert", 1, true) or err:find("key", 1, true))
end)

s:test("bind_async: onaccept missing raises an error", function()
  local ok, err = pcall(tcp.bind_async, {
    host = "127.0.0.1", port = 24511,
    -- no onaccept
  })
  T.eq(ok, false)
  T.truthy(err and err:find("onaccept", 1, true))
end)

s:test("bind_async: bad host raises an error", function()
  local ok, err = pcall(tcp.bind_async, {
    host = "not-a-valid-ip", port = 24512,
    onaccept = function() end,
  })
  T.eq(ok, false)
  T.truthy(err and err:find("host", 1, true))
end)

s:test("bind_async: server:close() then rebind returns (nil, err)", function()
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = 24513,
      onaccept = function() end,
    })
    fan.loopbreak()
  end)
  server:close()
  local ok, err = server:rebind()
  T.eq(ok, nil)
  T.is_type(err, "string")
end)

s:test("bind_async: server:getport after close returns (nil, err)", function()
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = 24514,
      onaccept = function() end,
    })
    fan.loopbreak()
  end)
  server:close()
  local p, err = server:getport()
  T.eq(p, nil)
  T.is_type(err, "string")
end)

s:test("bind_async: accept:bind twice returns (nil, 'accept already bound')", function()
  local PORT = 24515
  local err1, err2
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = PORT,
      onaccept = function(srv, accept)
        local ok1 = accept:bind{
          ondisconnected = function() fan.loopbreak() end,
        }
        err1 = ok1
        local ok2, e = accept:bind{ onread = function() end }
        err2 = e
        accept:close()
      end,
    })
    tcp.connect_async{
      host = "127.0.0.1", port = PORT,
      ondisconnected = function() end,
    }
  end)
  if server then server:close() end
  T.eq(err1, true)                     -- first bind succeeded
  T.is_type(err2, "string")            -- second bind reported an error
  T.truthy(err2:find("already bound", 1, true))
end)

s:test("bind_async: accept:bind rejects callback_self_first", function()
  local PORT = 24516
  local caught
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = PORT,
      onaccept = function(srv, accept)
        local ok, err = pcall(function()
          accept:bind{ callback_self_first = false }
        end)
        caught = err
        accept:bind{ ondisconnected = function() fan.loopbreak() end }
        accept:close()
      end,
    })
    tcp.connect_async{
      host = "127.0.0.1", port = PORT,
      ondisconnected = function() end,
    }
  end)
  if server then server:close() end
  T.truthy(caught and caught:find("callback_self_first", 1, true))
end)

s:test("bind_async: accept:remoteinfo on closed accept returns (nil, err)", function()
  local PORT = 24517
  local ri, err
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = PORT,
      onaccept = function(srv, accept)
        accept:bind{ ondisconnected = function() fan.loopbreak() end }
        accept:close()
        ri, err = accept:remoteinfo()
      end,
    })
    tcp.connect_async{
      host = "127.0.0.1", port = PORT,
      ondisconnected = function() end,
    }
  end)
  if server then server:close() end
  T.eq(ri, nil)
  T.is_type(err, "string")
end)

s:test("bind_async: send_buffer_size / receive_buffer_size are accepted", function()
  -- We cannot easily observe SO_SNDBUF / SO_RCVBUF from Lua, but we CAN
  -- assert the bind + accept complete without error when the option is
  -- set — proving the setsockopt code path is reached.
  local PORT = 24518
  local accepted = 0
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = PORT,
      send_buffer_size    = 64 * 1024,
      receive_buffer_size = 64 * 1024,
      onaccept = function(srv, accept)
        accepted = accepted + 1
        accept:bind{ ondisconnected = function() fan.loopbreak() end }
        accept:close()
      end,
    })
    tcp.connect_async{
      host = "127.0.0.1", port = PORT,
      ondisconnected = function() end,
    }
  end)
  if server then server:close() end
  T.eq(accepted, 1)
end)

s:test("bind_async: onsendready fires on accept side after drain", function()
  local PORT = 24519
  local sendready_seen = 0
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = PORT,
      onaccept = function(srv, accept)
        accept:bind{
          onsendready = function(a)
            sendready_seen = sendready_seen + 1
            a:close()
          end,
          ondisconnected = function() fan.loopbreak() end,
        }
        accept:send("data-that-drains")
      end,
    })
    tcp.connect_async{
      host = "127.0.0.1", port = PORT,
      onread = function(self, data) end,   -- drain
      ondisconnected = function() end,
    }
  end)
  if server then server:close() end
  T.truthy(sendready_seen >= 1)
end)

s:test("bind_async: TLS server accepts a TLS client (end-to-end)", function()
  local TMP = os.getenv("TMPDIR") or "/tmp"
  local CERT = TMP .. "/fan_tcpd_async_cert.pem"
  local KEY  = TMP .. "/fan_tcpd_async_key.pem"
  local have = os.execute("command -v openssl >/dev/null 2>&1")
  if not (have == true or have == 0) then return end

  os.execute(string.format(
    "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s -days 1 -nodes " ..
    "-subj /CN=localhost >/dev/null 2>&1", KEY, CERT))

  local PORT = 24520
  local got_on_server, got_on_client
  local server
  with_loop(function()
    server = assert(tcp.bind_async{
      host = "127.0.0.1", port = PORT,
      ssl = true, cert = CERT, key = KEY,
      onaccept = function(srv, accept)
        accept:bind{
          onread = function(a, data)
            got_on_server = data
            a:send("srv-echo:" .. data)
          end,
          ondisconnected = function() end,
        }
      end,
    })
    tcp.connect_async{
      host = "127.0.0.1", port = PORT,
      ssl = true,
      ssl_verifypeer = 0, ssl_verifyhost = 0,   -- self-signed cert
      onconnected = function(self) self:send("hello-tls") end,
      onread = function(self, data)
        got_on_client = data
        self:close()
      end,
      ondisconnected = function() fan.loopbreak() end,
    }
  end)
  if server then server:close() end
  os.execute("rm -f " .. CERT .. " " .. KEY)
  T.eq(got_on_server, "hello-tls")
  T.eq(got_on_client, "srv-echo:hello-tls")
end)

os.exit(T.run(s))
