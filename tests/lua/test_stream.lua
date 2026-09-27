--[[
  test_stream.lua — fan.stream contract tests (M5 + M14.C-i v1 parity).

  Coverage:
    - AddU/S 8/16/24/32 round-trip at exact bit boundaries
    - AddU30 LEB128 (v1 wire-compatible) across all width brackets
    - AddD64 IEEE-754 8-byte round-trip, plus wire endian check
    - AddBytes / GetBytes preserves raw bytes including embedded NULs
    - GetBytes() / GetBytes(nil) reads everything remaining
    - TestBytes(n) / TestBytes() peeks without moving the cursor
    - AddString / GetString uses U30-prefixed length; empty + long strings
    - GetString short-read returns (nil, expected_length) matching v1
    - package() returns the current wire bytes
    - fan.stream.new(str) pre-fills with a payload ready to Get*
    - pos() get / pos(n) set / available() / len() introspection
    - mark() / reset() v1 semantics: reset rewinds to the last mark (or 0)
    - empty() clears the buffer + cursors
    - readline() extracts \r / \n / \r\n terminated lines, restores pos on
      incomplete input, and coexists with subsequent Get* calls
    - prepare_add / prepare_get switch between append / read modes
    - v1 aliases GetABCU32 / AddABCU32 / GetABCS32 / AddABCS32 map to U30
    - __tostring reports "<fan.stream available=N>"
    - short-read at end returns nil, "eof" without crashing
]]
local T = require("test_framework")
local stream = require("fan.stream")
local s = T.suite("fan.stream (M5 + M14 v1 parity)")

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

s:test("U30 LEB128 round-trip across all width brackets", function()
  -- v1-compatible LEB128 boundaries: 128 => 2B, 16384 => 3B, 2097152 => 4B,
  -- 0x10000000 (268435456) => 5B. Highest 32-bit value fits in 5 bytes.
  local samples = {
    0, 1, 0x7f,                       -- 1-byte form
    0x80, 0x3fff,                     -- 2-byte form
    0x4000, 0x1fffff,                 -- 3-byte form
    0x200000, 0xfffffff,              -- 4-byte form
    0x10000000, 0xffffffff,           -- 5-byte form
  }
  for _, v in ipairs(samples) do
    local s1 = stream.new(); s1:AddU30(v)
    local s2 = stream.new(s1:package())
    T.eq(s2:GetU30(), v, "u30 round-trip failed for " .. tostring(v))
  end
end)

s:test("U30 LEB128 wire boundaries match v1 exactly", function()
  local widths = {
    {val = 0,          bytes = 1},
    {val = 127,        bytes = 1},
    {val = 128,        bytes = 2},
    {val = 16383,      bytes = 2},
    {val = 16384,      bytes = 3},
    {val = 2097151,    bytes = 3},
    {val = 2097152,    bytes = 4},
    {val = 268435455,  bytes = 4},
    {val = 268435456,  bytes = 5},
    {val = 0xffffffff, bytes = 5},
  }
  for _, w in ipairs(widths) do
    local s1 = stream.new(); s1:AddU30(w.val)
    local wire = s1:package()
    T.eq(#wire, w.bytes,
      string.format("U30(%d): expected %d bytes, got %d", w.val, w.bytes, #wire))
  end
  -- Spot-check exact bytes for the canonical v1 example (128 -> 0x80 0x01).
  local s2 = stream.new(); s2:AddU30(128)
  T.eq(s2:package(), "\x80\x01", "U30(128) wire must be 0x80 0x01 for v1 compat")
end)

s:test("AddD64 / GetD64 round-trip (finite + special values)", function()
  local samples = { 0.0, -0.0, 1.0, -1.0, 3.14159265358979, 2.718281828,
                    1e-300, 1e300, 1/0, -1/0 }
  for _, v in ipairs(samples) do
    local s1 = stream.new(); s1:AddD64(v)
    local s2 = stream.new(s1:package())
    local got = s2:GetD64()
    if v ~= v then
      T.truthy(got ~= got, "NaN round-trip must stay NaN")
    else
      T.eq(got, v, "D64 round-trip failed for " .. tostring(v))
    end
  end
  -- NaN payload preservation (0/0 produces a quiet NaN on IEEE-754)
  local nan = 0/0
  local s1 = stream.new(); s1:AddD64(nan)
  local s2 = stream.new(s1:package())
  local g = s2:GetD64(); T.truthy(g ~= g, "NaN stays NaN through D64")
end)

s:test("D64 wire size is 8 bytes little-endian", function()
  local s1 = stream.new(); s1:AddD64(1.0)
  local wire = s1:package()
  T.eq(#wire, 8, "D64 must occupy 8 bytes")
  -- IEEE-754 for 1.0: 0x3ff0000000000000; little-endian => 00 00 00 00 00 00 f0 3f
  T.eq(wire, "\x00\x00\x00\x00\x00\x00\xf0\x3f", "D64(1.0) little-endian wire")
end)

s:test("AddBytes / GetBytes preserves NULs and arbitrary bytes", function()
  local raw = "\0\1\2\3\255\0\0z"
  local s1 = stream.new(); s1:AddBytes(raw)
  local s2 = stream.new(s1:package())
  T.eq(s2:GetBytes(#raw), raw)
end)

s:test("GetBytes() with no arg / nil returns all remaining", function()
  -- Craft a known payload so the LE encoding of AddU16 doesn't confuse the
  -- assertion. `stream.new(str)` seeds the buffer with raw bytes ready to Get*.
  local s1 = stream.new("\x2a\x03\x04\x05")
  T.eq(s1:GetU8(), 0x2a)
  T.eq(s1:GetBytes(), "\x03\x04\x05")    -- no arg = all remaining
  T.eq(s1:available(), 0)
  local s2 = stream.new("\x11\x22")
  T.eq(s2:GetBytes(nil), "\x11\x22")     -- explicit nil = all
  local s3 = stream.new()
  T.eq(s3:GetBytes(), "")                -- empty stream: all-of-nothing is ""
end)

s:test("TestBytes(n) peeks without moving the cursor", function()
  local s1 = stream.new("hello world")
  T.eq(s1:TestBytes(5), "hello")
  T.eq(s1:pos(), 0, "TestBytes must not advance the read cursor")
  T.eq(s1:GetBytes(5), "hello")
  T.eq(s1:pos(), 5)
  T.eq(s1:TestBytes(), " world")         -- no arg = all remaining
  T.eq(s1:pos(), 5, "TestBytes() no-arg must not advance either")
  -- clamp when n > available
  T.eq(s1:TestBytes(100), " world")
end)

s:test("TestBytes on empty / at EOF returns nothing", function()
  local s1 = stream.new()
  T.is_nil(s1:TestBytes(5))
  T.is_nil(s1:TestBytes())
  local s2 = stream.new("ab"); s2:GetBytes(2)
  T.is_nil(s2:TestBytes(1), "at-EOF peek returns nil (v1: 0 return values)")
end)

s:test("AddString / GetString length-prefixed round-trip", function()
  local samples = { "", "x", "hello", string.rep("a", 100), string.rep("b", 20000) }
  for _, v in ipairs(samples) do
    local s1 = stream.new(); s1:AddString(v)
    local s2 = stream.new(s1:package())
    T.eq(s2:GetString(), v, "string len " .. #v .. " failed")
  end
end)

s:test("GetString short-read returns (nil, expected_length) — v1 shape", function()
  -- craft a wire header saying "declared 100 bytes follow" but truncate.
  local s1 = stream.new(); s1:AddString(string.rep("x", 100))
  local full = s1:package()
  local truncated = full:sub(1, 5)       -- keep u30 header + a few bytes
  local s2 = stream.new(truncated)
  local v, expected = s2:GetString()
  T.is_nil(v)
  T.eq(type(expected), "number")
  T.truthy(expected > 0, "expected_length must be a positive hint")
  -- With no header at all, v1 reports (nil, available+1). Empty stream -> 1.
  local s3 = stream.new()
  local v3, e3 = s3:GetString(); T.is_nil(v3); T.eq(e3, 1)
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

s:test("pos / available / len introspection", function()
  local s1 = stream.new()
  s1:AddU32(0xdeadbeef); s1:AddU32(0x12345678)
  T.eq(s1:len(), 8)
  T.eq(s1:pos(), 0); T.eq(s1:available(), 8)
  T.eq(s1:GetU32(), 0xdeadbeef)
  T.eq(s1:pos(), 4); T.eq(s1:available(), 4)
  s1:pos(4)
  T.eq(s1:GetU32(), 0x12345678)
end)

s:test("mark() / reset() rewind read cursor to last mark (v1 semantics)", function()
  local s1 = stream.new("ABCDEFGH")
  T.eq(s1:GetU8(), string.byte("A"))
  T.eq(s1:GetU8(), string.byte("B"))
  s1:mark()                              -- mark at pos=2
  T.eq(s1:GetU8(), string.byte("C"))
  T.eq(s1:GetU8(), string.byte("D"))
  s1:reset()                             -- rewind to mark
  T.eq(s1:pos(), 2)
  T.eq(s1:GetU8(), string.byte("C"))     -- re-read starts at mark
end)

s:test("reset() before any mark rewinds to 0 (initial mark)", function()
  local s1 = stream.new("XYZ")
  T.eq(s1:GetU8(), string.byte("X"))
  T.eq(s1:GetU8(), string.byte("Y"))
  s1:reset()                             -- no mark yet; initial mark = 0
  T.eq(s1:pos(), 0)
  T.eq(s1:GetU8(), string.byte("X"))
end)

s:test("empty() clears buffer + cursors", function()
  local s1 = stream.new()
  s1:AddU32(0xdeadbeef)
  T.eq(s1:len(), 4); T.eq(s1:available(), 4)
  s1:empty()
  T.eq(s1:len(), 0); T.eq(s1:available(), 0); T.eq(s1:pos(), 0)
  -- After empty(), the stream can be reused as a fresh write buffer.
  s1:AddU16(0xcafe)
  local s2 = stream.new(s1:package())
  T.eq(s2:GetU16(), 0xcafe)
end)

s:test("readline() consumes \\r \\n and \\r\\n terminated lines", function()
  local s1 = stream.new("first\r\nsecond\nthird\r")
  local line, brk = s1:readline(); T.eq(line, "first");  T.eq(brk, "\r\n")
  line, brk = s1:readline();       T.eq(line, "second"); T.eq(brk, "\n")
  line, brk = s1:readline();       T.eq(line, "third");  T.eq(brk, "\r")
  T.is_nil(s1:readline(), "empty tail returns nil")
end)

s:test("readline() on incomplete line rewinds without consuming", function()
  local s1 = stream.new("no-terminator-here")
  local before = s1:pos()
  T.is_nil(s1:readline())
  T.eq(s1:pos(), before, "read cursor must be restored on failure")
  T.eq(s1:available(), #"no-terminator-here")
end)

s:test("readline() coexists with subsequent Get* calls", function()
  local s1 = stream.new()
  s1:AddBytes("hello\n"); s1:AddU8(0x2a); s1:AddBytes("world\r\n")
  local s2 = stream.new(s1:package())
  T.eq(s2:readline(), "hello")
  T.eq(s2:GetU8(), 0x2a)
  T.eq(s2:readline(), "world")
  T.is_nil(s2:readline())
end)

s:test("prepare_add / prepare_get switch between append and read modes", function()
  local s1 = stream.new()
  s1:AddU8(0x11); s1:AddU8(0x22); s1:AddU8(0x33); s1:AddU8(0x44)
  s1:prepare_get()                       -- rewind read cursor
  T.eq(s1:GetU8(), 0x11); T.eq(s1:GetU8(), 0x22)
  -- Now switch to append mode: the two unread bytes should be preserved and
  -- new writes appended after them.
  s1:prepare_add()
  s1:AddU8(0x55)
  s1:prepare_get()
  T.eq(s1:GetU8(), 0x33)                 -- old unread first
  T.eq(s1:GetU8(), 0x44)
  T.eq(s1:GetU8(), 0x55)                 -- new appended last
end)

s:test("new(str) starts with pre-filled bytes for decoding", function()
  -- craft a payload manually: U8=42, U16=0x0102 LE => 02 01
  local s1 = stream.new("\x2a\x02\x01")
  T.eq(s1:len(), 3)
  T.eq(s1:GetU8(), 42)
  T.eq(s1:GetU16(), 0x0102)
  T.eq(s1:available(), 0)
end)

s:test("v1 aliases: ABC* map to U30", function()
  local s1 = stream.new()
  s1:AddABCU32(0x12345678); s1:AddABCS32(0x00)
  local s2 = stream.new(s1:package())
  T.eq(s2:GetABCU32(), 0x12345678)
  T.eq(s2:GetABCS32(), 0x00)
end)

s:test("__tostring reports available bytes", function()
  local s1 = stream.new("hello")
  T.eq(tostring(s1), "<fan.stream available=5>")
  s1:GetU8()
  T.eq(tostring(s1), "<fan.stream available=4>")
end)

s:test("mixed encode/decode round-trip", function()
  local s1 = stream.new()
  s1:AddU8(1); s1:AddU16(0xbeef); s1:AddU30(0x123456); s1:AddString("hello, world")
  s1:AddBytes("\0\0\0"); s1:AddD64(1.5)
  local s2 = stream.new(s1:package())
  T.eq(s2:GetU8(), 1)
  T.eq(s2:GetU16(), 0xbeef)
  T.eq(s2:GetU30(), 0x123456)
  T.eq(s2:GetString(), "hello, world")
  T.eq(s2:GetBytes(3), "\0\0\0")
  T.eq(s2:GetD64(), 1.5)
  T.eq(s2:available(), 0)
end)

os.exit(T.run(s))
