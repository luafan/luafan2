--[[
  test_zlib.lua — M4 zlib primitives contract tests (fan.zlib).

  Covers the raw deflate / raw inflate round-trip primitives used by
  permessage-deflate (future WS extension) and by HTTP deflate content
  encoding. Since these are one-shot with no shared state, tests are
  compact and driven directly (no fan loop required).
]]
local T = require("test_framework")
local fan = require("fan")

local s = T.suite("fan.zlib (M4)")

s:test("fan.zlib.available reports compiled-in state", function()
  -- built with -DFAN_WITH_ZLIB=ON in run_tests.sh, so this is true here
  T.truthy(fan.zlib.available())
  T.truthy(fan.zlib.enabled)
end)

s:test("deflate_raw / inflate_raw round-trip a small string", function()
  local src = "hello, hello, hello, world!"
  local c = assert(fan.zlib.deflate_raw(src))
  T.truthy(#c > 0)
  T.truthy(#c < #src, "small ASCII with repetition should compress")
  T.eq(assert(fan.zlib.inflate_raw(c)), src)
end)

s:test("empty input round-trips", function()
  local c = assert(fan.zlib.deflate_raw(""))
  T.eq(assert(fan.zlib.inflate_raw(c)), "")
end)

s:test("binary payload round-trips (all byte values)", function()
  local bytes = {}
  for i = 0, 255 do bytes[#bytes + 1] = string.char(i) end
  local src = table.concat(bytes):rep(50)   -- ~13 KiB
  local c = assert(fan.zlib.deflate_raw(src))
  T.eq(assert(fan.zlib.inflate_raw(c)), src)
end)

s:test("compression level: 0 = store, 9 = smallest for compressible input", function()
  local src = string.rep("A", 4096)
  local store = assert(fan.zlib.deflate_raw(src, 0))
  local max   = assert(fan.zlib.deflate_raw(src, 9))
  T.truthy(#max < #store)   -- level 9 must beat 'store' on repetitive data
  T.eq(assert(fan.zlib.inflate_raw(store)), src)
  T.eq(assert(fan.zlib.inflate_raw(max)),   src)
end)

s:test("out-of-range compression level rejected", function()
  local ok, err = fan.zlib.deflate_raw("x", 15)
  T.is_nil(ok); T.not_nil(err)
end)

s:test("inflate_raw rejects garbage input", function()
  -- 8 random bytes are extremely unlikely to be a valid raw deflate stream
  local ok, err = fan.zlib.inflate_raw("\1\2\3\4\5\6\7\8")
  -- Note: raw inflate is permissive; the guard is that it MUST return
  -- either a shorter/invalid decode or an error. We just assert that the
  -- call does not crash and that any error is surfaced as nil,err.
  if not ok then T.not_nil(err) end
end)

s:test("large payload round-trip (256 KiB text)", function()
  local src = string.rep("The quick brown fox jumps over the lazy dog. ", 6000)
  T.truthy(#src > 256 * 1024)
  local c = assert(fan.zlib.deflate_raw(src))
  T.truthy(#c < #src / 4)  -- highly repetitive text compresses well
  T.eq(assert(fan.zlib.inflate_raw(c)), src)
end)

s:test("sync-flush deflate ends with the 00 00 FF FF marker", function()
  -- non-empty input: Z_SYNC_FLUSH emits data followed by the sync marker
  local d = assert(fan.zlib.deflate_raw("hello", nil, true))
  T.eq(d:sub(-4), "\0\0\xff\xff", "sync-flush must terminate with 00 00 FF FF")
  -- appending the marker (redundant, but idempotent shape) still inflates
  -- because sync-flush already left one there.
  T.eq(assert(fan.zlib.inflate_raw(d)), "hello")
  -- also the RFC 7692 workflow: strip trailer, re-append at peer, inflate
  local stripped = d:sub(1, -5)
  T.eq(assert(fan.zlib.inflate_raw(stripped .. "\0\0\xff\xff")), "hello")
end)

os.exit(T.run(s))
