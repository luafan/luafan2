--[[
  test_worker.lua — M6 fan.worker contract tests.

  Covers:
    - inline collapse (slaves == 0)
    - multi-process: fork slaves, RPC via TCP loopback + objectbuf frames
    - error paths: unknown function, user error, terminate() while parked
]]
local T   = require("test_framework")
local fan = require("fan")
local worker = require("fan.worker")

local s = T.suite("fan.worker (M6)")

-- ---- Inline path --------------------------------------------------------
s:test("worker.new{slaves=0}: inline collapse; call dispatches to funcs",
function()
    local w = worker.new{
        slaves = 0,
        funcs = {
            add    = function(a, b) return a + b end,
            greet  = function(n) return "hi " .. n end,
        },
    }
    local sz = w:size()
    T.eq(sz.slaves, 0)
    T.eq(sz.inline, true)
    local ok, r = w:call("add", 2, 3)
    T.truthy(ok); T.eq(r, 5)
    local ok2, r2 = w:call("greet", "world")
    T.truthy(ok2); T.eq(r2, "hi world")
end)

s:test("inline: unknown function returns false, err", function()
    local w = worker.new{ slaves = 0, funcs = {} }
    local ok, err = w:call("nope")
    T.falsy(ok); T.truthy(err); T.truthy(err:find("no such function"))
end)

s:test("inline: user error propagates as (false, msg)", function()
    local w = worker.new{ slaves = 0, funcs = {
        bad = function() error("kaboom") end,
    }}
    local ok, err = w:call("bad")
    T.falsy(ok); T.truthy(err); T.truthy(err:find("kaboom"))
end)

s:test("inline: terminate is a no-op", function()
    local w = worker.new{ slaves = 0, funcs = {} }
    w:terminate()
    T.eq(w:size().slaves, 0)
end)

-- ---- Multi-process path -------------------------------------------------
local function run_master(fn)
    local caught
    fan.spawn(function()
        local ok, err = pcall(fn)
        if not ok then caught = err end
        fan.loopbreak()
    end)
    fan.loop()
    if caught then error(caught) end
end

s:test("multi-process: 2 slaves, single call round-trips", function()
    run_master(function()
        local w = worker.new{
            slaves = 2,
            funcs = { add = function(a, b) return a + b end },
        }
        local ok, r = w:call("add", 40, 2)
        T.truthy(ok, "call ok"); T.eq(r, 42)
        w:terminate()
    end)
end)

s:test("multi-process: 3 slaves, many concurrent calls", function()
    run_master(function()
        local w = worker.new{
            slaves = 3,
            funcs = {
                square = function(n) fan.sleep(0.01); return n * n end,
            },
        }
        local results, done = {}, 0
        local N = 3
        for i = 1, N do
            fan.spawn(function()
                local ok, r = w:call("square", i)
                results[i] = ok and r or false
                done = done + 1
            end)
        end
        local waited = 0
        while done < N do
            fan.sleep(0.05)
            waited = waited + 0.05
            if waited > 5 then
                error("concurrent test timed out: done=" .. done)
            end
        end
        for i = 1, N do T.eq(results[i], i * i, "i="..i) end
        w:terminate()
    end)
end)

s:test("multi-process: unknown function returns (false, err)", function()
    run_master(function()
        local w = worker.new{ slaves = 1, funcs = {} }
        local ok, err = w:call("nope")
        T.falsy(ok); T.truthy(err); T.truthy(err:find("no such function"))
        w:terminate()
    end)
end)

s:test("multi-process: user error is captured and reported", function()
    run_master(function()
        local w = worker.new{
            slaves = 1,
            funcs = { bad = function() error("kaboom") end },
        }
        local ok, err = w:call("bad")
        T.falsy(ok); T.truthy(err); T.truthy(err:find("kaboom"))
        w:terminate()
    end)
end)

s:test("multi-process: terminate() resumes callers parked mid-flight",
function()
    run_master(function()
        local w = worker.new{
            slaves = 1,
            funcs = { sleep = function() fan.sleep(2) return "done" end },
        }
        local outcome = {}
        fan.spawn(function()
            local ok, r = w:call("sleep")
            outcome.ok, outcome.r = ok, r
        end)
        fan.sleep(0.2)
        w:terminate()
        -- give the reaper a moment
        for _ = 1, 30 do
            if outcome.ok ~= nil then break end
            fan.sleep(0.02)
        end
        T.falsy(outcome.ok)
        T.truthy(outcome.r); T.truthy(outcome.r:find("dead") or outcome.r:find("terminate"))
    end)
end)

os.exit(T.run(s))
