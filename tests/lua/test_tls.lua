--[[
  test_tls.lua — M4 TLS client contract tests (fan.tcp.connect{ssl=true}).

  These tests need a TLS peer. We spin up OpenSSL's `s_server` in "-rev" echo
  mode with an on-the-fly self-signed cert, then connect through the v2 TLS
  client (bufferevent_openssl under the hood) and assert handshake + data flow
  and the verification-failure path. If the `openssl` CLI is unavailable the
  peer-dependent cases self-skip so the suite still passes on minimal hosts.
]]
local T = require("test_framework")
local fan = require("fan")

local s = T.suite("fan.tls client (M4)")

local TMP = os.getenv("TMPDIR") or "/tmp"
local CERT = TMP .. "/fan_tls_test_cert.pem"
local KEY  = TMP .. "/fan_tls_test_key.pem"

local function have_openssl()
  local ok = os.execute("command -v openssl >/dev/null 2>&1")
  return ok == true or ok == 0
end

local function gen_cert()
  local cmd = string.format(
    "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s -days 1 -nodes "
    .. "-subj /CN=localhost >/dev/null 2>&1", KEY, CERT)
  local ok = os.execute(cmd)
  return ok == true or ok == 0
end

-- start s_server in the background on `port`; returns nothing (fire and forget)
local function start_server(port, extra)
  os.execute(string.format(
    "openssl s_server -quiet %s -accept %d -cert %s -key %s "
    .. ">/dev/null 2>&1 &", extra or "-rev", port, CERT, KEY))
end

local function stop_servers()
  os.execute("pkill -f 's_server' >/dev/null 2>&1")
end

local function run(body)
  fan.spawn(function()
    local ok, err = pcall(body)
    fan.loopbreak()
    if not ok then error(err, 0) end
  end)
  fan.loop()
end

s:test("fan.tls.available reports compiled-in state", function()
  -- built with -DFAN_WITH_OPENSSL=ON, so this must be true here
  T.truthy(fan.tls.available())
  T.truthy(fan.tls.enabled)
end)

local skip = not have_openssl()
if skip then
  s:test("openssl CLI unavailable — TLS peer tests skipped", function()
    T.truthy(true)
  end)
end

if not skip then
  assert(gen_cert(), "failed to generate test certificate")

  s:test("TLS handshake + echo round-trip (verify off)", function()
    local PORT = 28443
    local got
    start_server(PORT)  -- -rev: reverses each line it receives
    run(function()
      fan.sleep(0.4)  -- let s_server bind
      local c, err = fan.tcp.connect("127.0.0.1", PORT,
        { ssl = true, verify_peer = false, verify_host = false })
      assert(c, "tls connect: " .. tostring(err))
      c:send("hello\n")
      got = c:receive()
      c:close()
    end)
    T.truthy(got ~= nil)
    T.eq((got or ""):gsub("%s+$", ""), "olleh")
  end)

  s:test("peer verification failure is reported (self-signed, verify on)", function()
    local PORT = 28444
    local result, err
    start_server(PORT)
    run(function()
      fan.sleep(0.4)
      -- verify_peer=true against a self-signed cert must fail the handshake
      result, err = fan.tcp.connect("127.0.0.1", PORT,
        { ssl = true, verify_peer = true, verify_host = true })
    end)
    -- connect returns nil,err when the TLS handshake is rejected
    T.is_nil(result)
    T.not_nil(err)
  end)

  stop_servers()
end

os.exit(T.run(s))
