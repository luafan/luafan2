-- Trivial background service: increments a counter every 60 s while
-- status == "running". The counter isn't exercised by tests (they'd
-- have to wait too long); the point is to prove onStart/onStop and
-- getStatus round-trip through service.lua.
status = "n/a"

local fan   = require "fan"
local co
local count = 0

function onStart()
  status = "running"
  count  = 0
  co = coroutine.create(function()
    while status == "running" do
      fan.sleep(60)
      count = count + 1
    end

    status = "stopped"
    co = nil
  end)

  coroutine.resume(co)
end

function onStop()
  status = "stopping"
  while status ~= "stopped" do
    fan.sleep(0.1)
  end
end

function getStatus()
  return string.format("%s %d", status, count)
end
