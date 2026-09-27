--[[
  test_coro.lua — M1 runtime contract tests: spawn / sleep / loop / loopbreak.
  Run with the `fan` executable: fan test_coro.lua
  Verifies the park/resume path and the R17/R20 lifetime invariant
  (a coroutine parked in fan.sleep must survive GC pressure across the resume).
]]
local T = require("test_framework")
local fan = require("fan")

local s = T.suite("fan runtime coro (M1)")

s:test("spawn runs the function body", function()
  local ran = false
  fan.spawn(function() ran = true end)
  -- spawn resumes immediately; a body with no yield finishes synchronously
  T.truthy(ran, "spawned function should have run")
end)

s:test("spawn passes arguments", function()
  local got
  fan.spawn(function(a, b, c) got = { a, b, c } end, 1, "two", true)
  T.eq(got[1], 1)
  T.eq(got[2], "two")
  T.eq(got[3], true)
end)

s:test("sleep outside coroutine errors, not crashes", function()
  -- calling sleep on the main thread must raise a Lua error
  T.error_raised(function() fan.sleep(0.01) end)
end)

s:test("sleep parks and resumes via the loop", function()
  local order = {}
  local t0 = fan.gettime()
  fan.spawn(function()
    order[#order + 1] = "before"
    fan.sleep(0.05)
    order[#order + 1] = "after"
    fan.loopbreak()
  end)
  -- at this point the coroutine has parked; "after" not yet appended
  T.eq(#order, 1)
  T.eq(order[1], "before")
  fan.loop()
  local dt = fan.gettime() - t0
  T.eq(#order, 2)
  T.eq(order[2], "after")
  T.truthy(dt >= 0.04, "should have slept ~0.05s, got " .. tostring(dt))
end)

s:test("multiple coroutines sleep concurrently", function()
  local done = 0
  local n = 5
  for i = 1, n do
    fan.spawn(function()
      fan.sleep(0.02 * i)
      done = done + 1
      if done == n then fan.loopbreak() end
    end)
  end
  T.eq(done, 0)
  fan.loop()
  T.eq(done, n)
end)

s:test("GC pressure across a parked sleep does not collect the coroutine", function()
  -- The parked coroutine holds no external reference; only the runtime's
  -- registry pin keeps it alive. Force aggressive GC while it is parked.
  local resumed = false
  fan.spawn(function()
    fan.sleep(0.03)
    resumed = true
    fan.loopbreak()
  end)
  -- allocate + full GC repeatedly while the coroutine is parked
  for _ = 1, 50 do
    local junk = {}
    for k = 1, 200 do junk[k] = { k, tostring(k) } end
    collectgarbage("collect")
  end
  fan.loop()
  T.truthy(resumed, "coroutine must resume after GC pressure (R17/R20 invariant)")
end)

os.exit(T.run(s))
