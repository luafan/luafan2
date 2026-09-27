--[[
  test_fifo.lua — M2 FIFO + connector contract tests.
  Run via ./fan.
]]
local T = require("test_framework")
local fan = require("fan")
local connector = require("fan.connector")

local s = T.suite("fan.fifo + connector (M2)")

local function with_loop(body)
  fan.spawn(body)
  fan.loop()
end

local function tmpname(tag)
  return "/tmp/fan2_fifo_" .. tag .. "_" .. tostring(os.time()) .. "_" .. tostring(math.random(1, 1e6))
end

s:test("fifo reader/writer round-trip", function()
  local path = tmpname("rt")
  local got
  with_loop(function()
    -- reader opens first (writer open would ENXIO without a reader)
    local r = assert(fan.fifo.open{ name = path, mode = "r", create = true })
    fan.spawn(function()
      local w = assert(fan.fifo.open{ name = path, mode = "w" })
      w:send("fifo-hello")
      fan.sleep(0.05)
      w:close()
    end)
    got = r:receive()
    r:close()
    fan.loopbreak()
  end)
  os.remove(path)
  T.eq(got, "fifo-hello")
end)

s:test("fifo reports not-a-reader / not-a-writer", function()
  local path = tmpname("role")
  with_loop(function()
    local r = assert(fan.fifo.open{ name = path, mode = "r", create = true })
    local ok, err = r:send("x")   -- reader cannot send
    T.is_nil(ok)
    T.not_nil(err)
    r:close()
    fan.loopbreak()
  end)
  os.remove(path)
end)

s:test("fifo create rejects non-fifo path", function()
  -- point at an existing regular file
  local path = tmpname("reg")
  local fh = io.open(path, "w"); fh:write("x"); fh:close()
  with_loop(function()
    local f, err = fan.fifo.open{ name = path, mode = "r", create = true }
    T.is_nil(f)
    T.not_nil(err)
    fan.loopbreak()
  end)
  os.remove(path)
end)

s:test("connector.connect tcp url round-trip", function()
  local PORT = 24211
  local got
  local server
  with_loop(function()
    server = assert(connector.bind("tcp://127.0.0.1:" .. PORT, function(conn)
      local d = conn:receive()
      if d then conn:send(d) end
      conn:close()
    end))
    local c = assert(connector.connect("tcp://127.0.0.1:" .. PORT))
    c:send("via-connector")
    got = c:receive()
    c:close()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.eq(got, "via-connector")
end)

s:test("connector rejects unknown scheme", function()
  local c, err = connector.connect("wat://nope")
  T.is_nil(c)
  T.not_nil(err)
end)

os.exit(T.run(s))
