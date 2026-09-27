--[[
  test_fan2_util.lua — tests for the M0 fan utility surface
  (version/gettime/data2hex/hex2data/is_valid_utf8/sanitize_utf8).
  Run with the `fan` executable: fan test_fan2_util.lua
]]
local T = require("test_framework")
local fan2 = require("fan")

local s = T.suite("fan2 util (M0)")

s:test("version present", function()
  T.is_type(fan2.version(), "string")
  T.eq(fan2._VERSION, fan2.version())
end)

s:test("gettime monotonic non-decreasing", function()
  local a = fan2.gettime()
  local b = fan2.gettime()
  T.is_type(a, "number")
  T.truthy(b >= a)
end)

s:test("data2hex basic", function()
  T.eq(fan2.data2hex("ABC"), "414243")
  T.eq(fan2.data2hex(""), "")
  -- v2 uses upper-case hex (matches v1 and RFC 4648 preferred form).
  T.eq(fan2.data2hex("\0\255"), "00FF")
end)

s:test("hex2data round-trip", function()
  T.eq(fan2.hex2data("414243"), "ABC")
  local bin = "\1\2\3\250\255\0"
  T.eq(fan2.hex2data(fan2.data2hex(bin)), bin)
end)

s:test("hex2data rejects odd length", function()
  local v, err = fan2.hex2data("abc")
  T.is_nil(v)
  T.is_type(err, "string")
end)

s:test("hex2data rejects invalid digit", function()
  local v, err = fan2.hex2data("zz")
  T.is_nil(v)
  T.is_type(err, "string")
end)

s:test("hex2data accepts uppercase", function()
  T.eq(fan2.hex2data("FF"), "\255")
end)

s:test("is_valid_utf8", function()
  T.truthy(fan2.is_valid_utf8("hello"))
  T.truthy(fan2.is_valid_utf8("héllo"))       -- 2-byte
  T.truthy(fan2.is_valid_utf8("中文"))          -- 3-byte
  T.truthy(fan2.is_valid_utf8("😀"))           -- 4-byte
  T.falsy(fan2.is_valid_utf8("\255\255"))      -- invalid bytes
  T.falsy(fan2.is_valid_utf8("\xC0\x80"))      -- overlong
  T.falsy(fan2.is_valid_utf8("\xED\xA0\x80"))  -- surrogate
end)

s:test("sanitize_utf8 replaces invalid bytes", function()
  T.eq(fan2.sanitize_utf8("ok"), "ok")
  -- v2 replaces invalid bytes with U+FFFD (EF BF BD), not '?'. This matches
  -- the Unicode standard's recommended replacement character.
  T.eq(fan2.sanitize_utf8("a\255b"), "a\xEF\xBF\xBDb")
  T.truthy(fan2.is_valid_utf8(fan2.sanitize_utf8("\255mix\254中文")))
end)

-- ---- M14.C-h: v1 top-level fan.* misc helpers ------------------------------

s:test("gettop returns stack depth as integer", function()
  local top = fan2.gettop()
  T.is_type(top, "number")
  -- Positive: at least the framework's own frame lives on the stack.
  T.truthy(top >= 0)
end)

s:test("const returns unique sentinel with tostring", function()
  local a = fan2.const("EOF")
  local b = fan2.const("EOF")
  T.is_type(a, "userdata")
  T.eq(tostring(a), "const: EOF")
  -- Two calls with the same name still return distinct identities.
  -- v1 sentinels are compared by pointer, not by name.
  T.truthy(a ~= b)
  -- __metatable = false: getmetatable is opaque.
  T.eq(getmetatable(a), false)
end)

s:test("const different names produce different tostring", function()
  local a = fan2.const("A")
  local b = fan2.const("BOUNDARY")
  T.eq(tostring(a), "const: A")
  T.eq(tostring(b), "const: BOUNDARY")
end)

s:test("open / close round-trip on /dev/null", function()
  -- O_RDONLY = 0 on Linux/glibc/musl/macOS. Skip test if it isn't.
  local O_RDONLY = 0
  local fd = fan2.open("/dev/null", O_RDONLY)
  T.is_type(fd, "number")
  T.truthy(fd >= 0)
  local ok = fan2.close(fd)
  T.eq(ok, 0)
end)

s:test("open on missing path returns nil + err + errno", function()
  local fd, err, eno = fan2.open("/definitely/does/not/exist/xyz", 0)
  T.is_nil(fd)
  T.is_type(err, "string")
  T.is_type(eno, "number")
  T.truthy(eno ~= 0)
end)

s:test("close on invalid fd returns nil + err + errno (EBADF)", function()
  local ok, err, eno = fan2.close(999999)
  T.is_nil(ok)
  T.is_type(err, "string")
  T.is_type(eno, "number")
end)

s:test("getdtablesize is positive integer", function()
  local n = fan2.getdtablesize()
  T.is_type(n, "number")
  -- POSIX guarantees at least 20; every reasonable Linux/macOS box has
  -- 1024 or higher.
  T.truthy(n >= 20)
end)

os.exit(T.run(s))
