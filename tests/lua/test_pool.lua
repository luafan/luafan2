--[[
  test_pool.lua — M6 fan.pool contract tests.

  Covers: get/put reuse, max-cap enforcement (blocking + resume),
  health check drop, close_all, with() wrapper, error propagation
  from user fn, and factory-fail handling.
]]
local T   = require("test_framework")
local fan = require("fan")
local pool = require("fan.pool")

local s = T.suite("fan.pool (M6)")

-- Simple counter-backed resource
local function make_factory()
    local nxt = 0
    return function()
        nxt = nxt + 1
        return { id = nxt, closed = false, alive = true }
    end, function(r) r.closed = true end
end

s:test("get/put reuse: same resource returned to the top of the stack", function()
    local n, c = make_factory()
    local p = pool.new{new = n, close = c, max = 3}
    local a = p:get(); T.eq(a.id, 1)
    p:put(a)
    local b = p:get(); T.eq(b.id, 1) -- LIFO reuse
    p:put(b)
end)

s:test("max-cap: creating up to max succeeds, one more yields until put", function()
    local n, c = make_factory()
    local p = pool.new{new = n, close = c, max = 2}
    local a = p:get(); local b = p:get()
    T.eq(a.id, 1); T.eq(b.id, 2)
    T.eq(p:size().count, 2)

    -- inside a coroutine so get() may yield
    local out = {}
    local co = coroutine.wrap(function()
        local r = p:get()   -- must yield since count==max and idle empty
        out.got = r.id
    end)
    -- start it: it yields inside get()
    co()
    T.is_nil(out.got, "coroutine should still be parked")

    -- returning `a` should immediately wake the waiter with a
    p:put(a)
    T.eq(out.got, a.id)

    p:put(b)
end)

s:test("factory failure propagates as (nil, err) and does not consume a slot",
function()
    local p = pool.new{
        new = function() error("boom") end,
        max = 2,
    }
    local r, err = p:get()
    T.is_nil(r); T.truthy(err); T.truthy(err:find("pool.new failed"))
    T.eq(p:size().count, 0)
end)

s:test("put(nil): treats resource as dead; frees the slot; wakes waiter", function()
    local n, c = make_factory()
    local p = pool.new{new = n, close = c, max = 1}
    local a = p:get(); T.eq(a.id, 1)
    local out = {}
    local co = coroutine.wrap(function()
        local r = p:get(); out.id = r.id
    end)
    co()  -- parks
    T.is_nil(out.id)
    p:put(nil)                  -- caller signals a was killed
    -- freeing the slot + creating a new resource for the waiter
    T.eq(out.id, 2)
    p:put(nil)                  -- clean up
end)

s:test("health check drop: unhealthy resource is discarded, slot freed", function()
    local n, c = make_factory()
    local kill_next = false
    local p = pool.new{
        new = n, close = c, max = 1,
        health = function(r) return not kill_next end,
    }
    local a = p:get()
    kill_next = true
    p:put(a)
    T.truthy(a.closed, "unhealthy resource should be closed")
    T.eq(p:size().count, 0)
    -- next get() creates a fresh one
    kill_next = false
    local b = p:get(); T.eq(b.id, 2)
    p:put(b)
end)

s:test("with(): xpcall-wraps user fn, always calls put", function()
    local n, c = make_factory()
    local p = pool.new{new = n, close = c, max = 1}
    local ok, res = p:with(function(r) return r.id + 100 end)
    T.eq(ok, 101)
    T.is_nil(res)
    T.eq(p:size().idle, 1)   -- returned to pool
    -- error path: put still happens
    local rc, err = p:with(function(r) error("kaboom") end)
    T.is_nil(rc); T.truthy(err); T.truthy(err:find("kaboom"))
    T.eq(p:size().idle, 1)   -- still one idle
end)

s:test("close_all: idle resources get close_fn called, count drops", function()
    local n, c = make_factory()
    local p = pool.new{new = n, close = c, max = 4}
    local a = p:get(); local b = p:get(); local d = p:get()
    p:put(a); p:put(b); p:put(d)
    T.eq(p:size().idle, 3)
    p:close_all()
    T.eq(p:size().idle, 0)
    T.eq(p:size().count, 0)
    T.truthy(a.closed and b.closed and d.closed)
end)

s:test("size(): reports live count / idle / waiters / max", function()
    local n, c = make_factory()
    local p = pool.new{new = n, close = c, max = 3}
    local a = p:get(); local b = p:get()
    p:put(a)
    local sz = p:size()
    T.eq(sz.count, 2)
    T.eq(sz.idle,  1)
    T.eq(sz.max,   3)
    T.eq(sz.waiters, 0)
    p:put(b); p:close_all()
end)

s:test("get on main thread when cap-blocked returns nil, err", function()
    local n = make_factory()
    local p = pool.new{new = n, max = 1}
    local a = p:get()
    local r, err = p:get()   -- main thread, no coroutine to yield from
    T.is_nil(r); T.truthy(err); T.truthy(err:find("blocked"))
    p:put(a)
end)

os.exit(T.run(s))
