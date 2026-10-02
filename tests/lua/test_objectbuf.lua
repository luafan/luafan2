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
local fan = require("fan")
local buf = require("fan.objectbuf")

local s = T.suite("fan.objectbuf (M5)")

s:test("direct fan table uses the same v1-compatible codec", function()
  T.eq(fan.objectbuf.encode(false), "\x00")
  T.is_type(fan.objectbuf.symbol, "function")
  T.is_type(fan.objectbuf.sample, "function")
  T.is_nil(fan.objectbuf_v2)
end)

local function rt(v) return buf.decode(buf.encode(v)) end

s:test("primitive round-trips", function()
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

s:test("v1 wire golden vectors remain readable and writable", function()
  local vectors = {
    { value = false, wire = "\x00" },
    { value = true,  wire = "\x01" },
    { value = 42,    wire = "\x40\x01\x2a" },
    { value = "x",   wire = "\x20\x01\x01x" },
    { value = {},    wire = "\x08\x01\x01\x00" },
  }
  for _, tc in ipairs(vectors) do
    local decoded = buf.decode(tc.wire)
    if type(tc.value) == "table" then
      T.is_type(decoded, "table")
      T.eq(next(decoded), nil)
    else
      T.eq(decoded, tc.value)
    end
    T.eq(buf.encode(tc.value), tc.wire)
  end
end)

s:test("v1 symbol and sample APIs preserve compression workflow", function()
  T.is_type(buf.symbol, "function")
  T.is_type(buf.sample, "function")
  local value = { users = {} }
  for i = 1, 40 do
    value.users[i] = { status = "active", role = "member", region = "eu" }
  end
  local sym = buf.symbol(value)
  T.is_type(sym, "table")
  local plain = buf.encode(value)
  local packed = buf.encode(value, sym)
  T.truthy(#packed < #plain, "symbol table must reduce wire size")
  local decoded = buf.decode(packed, sym)
  T.eq(decoded.users[1].status, "active")
  T.eq(decoded.users[40].region, "eu")
  local sample = buf.sample(value, 2)
  T.is_type(sample, "table")
  T.truthy(#sample <= 2)
end)

s:test("binary-safe strings and UTF-8 round-trip", function()
  local values = { "a\0b", "\0", "\255\254", "\1\2\3", "h\195\169llo\226\134\146" }
  for _, v in ipairs(values) do
    T.eq(rt(v), v)
  end
end)

s:test("array holes leave a contiguous array part plus an explicit key", function()
  local v = { 1, [3] = 3 }
  local dec = rt(v)
  T.eq(dec[1], 1)
  T.is_nil(dec[2])
  T.eq(dec[3], 3)
end)

s:test("tables may be used as keys and survive round-trip", function()
  local key = { 1, 2 }
  local v = { [key] = "x" }
  local dec = buf.decode(buf.encode(v))
  local found
  for k, val in pairs(dec) do
    if type(k) == "table" then found = val end
  end
  T.eq(found, "x")
end)

s:test("deeply nested tables round-trip", function()
  local v = { leaf = 1 }
  for _ = 1, 30 do v = { n = v } end
  local dec = rt(v)
  for _ = 1, 30 do dec = dec.n end
  T.eq(dec.leaf, 1)
end)

s:test("decode rejects empty and truncated input", function()
  local v1, e1 = buf.decode("")
  T.is_nil(v1)
  T.not_nil(e1)
  local v2 = buf.decode("\x40")           -- U30 flag without a count
  T.is_nil(v2)
  local v3 = buf.decode("\x20\x01\x05ab") -- string length exceeds payload
  T.is_nil(v3)
end)

s:test("sample ranks the most frequent values first", function()
  local value = { "a", "a", "a", "b", "b", "c", 7, 7, 7, 7 }
  local sample = buf.sample(value, 2)
  T.eq(#sample, 2)
  T.eq(sample[1], 7)
  T.eq(sample[2], "a")
end)

s:test("extended integer boundaries round-trip as u30 or D64", function()
  local values = {
    2, 126, 127, 128, 129, 254, 255, 256, 257,
    16382, 16383, 16384, 16385, 2097151, 2097152,
    268435455, 268435456, 4294967294, 4294967295,
    -1, -127, -128, -129, -255, -256, -32767, -32768, -32769,
    -2147483647, -2147483648, -2147483649,
    4294967296, 4294967297, 1099511627776, 9007199254740992,
  }
  for _, n in ipairs(values) do
    T.eq(rt(n), n, "int " .. tostring(n))
  end
end)

os.exit(T.run(s))
