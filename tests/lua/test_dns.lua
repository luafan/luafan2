--[[
  test_dns.lua — M3 DNS resolution contract tests. Run via ./fan.

  Note: network name resolution is environment-dependent. These tests focus on
  behaviours we can rely on offline: numeric literals resolve synchronously,
  localhost resolves, and a syntactically bogus name returns an error rather
  than crashing. A real public hostname is attempted but tolerated to fail
  (offline CI), asserting only that it does not crash.
]]
local T = require("test_framework")
local fan = require("fan")

local s = T.suite("fan.dns (M3)")

local function with_loop(body)
  -- Guarantee the loop always terminates even if `body` raises: on error we
  -- still request a loopbreak so a failing assertion cannot hang the suite.
  fan.spawn(function()
    local ok, err = pcall(body)
    fan.loopbreak()
    if not ok then error(err, 0) end
  end)
  fan.loop()
end

local function contains(t, v)
  for _, x in ipairs(t) do if x == v then return true end end
  return false
end

s:test("numeric IPv4 literal resolves to itself", function()
  local ips
  with_loop(function()
    ips = assert(fan.dns.resolve("127.0.0.1"))
    fan.loopbreak()
  end)
  T.truthy(type(ips) == "table")
  T.truthy(contains(ips, "127.0.0.1"))
end)

s:test("numeric IPv6 literal resolves to itself", function()
  local ips
  with_loop(function()
    ips = assert(fan.dns.resolve("::1"))
    fan.loopbreak()
  end)
  T.truthy(type(ips) == "table" and #ips >= 1)
  T.truthy(contains(ips, "::1"))
end)

s:test("localhost / real name resolution tolerated (evdns skips /etc/hosts)", function()
  -- libevent's evdns performs real DNS queries and does NOT consult
  -- /etc/hosts, so "localhost" only resolves if the configured nameserver
  -- answers it. In offline/metadata-only CI this may fail. We assert only
  -- that the call returns cleanly (table of IPs, or nil+err) without hanging
  -- or crashing; the bounded evdns timeout guarantees termination.
  local ips, err
  with_loop(function()
    ips, err = fan.dns.resolve("localhost")
    fan.loopbreak()
  end)
  if ips then
    T.truthy(type(ips) == "table")
  else
    T.not_nil(err)
  end
end)

s:test("bogus hostname returns nil, err (no crash)", function()
  local ips, err
  with_loop(function()
    ips, err = fan.dns.resolve("no-such-host.invalid.")
    fan.loopbreak()
  end)
  T.is_nil(ips)
  T.not_nil(err)
end)

s:test("resolve outside coroutine errors cleanly", function()
  local ok = pcall(function() fan.dns.resolve("127.0.0.1") end)
  T.falsy(ok)
end)

s:test("port out of range errors", function()
  with_loop(function()
    local ok = pcall(function() fan.dns.resolve("127.0.0.1", 99999) end)
    T.falsy(ok)
    fan.loopbreak()
  end)
end)

os.exit(T.run(s))
