--[[
  test_utils.lua — contract tests for fan.utils (M8).

  Coverage goals (target: >= 90% line coverage of lua/fan/utils.lua):
    - random_string: ungrouped, grouped, empty-count, error branches;
    - gettime: monotonic-ish; well-typed float;
    - split: leading/trailing sep, empty input, multi-char pattern;
    - weakify_object: reads/writes forward; GC of target -> reads nil;
    - weakify: 1-arg / N-arg forms;
    - error paths on bad arguments.
]]
local T = require("test_framework")
local U = require("fan.utils")

local s = T.suite("fan.utils (M8)")

-- ---------------------------------------------------------------------------
-- random_string
-- ---------------------------------------------------------------------------
s:test("random_string ungrouped: length + alphabet membership", function()
    math.randomseed(1)
    local out = U.random_string(U.LETTERS_W, 20)
    T.eq(#out, 20)
    for i = 1, #out do
        local ch = out:sub(i, i)
        T.truthy(U.LETTERS_W:find(ch, 1, true), "unexpected char: " .. ch)
    end
end)

s:test("random_string count=0 returns empty", function()
    T.eq(U.random_string(U.LETTERS_W, 0), "")
end)

s:test("random_string grouped: groups joined by `join`", function()
    math.randomseed(2)
    local out = U.random_string("ab", 8, "-", 4)
    -- 8/4 = 2 groups of 4 -> "xxxx-xxxx"
    T.eq(#out, 9)                                     -- 4 + 1 + 4
    T.eq(out:sub(5, 5), "-")
    T.truthy(out:match("^[ab][ab][ab][ab]%-[ab][ab][ab][ab]$"))
end)

s:test("random_string ungrouped with join", function()
    math.randomseed(3)
    local out = U.random_string("x", 3, ",")
    T.eq(out, "x,x,x")
end)

s:test("random_string LETTERS_W has 62 chars", function()
    T.eq(#U.LETTERS_W, 62)
end)

s:test("random_string rejects empty alphabet", function()
    T.error_raised(function() U.random_string("", 3) end)
end)

s:test("random_string rejects negative count", function()
    T.error_raised(function() U.random_string("a", -1) end)
end)

s:test("random_string rejects count not multiple of joingroupcount", function()
    T.error_raised(function() U.random_string("a", 5, "-", 2) end)
end)

s:test("random_string rejects non-positive joingroupcount", function()
    T.error_raised(function() U.random_string("a", 4, "-", 0) end)
end)

-- ---------------------------------------------------------------------------
-- gettime
-- ---------------------------------------------------------------------------
s:test("gettime returns positive float", function()
    local t = U.gettime()
    T.is_type(t, "number")
    T.truthy(t > 0)
end)

s:test("gettime is monotonic within a tight loop", function()
    local t0 = U.gettime()
    -- do some work; gettimeofday resolution is typ. microseconds.
    local sum = 0
    for i = 1, 100000 do sum = sum + i end
    local t1 = U.gettime()
    T.truthy(t1 >= t0)
end)

-- ---------------------------------------------------------------------------
-- split
-- ---------------------------------------------------------------------------
s:test("split by single char", function()
    local t = U.split("a,b,c", ",")
    T.eq(#t, 3)
    T.eq(t[1], "a"); T.eq(t[2], "b"); T.eq(t[3], "c")
end)

s:test("split drops leading empty capture (v1 quirk preserved)", function()
    local t = U.split(",a,b", ",")
    T.eq(#t, 2)
    T.eq(t[1], "a"); T.eq(t[2], "b")
end)

s:test("split keeps trailing content", function()
    local t = U.split("a,b,", ",")
    -- last_end is at length+1 after final "," -> nothing to append
    T.eq(#t, 2)
    T.eq(t[1], "a"); T.eq(t[2], "b")
end)

s:test("split with no separator returns single element", function()
    local t = U.split("abc", ",")
    T.eq(#t, 1)
    T.eq(t[1], "abc")
end)

s:test("split of empty string returns empty table", function()
    local t = U.split("", ",")
    T.eq(#t, 0)
end)

s:test("split of nil returns empty table", function()
    local t = U.split(nil, ",")
    T.eq(#t, 0)
end)

s:test("split with multi-char pattern", function()
    local t = U.split("a::b::c::d", "::")
    T.eq(#t, 4)
    T.eq(t[4], "d")
end)

s:test("split rejects empty pattern", function()
    T.error_raised(function() U.split("abc", "") end)
end)

s:test("split rejects non-string non-nil str", function()
    T.error_raised(function() U.split({}, ",") end)
end)

-- ---------------------------------------------------------------------------
-- weakify
-- ---------------------------------------------------------------------------
s:test("weakify_object: read/write forwards to target", function()
    local real = { x = 1 }
    local proxy = U.weakify_object(real)
    T.eq(proxy.x, 1)
    proxy.y = 42
    T.eq(real.y, 42)
end)

s:test("weakify_object: read after target is collected returns nil", function()
    local proxy
    do
        local real = { x = "alive" }
        proxy = U.weakify_object(real)
        T.eq(proxy.x, "alive")
    end
    collectgarbage("collect")
    collectgarbage("collect")
    -- After the strong reference is out of scope and GC ran twice, the
    -- weak store should have dropped the target and reads return nil.
    T.is_nil(proxy.x)
    -- writes silently no-op after collection
    proxy.newfield = 1
    T.is_nil(proxy.newfield)
end)

s:test("weakify (1 arg) behaves like weakify_object", function()
    local proxy = U.weakify({ n = 7 })
    T.eq(proxy.n, 7)
end)

s:test("weakify (N args) returns N proxies", function()
    local a, b, c = U.weakify({ v = 1 }, { v = 2 }, { v = 3 })
    T.eq(a.v, 1); T.eq(b.v, 2); T.eq(c.v, 3)
end)

os.exit(T.run(s))
