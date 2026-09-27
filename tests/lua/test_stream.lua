--[[
  test_stream.lua — M5.3 fan.stream contract tests.

  Coverage:
    - AddU/S 8/16/24/32 round-trip through GetU/S at exact bit boundaries
    - AddU30 VLQ across all four width brackets (1/2/3/5 bytes)
    - AddBytes / GetBytes preserves raw bytes including embedded NULs
    - AddString / GetString uses U30-prefixed length; empty + long strings
    - package() returns the current wire bytes
    - fan.stream.new(str) pre-fills with a payload ready to Get*
    - pos() get / pos(n) set / reset() / available() / len() introspection
    - short-read at end returns nil, "eof" without crashing
]]
local T = require("test_framework")
local stream = require("fan.stream")
local s = T.suite("fan.stream (M5)")

s:test("unsigned round-trips (8/16/24/32)", function()
  local s1 = stream.new()
  s1:AddU8(0); s1:AddU8(255)
  s1:AddU16(0); s1:AddU16(0xffff)
  s1:AddU24(0); s1:AddU24(0xffffff)
  s1:AddU32(0); s1:AddU32(0xffffffff)
  local wire = s1:package()
  local s2 = stream.new(wire)
  T.eq(s2:GetU8(),  0);          T.eq(s2:GetU8(),  255)
  T.eq(s2:GetU16(), 0);          T.eq(s2:GetU16(), 0xffff)
  T.eq(s2:GetU24(), 0);          T.eq(s2:GetU24(), 0xffffff)
  T.eq(s2:GetU32(), 0);          T.eq(s2:GetU32(), 0xffffffff)
end)

s:test("signed round-trips at signed-width boundaries", function()
  local s1 = stream.new()
  s1:AddS8(-128); s1:AddS8(127)
  s1:AddS16(-32768); s1:AddS16(32767)
  s1:AddS24(-8388608); s1:AddS24(8388607)
  s1:AddS32(-2147483648); s1:AddS32(2147483647)
  local s2 = stream.new(s1:package())
  T.eq(s2:GetS8(),  -128); T.eq(s2:GetS8(),  127)
  T.eq(s2:GetS16(), -32768); T.eq(s2:GetS16(), 32767)
  T.eq(s2:GetS24(), -8388608); T.eq(s2:GetS24(), 8388607)
  T.eq(s2:GetS32(), -2147483648); T.eq(s2:GetS32(), 2147483647)
end)

s:test("U30 VLQ round-trip across all four widths", function()
  local samples = {
    0, 1, 0x3f,               -- 1-byte form
    0x40, 0x3fff,             -- 2-byte form
    0x4000, 0x3fffff,         -- 3-byte form
    0x400000, 0xffffffff,     -- 5-byte form
  }
  for _, v in ipairs(samples) do
    local s1 = stream.new(); s1:AddU30(v)
    local s2 = stream.new(s1:package())
    T.eq(s2:GetU30(), v, "u30 round-trip failed for " .. tostring(v))
  end
end)

s:test("AddBytes / GetBytes preserves NULs and arbitrary bytes", function()
  local raw = "\0\1\2\3\255\0\0z"
  local s1 = stream.new(); s1:AddBytes(raw)
  local s2 = stream.new(s1:package())
  T.eq(s2:GetBytes(#raw), raw)
end)

s:test("AddString / GetString length-prefixed round-trip", function()
  local samples = { "", "x", "hello", string.rep("a", 100), string.rep("b", 20000) }
  for _, v in ipairs(samples) do
    local s1 = stream.new(); s1:AddString(v)
    local s2 = stream.new(s1:package())
    T.eq(s2:GetString(), v, "string len " .. #v .. " failed")
  end
end)

s:test("range checks: AddU8 rejects out-of-range values", function()
  local s1 = stream.new()
  T.error_raised(function() s1:AddU8(-1) end)
  T.error_raised(function() s1:AddU8(256) end)
  T.error_raised(function() s1:AddU30(-1) end)
end)

s:test("short read at EOF returns nil, 'eof'", function()
  local s1 = stream.new()
  local v, err = s1:GetU8()
  T.is_nil(v); T.eq(err, "eof")
  local v2, err2 = s1:GetU16()
  T.is_nil(v2); T.eq(err2, "eof")
  local s2 = stream.new("\xff")   -- only 1 byte
  local v3, err3 = s2:GetU16()    -- want 2 -> eof
  T.is_nil(v3); T.eq(err3, "eof")
end)

s:test("pos / reset / available / len introspection", function()
  local s1 = stream.new()
  s1:AddU32(0xdeadbeef); s1:AddU32(0x12345678)
  T.eq(s1:len(), 8)
  T.eq(s1:pos(), 0); T.eq(s1:available(), 8)
  T.eq(s1:GetU32(), 0xdeadbeef)
  T.eq(s1:pos(), 4); T.eq(s1:available(), 4)
  s1:reset()
  T.eq(s1:pos(), 0); T.eq(s1:available(), 8)
  s1:pos(4)
  T.eq(s1:GetU32(), 0x12345678)
end)

s:test("new(str) starts with pre-filled bytes for decoding", function()
  -- craft a payload manually: U8=42, U16=0x0102 LE => 02 01
  local s1 = stream.new("\x2a\x02\x01")
  T.eq(s1:len(), 3)
  T.eq(s1:GetU8(), 42)
  T.eq(s1:GetU16(), 0x0102)
  T.eq(s1:available(), 0)
end)

s:test("mixed encode/decode round-trip", function()
  local s1 = stream.new()
  s1:AddU8(1); s1:AddU16(0xbeef); s1:AddU30(0x123456); s1:AddString("hello, world")
  s1:AddBytes("\0\0\0")
  local s2 = stream.new(s1:package())
  T.eq(s2:GetU8(), 1)
  T.eq(s2:GetU16(), 0xbeef)
  T.eq(s2:GetU30(), 0x123456)
  T.eq(s2:GetString(), "hello, world")
  T.eq(s2:GetBytes(3), "\0\0\0")
  T.eq(s2:available(), 0)
end)

os.exit(T.run(s))
