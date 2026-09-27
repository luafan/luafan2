-- fan/pool.lua — LuaFan v2 generic resource pool (M6).
--
-- A simple LIFO pool of reusable resources with a max-size soft cap and
-- coroutine-yielding acquisition when the pool is exhausted.
--
--   local pool = require("fan.pool")
--   local p = pool.new{
--     max = 8,                     -- soft cap on concurrent live resources
--     new = function() return connect(...) end,  -- create a resource
--     close = function(r) r:close() end,          -- release a resource
--     health = function(r) return r:ping() end,   -- optional health check
--                                                 -- return truthy to keep,
--                                                 -- falsy/nil to discard
--   }
--   local r = p:get()               -- yields if the pool is exhausted
--   ... use r ...
--   p:put(r)                        -- return to pool (or discard if unhealthy)
--
--   p:with(function(r) ... end)     -- convenience wrapper; xpcall + auto-put
--   p:close_all()                   -- close every idle resource synchronously
--
-- Semantics:
--   - `new` is called lazily; the pool starts empty.
--   - `max` is a soft cap on total live resources (idle + in-use).
--     When exceeded, `get()` yields the calling coroutine until a `put()`
--     returns one. Must be called from inside a coroutine when the pool is
--     at capacity.
--   - `put(r)` runs `health(r)` if provided; on failure the resource is
--     closed and the slot is freed (count decrements) — a waiter, if any,
--     wakes up with an immediately created new resource; otherwise the
--     resource is pushed onto the idle stack.
--   - `close_all()` calls the user's `close` on every idle resource and
--     empties the idle stack. In-use resources are left alone.
--
-- Yielding scheme: since fan.loop runs coroutines cooperatively via
-- coroutine.yield / coroutine.resume, and `put()` is always called
-- synchronously from user code (never from an event callback), we can
-- park the waiter with coroutine.yield and resume it directly from
-- `put()` on the caller's stack. No libevent registration required.

local co_running = coroutine.running
local co_yield   = coroutine.yield
local co_resume  = coroutine.resume

local Pool = {}
Pool.__index = Pool

local function _push_waiter(p, co)
    local n = { co = co, next = nil }
    if p.tail then p.tail.next = n else p.head = n end
    p.tail = n
end
local function _pop_waiter(p)
    local h = p.head
    if not h then return nil end
    p.head = h.next
    if not p.head then p.tail = nil end
    return h.co
end

function Pool:get()
    -- Fast path: idle resource available.
    local n = #self.idle
    if n > 0 then
        local r = self.idle[n]
        self.idle[n] = nil
        return r
    end
    -- Room to create a new one.
    if self.count < self.max then
        self.count = self.count + 1
        local ok, r = pcall(self.new_fn)
        if not ok or r == nil then
            self.count = self.count - 1
            return nil, ("pool.new failed: " .. tostring(r))
        end
        return r
    end
    -- Exhausted: park the caller.
    local co, is_main = co_running()
    if is_main or not co then
        return nil, "fan.pool:get() blocked and no coroutine to yield from"
    end
    _push_waiter(self, co)
    local r = co_yield()
    if r == nil then return nil, "pool: get() cancelled" end
    return r
end

function Pool:put(r)
    if r == nil then
        -- caller signals the resource is dead; free the slot
        self.count = self.count - 1
        -- if a waiter is queued, try to satisfy it with a fresh resource
        local co = _pop_waiter(self)
        if co then
            self.count = self.count + 1
            local ok, nr = pcall(self.new_fn)
            if not ok or nr == nil then
                self.count = self.count - 1
                co_resume(co, nil)
                return
            end
            co_resume(co, nr)
        end
        return
    end
    -- optional health check
    if self.health_fn then
        local ok, alive = pcall(self.health_fn, r)
        if (not ok) or (not alive) then
            self:_discard(r)
            self.count = self.count - 1
            -- wake up a waiter with a fresh resource, if any
            local co = _pop_waiter(self)
            if co then
                self.count = self.count + 1
                local ok2, nr = pcall(self.new_fn)
                if not ok2 or nr == nil then
                    self.count = self.count - 1
                    co_resume(co, nil)
                    return
                end
                co_resume(co, nr)
            end
            return
        end
    end
    -- healthy: hand off to a waiter, else push to idle
    local co = _pop_waiter(self)
    if co then
        co_resume(co, r)
        return
    end
    self.idle[#self.idle + 1] = r
end

function Pool:_discard(r)
    if self.close_fn then pcall(self.close_fn, r) end
end

function Pool:with(fn, ...)
    local r, err = self:get()
    if r == nil then return nil, err end
    local ok, a, b, c = pcall(fn, r, ...)
    self:put(r)
    if not ok then return nil, a end
    return a, b, c
end

function Pool:close_all()
    for i = #self.idle, 1, -1 do
        self:_discard(self.idle[i])
        self.idle[i] = nil
        self.count = self.count - 1
    end
end

function Pool:size()
    return { count = self.count, idle = #self.idle,
             waiters = self:_num_waiters(), max = self.max }
end

function Pool:_num_waiters()
    local n, c = 0, self.head
    while c do n = n + 1; c = c.next end
    return n
end

local M = {}
function M.new(opts)
    opts = opts or {}
    assert(type(opts.new) == "function",
        "fan.pool.new: `new` function is required")
    if opts.max ~= nil then
        assert(type(opts.max) == "number" and opts.max >= 1,
            "fan.pool.new: `max` must be a positive integer")
    end
    local p = setmetatable({
        new_fn    = opts.new,
        close_fn  = opts.close,
        health_fn = opts.health,
        max       = opts.max or 8,
        count     = 0,
        idle      = {},
        head      = nil,
        tail      = nil,
    }, Pool)
    return p
end

return M
