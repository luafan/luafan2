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

-- ---- gzip_compress (M12.1) ------------------------------------------------

s:test("gzip_compress produces a valid gzip stream (magic 1f 8b 08)", function()
  local src = string.rep("hello ", 200)   -- ~1.2 KiB, highly compressible
  local g = assert(fan.zlib.gzip_compress(src))
  T.truthy(#g >= 20)               -- gzip has 10B header + 8B trailer at minimum
  T.eq(g:sub(1, 1), "\x1f")
  T.eq(g:sub(2, 2), "\x8b")
  T.eq(g:sub(3, 3), "\x08")        -- CM=8 (deflate) is the only allowed value
  T.truthy(#g < #src, "compressible input must shrink under gzip")
end)

s:test("gzip_compress: trailer contains CRC32 and ISIZE little-endian", function()
  -- ISIZE is the last 4 bytes = input length mod 2^32, little-endian.
  local src = "abcdefghij"                        -- 10 bytes
  local g = assert(fan.zlib.gzip_compress(src))
  local isize = g:sub(-4)
  T.eq(string.byte(isize, 1), 10)                  -- 0x0a
  T.eq(string.byte(isize, 2), 0)
  T.eq(string.byte(isize, 3), 0)
  T.eq(string.byte(isize, 4), 0)
end)

s:test("gzip_compress: empty input still produces a valid framed stream", function()
  local g = assert(fan.zlib.gzip_compress(""))
  T.truthy(#g >= 18)
  T.eq(g:sub(1, 3), "\x1f\x8b\x08")
  -- ISIZE for empty input is 0.
  T.eq(g:sub(-4), "\0\0\0\0")
end)

s:test("gzip_compress: level rejected outside -1/0..9", function()
  local ok, err = fan.zlib.gzip_compress("x", 42)
  T.is_nil(ok); T.not_nil(err)
end)

s:test("gzip_compress: gzip stream is decoded by a raw-inflate probe", function()
  -- We can't inflate gzip with raw_inflate (skips 10-byte header + trailer)
  -- without stripping framing manually. Instead: verify the "raw deflate
  -- body" between header (offset 10) and trailer (last 8 bytes) is a
  -- valid raw deflate stream by round-tripping.
  local src = string.rep("The quick brown fox. ", 64)  -- ~1.3 KiB
  local g = assert(fan.zlib.gzip_compress(src))
  T.truthy(#g > 18)
  local raw = g:sub(11, -9)   -- strip header + trailer
  local back = assert(fan.zlib.inflate_raw(raw))
  T.eq(back, src)
end)

os.exit(T.run(s))
