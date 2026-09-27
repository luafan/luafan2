--[[
  test_objectbuf.lua — M5.2 objectbuf serializer contract tests.

  Coverage (plan \u00a74.8):
    - primitives: nil, bool, int (widths), float, string (short/mid/long)
    - table round-trip (arrays, maps, mixed)
    - nested tables preserve deep structure
    - cyclic references decode back to the same reference-identical table
    - shared subgraphs remain shared after round-trip
    - unsupported Lua types (function, coroutine, userdata) raise on encode
    - decode rejects trailing garbage and unknown tags
]]
local T = require("test_framework")
local buf = require("fan.objectbuf")

local s = T.suite("fan.objectbuf (M5)")

local function rt(v) return buf.decode(buf.encode(v)) end

s:test("primitive round-trips", function()
  T.eq(rt(nil),   nil)
  T.eq(rt(false), false)
  T.eq(rt(true),  true)
  T.eq(rt(0),     0)
  T.eq(rt(-1),    -1)
  T.eq(rt(1),     1)
  T.eq(rt(""),    "")
  T.eq(rt("x"),   "x")
end)

s:test("integer widths cover all encoded ranges", function()
  local samples = {
    0, 1, -1, 127, -128,           -- INT8 boundary
    128, -129, 32767, -32768,      -- INT16 boundary
    32768, -32769, 2147483647, -2147483648,  -- INT32 boundary
    2147483648, -2147483649, 1 << 40, -(1 << 40),  -- INT64
  }
  for _, n in ipairs(samples) do
    T.eq(rt(n), n, "int " .. tostring(n) .. " failed round-trip")
  end
end)

s:test("float round-trip via 8-byte double", function()
  local samples = { 3.14, -0.5, 1e-10, 1e15, 0.1 + 0.2 }
  for _, n in ipairs(samples) do
    T.eq(rt(n), n)
  end
end)

s:test("string widths across all length encodings", function()
  local short = string.rep("a", 250)      -- STR8
  local mid   = string.rep("b", 5000)     -- STR16
  local big   = string.rep("c", 100000)   -- STR32
  T.eq(rt(short), short)
  T.eq(rt(mid),   mid)
  T.eq(rt(big),   big)
end)

s:test("array table round-trips", function()
  local a = {10, 20, "x", true, false, 3.5}
  local dec = rt(a)
  T.eq(#dec, #a)
  for i = 1, #a do T.eq(dec[i], a[i]) end
end)

s:test("map (string-keyed) table round-trips", function()
  local m = { name = "alice", age = 30, active = true }
  local dec = rt(m)
  T.eq(dec.name, "alice"); T.eq(dec.age, 30); T.eq(dec.active, true)
end)

s:test("mixed array+map table round-trips", function()
  local x = { [1] = "a", [2] = "b", key = "val", [3] = "c", flag = true }
  local dec = rt(x)
  T.eq(dec[1], "a"); T.eq(dec[2], "b"); T.eq(dec[3], "c")
  T.eq(dec.key, "val"); T.eq(dec.flag, true)
end)

s:test("nested tables preserve deep structure", function()
  local n = { outer = { inner = { leaf = 42, items = {1, 2, 3} } } }
  local dec = rt(n)
  T.eq(dec.outer.inner.leaf, 42)
  T.eq(dec.outer.inner.items[3], 3)
end)

s:test("cyclic self-reference decodes into the same table object", function()
  local a = {}; a.self = a
  local dec = buf.decode(buf.encode(a))
  T.eq(dec.self, dec, "cycle must round-trip to the same-identity table")
end)

s:test("mutually-cyclic tables preserve shared identity", function()
  local a, b = {}, {}
  a.b = b; b.a = a
  local dec = buf.decode(buf.encode({ x = a, y = b }))
  T.eq(dec.x.b, dec.y)
  T.eq(dec.y.a, dec.x)
end)

s:test("shared subgraphs stay shared (no duplication)", function()
  local sub = { tag = "shared" }
  local top = { first = sub, second = sub }
  local dec = rt(top)
  T.eq(dec.first, dec.second, "same subgraph must decode to one table")
  T.eq(dec.first.tag, "shared")
end)

s:test("unsupported types raise on encode", function()
  T.error_raised(function() buf.encode(print) end)
  T.error_raised(function() buf.encode(coroutine.create(function() end)) end)
end)

s:test("decode surfaces error on trailing garbage", function()
  local ok, err = buf.decode(buf.encode(42) .. "\0\0")
  T.is_nil(ok); T.not_nil(err); T.truthy(err:find("trailing"))
end)

s:test("decode surfaces error on unknown tag", function()
  local ok, err = buf.decode("\xff")
  T.is_nil(ok); T.not_nil(err); T.truthy(err:find("unknown tag"))
end)

os.exit(T.run(s))
