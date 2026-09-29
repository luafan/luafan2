-- webase/weakify.lua — thin proxy that wraps a target table behind a
-- weak-valued reference. Sourced from webase v1 (identical shape). Used
-- by v1 apps to hold "may be collected" references to session / cache
-- objects without keeping them alive.
--
-- Usage:
--   local weakify = require "weakify"
--   local p       = weakify(t)        -- one target -> one proxy
--   local a,b,c   = weakify(t1,t2,t3) -- N targets  -> N proxies
--
-- Reading / writing `p.field` forwards to the target while it is still
-- reachable elsewhere; once the last strong reference goes and the GC
-- runs, `p.field` reads nil and writes silently vanish.
local m = {}
local setmetatable = setmetatable

local weak_mt = {}
weak_mt.__index = function(self, key)
    local target = self[weak_mt].target
    if target then
        return target[key]
    end
end

weak_mt.__newindex = function(self, key, value)
    local target = self[weak_mt].target
    if target then
        target[key] = value
    end
end

local function weakify(target)
    local obj = {[weak_mt] = setmetatable({target = target}, {__mode = "v"})}
    setmetatable(obj, weak_mt)
    return obj
end

setmetatable(m, {
    __call = function(self, ...)
        local t = {...}
        if #t > 1 then
            local out = {}
            for i,v in ipairs(t) do
                table.insert(out, weakify(v))
            end

            return table.unpack(out)
        else
            return weakify(t[1])
        end
    end
})

return m
