--[[
  test_httpsd.lua — M4 HTTPS server contract tests.

  Spins up fan.httpd with { ssl=true, cert=..., key=... } over a locally
  generated self-signed cert, then drives it with fan.http (https:// scheme,
  verify=false). Verifies the C-level TLS server bufferevent path in
  net/tls.c (server_ctx_new + server_bev with BUFFEREVENT_SSL_ACCEPTING)
  plumbs through fan.tcp.bind and fan.httpd cleanly.

  If the openssl CLI is not available the cases self-skip.
]]
local T = require("test_framework")
local fan   = require("fan")
local http  = require("fan.http")
local httpd = require("fan.httpd")

local s = T.suite("fan.httpd HTTPS (M4)")

local TMP  = os.getenv("TMPDIR") or "/tmp"
local CERT = TMP .. "/fan_httpsd_cert.pem"
local KEY  = TMP .. "/fan_httpsd_key.pem"

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

s:test("fan.tls compiled in (guard)", function()
  T.truthy(fan.tls.available())
end)

if not have_openssl() then
  s:test("openssl CLI unavailable — HTTPS server tests skipped", function()
    T.truthy(true)
  end)
  os.exit(T.run(s))
end

assert(gen_cert(), "failed to generate test certificate")

s:test("bind{ssl=true} without cert/key returns nil,err (no crash)", function()
  local server, err
  run(function()
    server, err = httpd.bind{ port = 24601, ssl = true,
      handler = function() end }
    -- if bind fails synchronously we get back nil,err
  end)
  if server then server:close() end
  T.is_nil(server)
  T.not_nil(err)
end)

s:test("bind{ssl=true, cert, key} accepts HTTPS GET (fan.http verify=off)", function()
  local PORT = 24602
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, ssl = true, cert = CERT, key = KEY,
      handler = function(req, resp)
        resp:reply(200, { ["Content-Type"] = "text/plain" },
                   "HTTPS " .. req.method .. " " .. req.path)
      end,
    })
    resp = http.get("https://127.0.0.1:" .. PORT .. "/hello",
      { verify = false })
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.headers["content-type"], "text/plain")
  T.eq(resp.body, "HTTPS GET /hello")
end)

s:test("HTTPS POST body round-trip", function()
  local PORT = 24603
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, ssl = true, cert = CERT, key = KEY,
      handler = function(req, resp)
        resp:reply(200, {}, "echo:" .. req.body)
      end,
    })
    resp = http.post("https://127.0.0.1:" .. PORT .. "/echo",
      { body = "secure-payload", verify = false })
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "echo:secure-payload")
end)

s:test("HTTPS chunked reply (reply_start / reply_chunk / reply_end)", function()
  local PORT = 24604
  local server, resp
  run(function()
    server = assert(httpd.bind{
      port = PORT, ssl = true, cert = CERT, key = KEY,
      handler = function(_, resp)
        resp:reply_start(200, {})
        resp:reply_chunk("alpha")
        resp:reply_chunk("-")
        resp:reply_chunk("bravo")
        resp:reply_end()
      end,
    })
    resp = http.get("https://127.0.0.1:" .. PORT .. "/stream",
      { verify = false })
  end)
  if server then server:close() end
  T.not_nil(resp)
  T.eq(resp.status, 200)
  T.eq(resp.body, "alpha-bravo")
end)

s:test("N concurrent HTTPS clients over one server", function()
  local PORT = 24605
  local N = 4
  local results = {}
  local done = 0
  run(function()
    local server = assert(httpd.bind{
      port = PORT, ssl = true, cert = CERT, key = KEY,
      handler = function(req, resp)
        resp:reply(200, {}, "OK:" .. req.body)
      end,
    })
    for i = 1, N do
      fan.spawn(function()
        local r = http.post("https://127.0.0.1:" .. PORT .. "/",
          { body = "c" .. i, verify = false })
        results[i] = r and r.body
        done = done + 1
        if done == N then server:close(); fan.loopbreak() end
      end)
    end
    while done < N do fan.sleep(0.02) end
  end)
  T.eq(done, N)
  for i = 1, N do T.eq(results[i], "OK:c" .. i) end
end)

os.exit(T.run(s))
