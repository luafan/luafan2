local T = require("test_framework")
local fan = require("fan")
local http = require("fan.http")

local s = T.suite("fan.http pure-Lua v1 additions")
local BASE = "http://127.0.0.1:"

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

local function read_request(conn)
  local buf = ""
  while not buf:find("\r\n\r\n", 1, true) do
    local data = conn:receive()
    if not data then return nil end
    buf = buf .. data
  end
  local head, rest = buf:match("^(.-\r\n\r\n)(.*)$")
  local headers = {}
  for k, v in head:gmatch("\r\n([^:\r\n]+):%s*([^\r\n]*)") do
    headers[k:lower()] = v
  end
  local length = tonumber(headers["content-length"] or "0") or 0
  while #rest < length do
    local data = conn:receive()
    if not data then break end
    rest = rest .. data
  end
  return {body = rest:sub(1, length)}
end

s:test("repeated headers use v1 arrays", function()
  local port = 24601
  local server, response
  run(function()
    server = assert(fan.tcp.bind("127.0.0.1", port, function(conn)
      conn:receive()
      conn:send("HTTP/1.1 200 OK\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
      fan.sleep(0.02)
      conn:close()
    end))
    response = http.request{backend = "lua", url = BASE .. port .. "/"}
  end)
  if server then server:close() end
  T.not_nil(response)
  T.eq(response.responseMessage, "OK")
  T.eq(response.headers["set-cookie"][1], "a=1")
  T.eq(response.headers["set-cookie"][2], "b=2")
end)

s:test("upload callbacks send body and receive args self", function()
  local port = 24602
  local server, response, self_seen
  run(function()
    server = assert(fan.tcp.bind("127.0.0.1", port, function(conn)
      local request = read_request(conn)
      local body = request and request.body or ""
      conn:send("HTTP/1.1 200 OK\r\nContent-Length: " .. #body .. "\r\nConnection: close\r\n\r\n" .. body)
      fan.sleep(0.02)
      conn:close()
    end))
    response = http.request{backend = "lua", method = "POST", url = BASE .. port .. "/",
      onbodylength = function(args)
        self_seen = args.method == "POST"
        return 11
      end,
      onsend = function(size)
        if size > 0 then return "onsend-body" end
      end}
  end)
  if server then server:close() end
  T.not_nil(response)
  T.truthy(self_seen)
  T.eq(response.body, "onsend-body")
end)

s:test("successful oncomplete receives response and suppresses return", function()
  local port = 24603
  local server, response, complete
  run(function()
    server = assert(fan.tcp.bind("127.0.0.1", port, function(conn)
      conn:receive()
      conn:send("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
      fan.sleep(0.02)
      conn:close()
    end))
    response = http.request{backend = "lua", url = BASE .. port .. "/",
      oncomplete = function(value) complete = value end}
  end)
  if server then server:close() end
  T.is_nil(response)
  T.eq(complete.status, 200)
  T.eq(complete.body, "ok")
end)

s:test("error oncomplete receives v1 error response", function()
  local response, err, complete
  run(function()
    response, err = http.request{backend = "lua", url = "http://127.0.0.1:1/",
      oncomplete = function(value) complete = value end}
  end)
  T.is_nil(response)
  T.is_nil(err)
  T.eq(complete.status, 0)
  T.is_type(complete.error, "string")
end)

os.exit(T.run(s))
