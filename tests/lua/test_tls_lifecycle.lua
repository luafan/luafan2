--[=[
  test_tls_lifecycle.lua — client SSL_CTX lease and bufferevent teardown.

  The TLS peer uses the same in-process httpd.bind{ssl=true} path as the
  established HTTPS server suite. The cases exercise public sync and async
  client APIs so ASan/LSan can validate SSL, bufferevent, and SSL_CTX ownership.
]=]

local T = require("test_framework")
local fan = require("fan")
local tcp = fan.tcp
local httpd = require("fan.httpd")

local s = T.suite("TLS client lease lifecycle")
local TMP = os.getenv("TMPDIR") or "/tmp"
local CERT = TMP .. "/fan_httpsd_cert.pem"
local KEY = TMP .. "/fan_httpsd_key.pem"

local function have_openssl()
  local ok = os.execute("command -v openssl >/dev/null 2>&1")
  return ok == true or ok == 0
end

local function gen_cert()
  local cmd = string.format(
    "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s -days 1 -nodes "
    .. "-subj /CN=localhost >/dev/null 2>&1", KEY, CERT)
  local ok = os.execute(cmd)
  local cert = io.open(CERT, "rb")
  local key = io.open(KEY, "rb")
  local has_cert = cert ~= nil
  local has_key = key ~= nil
  local cert_size = cert and cert:seek("end") or -1
  local key_size = key and key:seek("end") or -1
  if cert then cert:close() end
  if key then key:close() end
  local cli = os.execute("openssl x509 -in " .. CERT .. " -noout >/dev/null 2>&1")
  print(string.format("TLS lifecycle fixture: TMP=%s cert=%s/%d key=%s/%d cli=%s os_ok=%s", TMP, tostring(has_cert), cert_size, tostring(has_key), key_size, tostring(cli), tostring(ok)))
  return (ok == true or ok == 0) and has_cert and has_key and (cli == true or cli == 0)
end

local function with_loop(body, keep_running)
  local caught
  fan.spawn(function()
    local ok, err = pcall(body)
    if not ok then caught = err end
    if not keep_running then fan.loopbreak() end
  end)
  fan.loop()
  if caught then error(caught, 0) end
end

local function bind_tls_server(port)
  return assert(httpd.bind{
    port = port,
    ssl = true,
    cert = CERT,
    key = KEY,
    handler = function(_, resp)
      resp:reply(200, {}, "lease-ok")
    end,
  })
end

local function bind_tls_async_server(port)
  return assert(tcp.bind_async{
    host = "127.0.0.1",
    port = port,
    ssl = true,
    cert = CERT,
    key = KEY,
    onaccept = function(_, accept)
      accept:bind{
        onread = function(conn)
          conn:send("lease-ok")
          conn:close()
        end,
        ondisconnected = function() end,
      }
    end,
  })
end

if not have_openssl() then
  s:test("openssl CLI unavailable — lifecycle cases skipped", function()
    T.truthy(true)
  end)
  os.exit(T.run(s))
end

assert(gen_cert(), "failed to generate lifecycle certificate")

s:test("sync default TLS context: repeated connect and close", function()
  local port = 28601
  local server
  with_loop(function()
    server = bind_tls_server(port)
    for _ = 1, 20 do
      local conn, err = fan.tcp.connect("127.0.0.1", port,
        { ssl = true, verify_peer = false, verify_host = false })
      assert(conn, tostring(err))
      conn:send("GET / HTTP/1.0\r\n\r\n")
      conn:receive()
      conn:close()
    end
  end)
  if server then server:close() end
  T.truthy(true)
end)

s:test("sync custom CA cache: repeated connect and final release", function()
  local port = 28602
  local server
  with_loop(function()
    server = bind_tls_server(port)
    for _ = 1, 20 do
      local conn, err = fan.tcp.connect("127.0.0.1", port, {
        ssl = true,
        verify_peer = true,
        verify_host = true,
        ssl_host = "localhost",
        cainfo = CERT,
      })
      assert(conn, tostring(err))
      conn:send("GET / HTTP/1.0\r\n\r\n")
      conn:receive()
      conn:close()
    end
  end)
  if server then server:close() end
  T.truthy(true)
end)

s:test("sync GC after peer close does not retain client TLS state", function()
  local port = 28603
  local server
  with_loop(function()
    server = bind_tls_server(port)
    local conn, err = fan.tcp.connect("127.0.0.1", port,
      { ssl = true, verify_peer = false, verify_host = false })
    assert(conn, tostring(err))
    conn:send("GET / HTTP/1.0\r\n\r\n")
    conn:receive()
    conn = nil
    collectgarbage("collect")
    collectgarbage("collect")
  end)
  if server then server:close() end
  T.truthy(true)
end)

s:test("async TLS terminal disconnect releases lease before callback", function()
  local port = 28604
  local server
  local disconnected = false
  with_loop(function()
    server = bind_tls_async_server(port)
    fan.tcp.connect_async{
      host = "127.0.0.1",
      port = port,
      ssl = true,
      ssl_verifypeer = 0,
      ssl_verifyhost = 0,
      onconnected = function(conn)
        conn:send("GET / HTTP/1.0\r\n\r\n")
      end,
      onread = function()
      end,
      ondisconnected = function()
        disconnected = true
        fan.loopbreak()
      end,
    }
  end, true)
  if server then server:close() end
  T.truthy(disconnected)
end)

s:test("async TLS reconnect releases old lease before rebuilding", function()
  local port = 28605
  local server
  local connected = 0
  local disconnected = 0
  with_loop(function()
    server = bind_tls_async_server(port)
    fan.tcp.connect_async{
      host = "127.0.0.1",
      port = port,
      ssl = true,
      ssl_verifypeer = 0,
      ssl_verifyhost = 0,
      onconnected = function(conn)
        connected = connected + 1
        conn:send("GET / HTTP/1.0\r\n\r\n")
      end,
      onread = function()
      end,
      ondisconnected = function(conn)
        disconnected = disconnected + 1
        if connected < 2 then
          fan.sleep(0.01)
          assert(conn:reconnect())
        else
          fan.loopbreak()
        end
      end,
    }
  end, true)
  if server then server:close() end
  T.eq(connected, 2)
  T.eq(disconnected, 2)
end)

local result = T.run(s)
os.execute("rm -f " .. CERT .. " " .. KEY)
os.exit(result)
