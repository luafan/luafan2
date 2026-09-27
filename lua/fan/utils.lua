-- fan/utils.lua — LuaFan v2 miscellaneous helpers (M8).
--
-- Behavioural contract (v1 fan.utils compat):
--   - random_string(letters, count, join?, joingroupcount?)
--       Build a `count`-character string drawn uniformly from `letters`.
--       Optional `joingroupcount`: assemble groups of that many chars,
--       then join groups with `join` (like a serial-key formatter).
--   - gettime()  -> float seconds since the epoch (fan.gettime()
--       returns (sec, usec); this collapses them into one float).
--   - split(str, pat) -> array of substrings split by Lua pattern `pat`.
--       Empty leading capture is skipped so leading separators don't
--       produce spurious empty first element (v1 behaviour preserved).
--   - weakify(...) / weakify_object(target) -> proxy tables whose
--       __index / __newindex forward through a weak-valued table, so
--       the caller keeps a "handle" while the underlying object can
--       still be collected. When multiple args are passed to weakify,
--       it returns the proxies as multiple return values.
--   - LETTERS_W = alnum alphabet used by callers who want
--       random_string(m.LETTERS_W, 16).
--
-- Differences from v1 (why we rewrote):
--   - v1 has a LuaJIT FFI fast-path that reimplements gettime() via
--     gettimeofday(3). luafan2 targets stock Lua 5.3/5.4 where fan.gettime
--     is already a C function, so the FFI branch is dead code — omitted.
--   - v1 does `for _ = 1, count / joingroupcount do` in the grouped
--     random_string branch. On non-integer division that's a float loop,
--     which Lua 5.3+ allows but is semantically fuzzy. We floor the
--     quotient explicitly and validate the args (count must be a
--     non-negative multiple of joingroupcount when grouping).

local fan = require("fan")

local M = {}

-- ---------------------------------------------------------------------------
-- LETTERS_W: default alphabet used by random_string() callers who just want
-- "give me N alphanumerics". Kept as a top-level string so `#letters` is O(1).
-- ---------------------------------------------------------------------------
M.LETTERS_W = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

-- ---------------------------------------------------------------------------
-- random_string
--
-- Two modes:
--   ungrouped : count independent characters, joined by `join` (default "").
--   grouped   : joingroupcount characters per group, count/joingroupcount
--               groups, groups joined by `join` (default "").
--
-- Returns "" when count == 0 in either mode (v1 also does).
-- ---------------------------------------------------------------------------
function M.random_string(letters, count, join, joingroupcount)
    if type(letters) ~= "string" or #letters == 0 then
        error("fan.utils.random_string: `letters` must be a non-empty string", 2)
    end
    if type(count) ~= "number" or count < 0 then
        error("fan.utils.random_string: `count` must be >= 0", 2)
    end
    join = join or ""
    local L = #letters

    if type(joingroupcount) == "number" then
        if joingroupcount <= 0 or count % joingroupcount ~= 0 then
            error("fan.utils.random_string: `count` must be a positive "
                  .. "multiple of `joingroupcount`", 2)
        end
        local groups = math.floor(count / joingroupcount)
        local out = {}
        for i = 1, groups do
            local g = {}
            for j = 1, joingroupcount do
                local ri = math.random(1, L)
                g[j] = letters:sub(ri, ri)
            end
            out[i] = table.concat(g)
        end
        return table.concat(out, join)
    end

    local out = {}
    for i = 1, count do
        local ri = math.random(1, L)
        out[i] = letters:sub(ri, ri)
    end
    return table.concat(out, join)
end

-- ---------------------------------------------------------------------------
-- gettime — thin passthrough to fan.gettime() so callers who imported the
-- v1 helper keep working.
--
-- Compat note: v1's fan.gettime() returned (sec, usec) as a wall-clock
-- gettimeofday(3) pair, and this helper collapsed them into one float.
-- luafan2's fan.gettime() already returns a single monotonic float second,
-- which is what every v1 caller of utils.gettime() actually wanted
-- (differences, timeouts, throttles). Callers that need Unix epoch
-- seconds should use os.time() instead — utils.gettime() is not the
-- right primitive for that in either v1 or v2.
-- ---------------------------------------------------------------------------
function M.gettime()
    return fan.gettime()
end

-- ---------------------------------------------------------------------------
-- split — Lua-pattern split. Preserves v1's quirk of dropping a leading
-- empty capture (so split(",a,b", ",") returns {"a", "b"}, not {"", "a", "b"}).
-- Trailing content after the last separator is emitted as the final element.
-- ---------------------------------------------------------------------------
function M.split(str, pat)
    local out = {}
    if not str then return out end
    if type(str) ~= "string" then
        error("fan.utils.split: `str` must be a string or nil", 2)
    end
    if type(pat) ~= "string" or pat == "" then
        error("fan.utils.split: `pat` must be a non-empty Lua pattern", 2)
    end

    local fpat = "(.-)" .. pat
    local last_end = 1
    local s, e, cap = str:find(fpat, 1)
    while s do
        if s ~= 1 or cap ~= "" then
            out[#out + 1] = cap
        end
        last_end = e + 1
        s, e, cap = str:find(fpat, last_end)
    end
    if last_end <= #str then
        out[#out + 1] = str:sub(last_end)
    end
    return out
end

-- ---------------------------------------------------------------------------
-- weakify — return a proxy whose reads and writes hit the target through a
-- weak-valued table. Once the target has no other strong references, the
-- proxy transparently starts returning nil for every field.
--
-- Implementation:
--   proxy = {[MARKER] = {target = target}}   -- MARKER->store table is weak
--   setmetatable(proxy, {
--       __index    = function(t, k) return store.target and store.target[k] end,
--       __newindex = function(t, k, v) if store.target then store.target[k] = v end end,
--   })
--   setmetatable(store, {__mode = "v"})
--
-- Multi-arg form returns N proxies as multiple return values (via
-- table.unpack), matching v1.
-- ---------------------------------------------------------------------------
local MARKER = {}   -- unique sentinel; not exposed

local weak_index = function(self, key)
    local store = rawget(self, MARKER)
    local t = store and store.target
    return t and t[key] or nil
end

local weak_newindex = function(self, key, value)
    local store = rawget(self, MARKER)
    local t = store and store.target
    if t then t[key] = value end
end

local weak_proxy_mt = { __index = weak_index, __newindex = weak_newindex }

function M.weakify_object(target)
    local store = setmetatable({ target = target }, { __mode = "v" })
    local proxy = { [MARKER] = store }
    return setmetatable(proxy, weak_proxy_mt)
end

function M.weakify(...)
    local n = select("#", ...)
    if n <= 1 then
        return M.weakify_object((...))
    end
    local out = {}
    for i = 1, n do
        out[i] = M.weakify_object(select(i, ...))
    end
    return table.unpack(out, 1, n)
end

return M
