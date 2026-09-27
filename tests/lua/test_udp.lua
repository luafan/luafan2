--[[
  test_udp.lua — M3 UDP contract tests. Run via ./fan.
]]
local T = require("test_framework")
local fan = require("fan")

local s = T.suite("fan.udp (M3)")

local function with_loop(body)
  fan.spawn(body)
  fan.loop()
end

s:test("sendto / recv round-trip with sender address", function()
  local PORT = 24301
  local data, host, port
  with_loop(function()
    local srv = assert(fan.udp.new("127.0.0.1", PORT))
    fan.spawn(function()
      local cli = assert(fan.udp.new())        -- unbound client
      cli:sendto("udp-hello", "127.0.0.1", PORT)
      fan.sleep(0.05)
      cli:close()
    end)
    data, host, port = srv:recv()
    srv:close()
    fan.loopbreak()
  end)
  T.eq(data, "udp-hello")
  T.eq(host, "127.0.0.1")
  T.truthy(port and port > 0)
end)

s:test("multiple datagrams received in sequence", function()
  local PORT = 24302
  local got = {}
  with_loop(function()
    local srv = assert(fan.udp.new("127.0.0.1", PORT))
    fan.spawn(function()
      local cli = assert(fan.udp.new())
      for i = 1, 3 do cli:sendto("msg" .. i, "127.0.0.1", PORT) end
      fan.sleep(0.1)
      cli:close()
    end)
    for _ = 1, 3 do
      local d = srv:recv()
      got[#got + 1] = d
    end
    srv:close()
    fan.loopbreak()
  end)
  T.eq(#got, 3)
  -- UDP order not guaranteed across networks, but loopback preserves it
  table.sort(got)
  T.eq(got[1], "msg1"); T.eq(got[2], "msg2"); T.eq(got[3], "msg3")
end)

s:test("sendto rejects bad port / host", function()
  with_loop(function()
    local u = assert(fan.udp.new())
    local ok1 = pcall(function() u:sendto("x", "127.0.0.1", 70000) end)
    T.falsy(ok1)  -- port out of range raises
    local r, err = u:sendto("x", "not-an-ip", 1000)
    T.is_nil(r); T.not_nil(err)
    u:close()
    fan.loopbreak()
  end)
end)

s:test("bind_port out of range errors", function()
  with_loop(function()
    local ok = pcall(function() fan.udp.new("127.0.0.1", 99999) end)
    T.falsy(ok)
    fan.loopbreak()
  end)
end)

s:test("IPv6 sendto / recv round-trip over loopback", function()
  local PORT = 24303
  local data, host, port
  with_loop(function()
    local srv = assert(fan.udp.new("::1", PORT))
    fan.spawn(function()
      local cli = assert(fan.udp.new(nil, nil, "inet6"))
      cli:sendto("v6-hello", "::1", PORT)
      fan.sleep(0.05)
      cli:close()
    end)
    data, host, port = srv:recv()
    srv:close()
    fan.loopbreak()
  end)
  T.eq(data, "v6-hello")
  T.eq(host, "::1")
  T.truthy(port and port > 0)
end)

s:test("IPv4 multicast join / send / leave", function()
  local GROUP, PORT = "239.255.42.99", 24304
  local got
  with_loop(function()
    local rx = assert(fan.udp.new("0.0.0.0", PORT))
    local jok, jerr = rx:join(GROUP)
    T.truthy(jok, "join: " .. tostring(jerr))
    fan.spawn(function()
      local tx = assert(fan.udp.new())
      fan.sleep(0.03)
      tx:sendto("mcast4", GROUP, PORT)
      fan.sleep(0.05)
      tx:close()
    end)
    got = rx:recv()
    local lok = rx:leave(GROUP)
    T.truthy(lok, "leave should succeed")
    rx:close()
    fan.loopbreak()
  end)
  T.eq(got, "mcast4")
end)

s:test("IPv6 multicast join / leave (control-plane)", function()
  -- Loopback IPv6 multicast delivery is flaky across kernels/containers, so
  -- this test asserts the join/leave setsockopt path works, not delivery.
  local GROUP, PORT = "ff02::1:ff00:42", 24305
  local jok, jerr, lok
  with_loop(function()
    local rx = assert(fan.udp.new("::", PORT, "inet6"))
    jok, jerr = rx:join(GROUP)
    if jok then lok = rx:leave(GROUP) end
    rx:close()
    fan.loopbreak()
  end)
  -- Some CI containers lack IPv6 multicast routing; accept a clean error too,
  -- but a success must round-trip through leave.
  if jok then
    T.truthy(lok, "leave after successful join must succeed")
  else
    T.not_nil(jerr)  -- clean error string, no crash
  end
end)

s:test("join rejects non-multicast/garbage group", function()
  with_loop(function()
    local u = assert(fan.udp.new("0.0.0.0", 24306))
    local r, err = u:join("not-an-ip")
    T.is_nil(r); T.not_nil(err)
    u:close()
    fan.loopbreak()
  end)
end)

os.exit(T.run(s))
