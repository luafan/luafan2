--[[
  test_reliable_udp.lua — M3 reliable UDP transport contract tests.

  Two flavours of harness:
    * Real loopback: two fan.udp sockets, full pump/tick machinery, verifying
      genuine send/recv/fragment/reassemble over the OS UDP stack.
    * Lossy in-process link: a mock socket that connects two registries and can
      deterministically drop / reorder / duplicate datagrams, so retransmission,
      out-of-order reassembly and duplicate suppression are exercised without
      relying on the network to misbehave.
]]
local T = require("test_framework")
local fan = require("fan")
local ru = require("fan.reliable_udp")

local s = T.suite("fan.reliable_udp (M3)")

----------------------------------------------------------------------
-- unit: wrap-safe sequence comparison
----------------------------------------------------------------------
s:test("seq_gt handles u32 wraparound", function()
  T.truthy(ru.seq_gt(5, 3))
  T.falsy(ru.seq_gt(3, 5))
  T.falsy(ru.seq_gt(3, 3))
  -- near the wrap boundary: 1 is "after" 0xFFFFFFFF
  T.truthy(ru.seq_gt(1, 0xFFFFFFFF))
  T.falsy(ru.seq_gt(0xFFFFFFFF, 1))
end)

----------------------------------------------------------------------
-- real loopback helpers
----------------------------------------------------------------------
local function run(body)
  fan.spawn(function()
    local ok, err = pcall(body)
    fan.loopbreak()
    if not ok then error(err, 0) end
  end)
  fan.loop()
end

s:test("small message reliable round-trip over loopback", function()
  local PORT = 25401
  local got
  run(function()
    local srv_sock = assert(fan.udp.new("127.0.0.1", PORT))
    local srv = ru.new_registry(srv_sock, { rto = 0.1 })
    local cli_sock = assert(fan.udp.new())
    local cli = ru.new_registry(cli_sock, { rto = 0.1 })

    fan.spawn(function() srv:pump() end)
    fan.spawn(function() cli:pump() end)
    fan.spawn(function() srv:run_ticker(0.05) end)
    fan.spawn(function() cli:run_ticker(0.05) end)

    -- server echoes the first message it receives from any peer
    local sess_srv
    fan.spawn(function()
      -- wait for the peer session to appear
      while not sess_srv do
        for _, ss in pairs(srv.sessions) do sess_srv = ss break end
        if not sess_srv then fan.sleep(0.02) end
      end
    end)

    local c = cli:session("127.0.0.1", PORT)
    assert(c:send("ping"))
    -- give the server a moment, then read on the server side
    fan.spawn(function()
      -- find/create the inbound session and read
    end)

    -- server-side read
    local reader_done = false
    fan.spawn(function()
      -- poll for the inbound session, then recv one message
      local sess
      while not sess do
        for _, ss in pairs(srv.sessions) do sess = ss break end
        if not sess then fan.sleep(0.02) end
      end
      got = sess:recv()
      reader_done = true
    end)

    -- wait until read completes (bounded)
    local deadline = fan.gettime() + 3
    while not reader_done and fan.gettime() < deadline do fan.sleep(0.02) end
    srv:stop(); cli:stop()
    srv_sock:close(); cli_sock:close()
  end)
  T.eq(got, "ping")
end)

s:test("large message fragmentation + reassembly over loopback", function()
  local PORT = 25402
  local payload = string.rep("ABCDEFGH", 5000)  -- 40 KB, many fragments
  local got
  run(function()
    local srv_sock = assert(fan.udp.new("127.0.0.1", PORT))
    local srv = ru.new_registry(srv_sock, { rto = 0.1, mtu = 600 })
    local cli_sock = assert(fan.udp.new())
    local cli = ru.new_registry(cli_sock, { rto = 0.1, mtu = 600 })

    fan.spawn(function() srv:pump() end)
    fan.spawn(function() cli:pump() end)
    fan.spawn(function() srv:run_ticker(0.03) end)
    fan.spawn(function() cli:run_ticker(0.03) end)

    local reader_done = false
    fan.spawn(function()
      local sess
      while not sess do
        for _, ss in pairs(srv.sessions) do sess = ss break end
        if not sess then fan.sleep(0.02) end
      end
      got = sess:recv()
      reader_done = true
    end)

    local c = cli:session("127.0.0.1", PORT)
    assert(c:send(payload))

    local deadline = fan.gettime() + 5
    while not reader_done and fan.gettime() < deadline do fan.sleep(0.02) end
    srv:stop(); cli:stop()
    srv_sock:close(); cli_sock:close()
  end)
  T.truthy(got ~= nil)
  T.eq(got and #got, #payload)
  T.eq(got, payload)
end)

s:test("multiple messages preserve order over loopback", function()
  local PORT = 25403
  local got = {}
  run(function()
    local srv_sock = assert(fan.udp.new("127.0.0.1", PORT))
    local srv = ru.new_registry(srv_sock, { rto = 0.1 })
    local cli_sock = assert(fan.udp.new())
    local cli = ru.new_registry(cli_sock, { rto = 0.1 })

    fan.spawn(function() srv:pump() end)
    fan.spawn(function() cli:pump() end)
    fan.spawn(function() srv:run_ticker(0.03) end)
    fan.spawn(function() cli:run_ticker(0.03) end)

    fan.spawn(function()
      local sess
      while not sess do
        for _, ss in pairs(srv.sessions) do sess = ss break end
        if not sess then fan.sleep(0.02) end
      end
      for _ = 1, 5 do got[#got + 1] = sess:recv() end
    end)

    local c = cli:session("127.0.0.1", PORT)
    for i = 1, 5 do assert(c:send("m" .. i)) end

    local deadline = fan.gettime() + 4
    while #got < 5 and fan.gettime() < deadline do fan.sleep(0.02) end
    srv:stop(); cli:stop()
    srv_sock:close(); cli_sock:close()
  end)
  T.eq(#got, 5)
  for i = 1, 5 do T.eq(got[i], "m" .. i) end
end)

----------------------------------------------------------------------
-- lossy in-process link: deterministic drop / reorder / duplicate
----------------------------------------------------------------------
-- A mock socket whose sendto() feeds a shared switch. The switch routes frames
-- to the destination registry's session dispatch, applying a policy that can
-- drop, delay-reorder or duplicate frames. This isolates protocol behaviour
-- from real UDP timing.
local function make_link(policy)
  local link = { queues = {}, policy = policy or {}, seqno = 0 }
  -- endpoints keyed by "host:port"
  function link:register(host, port, reg)
    self.queues[host .. ":" .. port] = { reg = reg }
  end
  -- mock socket for endpoint (host,port) that delivers to peers via the switch
  function link:socket(self_host, self_port)
    local sw = self
    return {
      sendto = function(_, frame, dst_host, dst_port)
        sw.seqno = sw.seqno + 1
        local n = sw.seqno
        local ep = sw.queues[dst_host .. ":" .. dst_port]
        if not ep then return true end
        local deliver = function()
          local reg = ep.reg
          local sess = reg:session(self_host, self_port)
          sess:_on_frame(frame)
        end
        -- policy hooks
        if sw.policy.drop and sw.policy.drop(n) then
          return true  -- dropped; sender must retransmit
        end
        if sw.policy.dup and sw.policy.dup(n) then
          fan.spawn(deliver); fan.spawn(deliver)
          return true
        end
        if sw.policy.delay and sw.policy.delay(n) then
          fan.spawn(function() fan.sleep(0.05); deliver() end)
          return true
        end
        fan.spawn(deliver)
        return true
      end,
      close = function() end,
    }
  end
  return link
end

s:test("recovers from 30%% frame loss via retransmission", function()
  local got
  local msg = string.rep("XYZ", 2000)  -- forces many fragments
  run(function()
    -- drop roughly every 3rd frame the first time it is sent
    local dropped = {}
    local link = make_link({
      drop = function(n)
        if (n % 3 == 0) and not dropped[n] then dropped[n] = true; return true end
        return false
      end,
    })
    local srv = ru.new_registry(nil, { rto = 0.05 })
    local cli = ru.new_registry(nil, { rto = 0.05 })
    srv.sock = link:socket("srv", 1)
    cli.sock = link:socket("cli", 1)
    link:register("srv", 1, srv)
    link:register("cli", 1, cli)

    fan.spawn(function() srv:run_ticker(0.02) end)
    fan.spawn(function() cli:run_ticker(0.02) end)

    local reader_done = false
    fan.spawn(function()
      local sess = srv:session("cli", 1)
      got = sess:recv()
      reader_done = true
    end)

    local c = cli:session("srv", 1)
    assert(c:send(msg))

    local deadline = fan.gettime() + 6
    while not reader_done and fan.gettime() < deadline do fan.sleep(0.02) end
    srv:stop(); cli:stop()
  end)
  T.eq(got, msg)
end)

s:test("tolerates reordering and duplication", function()
  local got = {}
  run(function()
    local link = make_link({
      delay = function(n) return n % 2 == 0 end,   -- reorder every other frame
      dup   = function(n) return n % 5 == 0 end,    -- duplicate every 5th
    })
    local srv = ru.new_registry(nil, { rto = 0.05 })
    local cli = ru.new_registry(nil, { rto = 0.05 })
    srv.sock = link:socket("srv", 1)
    cli.sock = link:socket("cli", 1)
    link:register("srv", 1, srv)
    link:register("cli", 1, cli)

    fan.spawn(function() srv:run_ticker(0.02) end)
    fan.spawn(function() cli:run_ticker(0.02) end)

    fan.spawn(function()
      local sess = srv:session("cli", 1)
      for _ = 1, 4 do got[#got + 1] = sess:recv() end
    end)

    local c = cli:session("srv", 1)
    for i = 1, 4 do assert(c:send("d" .. i)) end

    local deadline = fan.gettime() + 6
    while #got < 4 and fan.gettime() < deadline do fan.sleep(0.02) end
    srv:stop(); cli:stop()
  end)
  T.eq(#got, 4)
  for i = 1, 4 do T.eq(got[i], "d" .. i) end
end)

s:test("send blocks then resumes under window flow control", function()
  local sent_count, order = 0, {}
  run(function()
    -- tiny window forces the sender to block until ACKs arrive
    local link = make_link({})
    local srv = ru.new_registry(nil, { rto = 0.05, window = 2 })
    local cli = ru.new_registry(nil, { rto = 0.05, window = 2 })
    srv.sock = link:socket("srv", 1)
    cli.sock = link:socket("cli", 1)
    link:register("srv", 1, srv)
    link:register("cli", 1, cli)

    fan.spawn(function() srv:run_ticker(0.02) end)
    fan.spawn(function() cli:run_ticker(0.02) end)

    local received = 0
    fan.spawn(function()
      local sess = srv:session("cli", 1)
      while received < 6 do
        local m = sess:recv()
        if not m then break end
        received = received + 1
        order[#order + 1] = m
      end
    end)

    local c = cli:session("srv", 1)
    -- send 6 single-frame messages; window=2 means send() must yield/resume
    for i = 1, 6 do
      assert(c:send("w" .. i))
      sent_count = sent_count + 1
    end

    local deadline = fan.gettime() + 6
    while received < 6 and fan.gettime() < deadline do fan.sleep(0.02) end
    srv:stop(); cli:stop()
  end)
  T.eq(sent_count, 6)
  T.eq(#order, 6)
  for i = 1, 6 do T.eq(order[i], "w" .. i) end
end)

----------------------------------------------------------------------
-- session cache expiry
----------------------------------------------------------------------
s:test("sweep drops idle sessions past idle_ttl", function()
  local link = make_link({})
  local reg = ru.new_registry(link:socket("a", 1), { idle_ttl = 0.1 })
  local s1 = reg:session("peer1", 1)
  local s2 = reg:session("peer2", 1)
  -- force one session to look old
  s1.last_activity = fan.gettime() - 1
  local dropped = reg:sweep()
  T.eq(dropped, 1)
  T.is_nil(reg.sessions["peer1\0001"])
  T.not_nil(reg.sessions["peer2\0001"])
  -- a busy (unacked) session is not swept even when idle
  s2.last_activity = fan.gettime() - 1
  s2.unacked[0] = { frame = "x", sent_at = 0, retries = 0 }
  T.eq(reg:sweep(), 0)
end)

os.exit(T.run(s))
