--[[
  test_compat.lua — M7 fan.compat v1 API alias shim tests.

  After `require("fan.compat")` the top-level POSIX helpers (fan.getpid,
  fan.fork, fan.kill, ...) are available as v1-style aliases. Verifies
  the shim is idempotent, exposes signals/wait, and preserves the v2
  hardening (kill still refuses dangerous defaults).
]]
local T = require("test_framework")

local s = T.suite("fan.compat (M7)")

s:test("apply(): flat top-level POSIX names appear on fan", function()
    local fan = require("fan")
    -- prior to compat: fan.getpid may or may not exist depending on load
    -- order; require compat and verify all v1 flat names are present.
    require("fan.compat")
    T.is_type(fan.getpid, "function")
    T.is_type(fan.fork, "function")
    T.is_type(fan.kill, "function")
    T.is_type(fan.waitpid, "function")
    T.is_type(fan.setpgid, "function")
    T.is_type(fan.setsid, "function")
    T.is_type(fan.getcpucount, "function")
    T.is_type(fan.getinterfaces, "function")
    T.is_type(fan.setprogname, "function")
    T.is_type(fan.signals, "table")
    T.is_type(fan.wait, "table")
end)

s:test("fan.getpid() (v1 name) delegates to posix.getpid", function()
    local fan = require("fan")
    require("fan.compat")
    local a = fan.getpid()
    local b = fan.posix.getpid()
    T.eq(a, b)
end)

s:test("fan.kill (v1 name) preserves v2 dangerous-PID guardrail", function()
    local fan = require("fan")
    require("fan.compat")
    local ok, err = fan.kill(-1, fan.signals.SIGTERM)
    T.is_nil(ok); T.truthy(err); T.truthy(err:find("refused"))
end)

s:test("compat is idempotent (apply twice is a no-op)", function()
    local fan = require("fan")
    local M = require("fan.compat")
    local before = fan.getpid
    M.apply()
    M.apply()
    T.eq(fan.getpid, before)
end)

----------------------------------------------------------------------
-- v1 flat data-encoding + UTF-8 helpers
----------------------------------------------------------------------

s:test("fan.data2hex / fan.hex2data: v1 byte-for-byte round-trip", function()
    local fan = require("fan")
    require("fan.compat")
    T.is_type(fan.data2hex, "function")
    T.is_type(fan.hex2data, "function")
    -- Empty
    T.eq(fan.data2hex(""), "")
    T.eq(fan.hex2data(""), "")
    -- v1 test fixture: "Hello" <-> "48656C6C6F" (upper-case)
    T.eq(fan.data2hex("Hello"), "48656C6C6F")
    T.eq(fan.hex2data("48656C6C6F"), "Hello")
    -- All byte values 0..255 round-trip
    local all = {}
    for i = 0, 255 do all[#all + 1] = string.char(i) end
    local raw = table.concat(all)
    local hex = fan.data2hex(raw)
    T.eq(#hex, 512)
    T.eq(fan.hex2data(hex), raw)
end)

s:test("fan.data2hex uses upper-case hex digits", function()
    require("fan.compat")
    local fan = require("fan")
    T.eq(fan.data2hex("\xAB\xCD\xEF"), "ABCDEF")
    T.eq(fan.data2hex("\x0A\x0B\x0F"), "0A0B0F")
end)

s:test("fan.hex2data: lowercase / mixed-case accepted (v1 strtol)", function()
    require("fan.compat")
    local fan = require("fan")
    T.eq(fan.hex2data("abcdef"), "\xAB\xCD\xEF")
    T.eq(fan.hex2data("AbCdEf"), "\xAB\xCD\xEF")
end)

s:test("fan.hex2data: STRICT rejects odd length and invalid hex chars", function()
    -- v2 diverges from v1's silent-truncation / strtol "invalid -> 0" quirks:
    -- those were footguns that silently corrupted callers' data. v2 returns
    -- (nil, message) instead so callers can branch. Empty and even-length
    -- valid hex continue to work identically.
    require("fan.compat")
    local fan = require("fan")
    -- Odd length -> (nil, "hex string length must be even")
    local r, err = fan.hex2data("ABC")
    T.is_nil(r)
    T.is_type(err, "string")
    T.truthy(err:find("even"), "expected 'even' in error, got: " .. tostring(err))
    -- Invalid hex char at even offset
    r, err = fan.hex2data("XY")
    T.is_nil(r)
    T.is_type(err, "string")
    T.truthy(err:find("invalid hex"), "expected 'invalid hex' in error, got: " .. tostring(err))
    -- Invalid hex char at odd offset
    r, err = fan.hex2data("AX")
    T.is_nil(r)
    T.is_type(err, "string")
    -- Odd length takes precedence over invalid chars.
    r, err = fan.hex2data("XYZ")
    T.is_nil(r)
    T.truthy(err:find("even"), "odd-length error should win, got: " .. tostring(err))
    -- Empty string is still valid and returns "".
    T.eq(fan.hex2data(""), "")
end)

s:test("fan.is_valid_utf8: ASCII + multi-byte + edge cases", function()
    require("fan.compat")
    local fan = require("fan")
    T.is_type(fan.is_valid_utf8, "function")
    -- Empty and nil are valid.
    T.eq(fan.is_valid_utf8(""), true)
    T.eq(fan.is_valid_utf8(nil), true)
    -- Plain ASCII
    T.eq(fan.is_valid_utf8("hello world"), true)
    -- Two-byte (é = C3 A9) and three-byte (中 = E4 B8 AD) code units.
    T.eq(fan.is_valid_utf8("caf\xC3\xA9"), true)
    T.eq(fan.is_valid_utf8("\xE4\xB8\xAD"), true)
    -- Four-byte 😀 = F0 9F 98 80
    T.eq(fan.is_valid_utf8("\xF0\x9F\x98\x80"), true)
    -- Stray continuation byte -> invalid.
    T.eq(fan.is_valid_utf8("\x80"), false)
    -- Overlong 2-byte lead 0xC1 -> invalid.
    T.eq(fan.is_valid_utf8("\xC1\xA0"), false)
    -- Truncated multi-byte at end of string.
    T.eq(fan.is_valid_utf8("\xE4\xB8"), false)
    -- Surrogate half (ED A0 80) -> invalid.
    T.eq(fan.is_valid_utf8("\xED\xA0\x80"), false)
    -- Out-of-range 4-byte lead (0xF5) -> invalid.
    T.eq(fan.is_valid_utf8("\xF5\x80\x80\x80"), false)
end)

s:test("fan.sanitize_utf8: pass-through on valid, U+FFFD replacement on invalid", function()
    require("fan.compat")
    local fan = require("fan")
    T.is_type(fan.sanitize_utf8, "function")
    -- Valid strings are returned unchanged.
    T.eq(fan.sanitize_utf8(""), "")
    T.eq(fan.sanitize_utf8("hello"), "hello")
    T.eq(fan.sanitize_utf8("caf\xC3\xA9"), "caf\xC3\xA9")
    -- A single stray continuation byte is replaced by FFFD (EF BF BD).
    T.eq(fan.sanitize_utf8("\x80"), "\xEF\xBF\xBD")
    -- Valid prefix is preserved, invalid byte replaced, valid tail kept.
    T.eq(fan.sanitize_utf8("ok\x80done"), "ok\xEF\xBF\xBDdone")
    -- Two consecutive bad bytes -> two FFFD (v1 emits one per bad byte).
    T.eq(fan.sanitize_utf8("\x80\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD")
    -- Truncated multi-byte at EOF -> lead byte replaced.
    T.eq(fan.sanitize_utf8("a\xE4\xB8"), "a\xEF\xBF\xBD\xEF\xBF\xBD")
end)

s:test("fan.data2hex / hex2data reject non-string, accept number (v1 lua_isstring)", function()
    require("fan.compat")
    local fan = require("fan")
    -- v1 uses lua_isstring which is TRUE for numbers (Lua coerces them to
    -- their decimal string form before consumption). Match that exactly.
    T.eq(fan.data2hex(42), "3432")     -- "42" -> "3432"
    T.eq(fan.hex2data("3432"), "42")
    -- Non-coercible types return nil (v1: `return 0` from the C entry).
    T.is_nil(fan.data2hex(nil))
    T.is_nil(fan.hex2data(nil))
    T.is_nil(fan.data2hex(true))
    T.is_nil(fan.hex2data({}))
end)

----------------------------------------------------------------------
-- Pure-Lua fallback implementations exposed via the module table.
-- These are byte-identical shadows of the C entries and act as a
-- fallback if a build ever ships luafan.c without the flat helpers.
-- Exercise them directly so callers relying on the module surface
-- (and the fallback path itself) stay covered.
----------------------------------------------------------------------

s:test("compat.M.data2hex: pure-Lua fallback matches fan.data2hex", function()
    local compat = require("fan.compat")
    local fan = require("fan")
    T.is_type(compat.data2hex, "function")
    -- Byte-identical shadow of the C entry across every input we test on `fan`.
    T.eq(compat.data2hex(""), "")
    T.eq(compat.data2hex("Hello"), "48656C6C6F")
    T.eq(compat.data2hex("\xAB\xCD\xEF"), "ABCDEF")
    T.eq(compat.data2hex("\x00\x01\x0F\xA0"), "00010FA0")
    -- Round-trip every byte 0..255.
    local all = {}
    for i = 0, 255 do all[#all + 1] = string.char(i) end
    local raw = table.concat(all)
    T.eq(compat.data2hex(raw), fan.data2hex(raw))
    -- Number coercion parity.
    T.eq(compat.data2hex(42), "3432")
    -- Non-string rejection.
    T.is_nil(compat.data2hex(nil))
    T.is_nil(compat.data2hex(true))
    T.is_nil(compat.data2hex({}))
end)

s:test("compat.M.hex2data: pure-Lua fallback matches fan.hex2data (incl. errors)", function()
    local compat = require("fan.compat")
    local fan = require("fan")
    T.is_type(compat.hex2data, "function")
    -- Happy paths.
    T.eq(compat.hex2data(""), "")
    T.eq(compat.hex2data("48656C6C6F"), "Hello")
    T.eq(compat.hex2data("abcdef"), "\xAB\xCD\xEF")
    T.eq(compat.hex2data("AbCdEf"), "\xAB\xCD\xEF")
    -- Full byte round-trip parity with C.
    for b = 0, 255 do
        local hex = string.format("%02X", b)
        T.eq(compat.hex2data(hex), string.char(b))
        T.eq(compat.hex2data(hex), fan.hex2data(hex))
    end
    -- STRICT rejections (error branches).
    local r, err = compat.hex2data("ABC")
    T.is_nil(r); T.is_type(err, "string")
    T.truthy(err:find("even"))
    r, err = compat.hex2data("XY")
    T.is_nil(r); T.is_type(err, "string")
    T.truthy(err:find("invalid hex"))
    -- Invalid nibble at odd offset (covers the `hi < 0 ? i : i + 1` branch).
    r, err = compat.hex2data("AX")
    T.is_nil(r); T.truthy(err:find("invalid hex"))
    -- Number coercion parity.
    T.eq(compat.hex2data("3432"), "42")
    -- Non-coercible input.
    T.is_nil(compat.hex2data(nil))
    T.is_nil(compat.hex2data(true))
    T.is_nil(compat.hex2data({}))
end)

s:test("compat.M.is_valid_utf8: pure-Lua fallback matches fan.is_valid_utf8", function()
    local compat = require("fan.compat")
    T.is_type(compat.is_valid_utf8, "function")
    -- Valid inputs (each exercises a different lead-byte class).
    T.eq(compat.is_valid_utf8(nil), true)
    T.eq(compat.is_valid_utf8(""), true)
    T.eq(compat.is_valid_utf8("ascii only"), true)
    T.eq(compat.is_valid_utf8("caf\xC3\xA9"), true)          -- 2-byte
    T.eq(compat.is_valid_utf8("\xE4\xB8\xAD"), true)          -- 3-byte
    T.eq(compat.is_valid_utf8("\xE0\xA0\x80"), true)          -- 3-byte lead 0xE0 min
    T.eq(compat.is_valid_utf8("\xED\x9F\xBF"), true)          -- 3-byte lead 0xED just below surrogate
    T.eq(compat.is_valid_utf8("\xF0\x9F\x98\x80"), true)      -- 4-byte 😀
    T.eq(compat.is_valid_utf8("\xF4\x8F\xBF\xBF"), true)      -- 4-byte max U+10FFFF
    -- Invalid inputs (each exercises a distinct rejection branch).
    T.eq(compat.is_valid_utf8("\x80"), false)                 -- stray continuation
    T.eq(compat.is_valid_utf8("\xC1\xA0"), false)             -- overlong 2-byte
    T.eq(compat.is_valid_utf8("\xC2"), false)                 -- truncated 2-byte
    T.eq(compat.is_valid_utf8("\xC2\x00"), false)             -- bad 2-byte cont
    T.eq(compat.is_valid_utf8("\xE0\x80\x80"), false)         -- overlong 3-byte (E0 lead)
    T.eq(compat.is_valid_utf8("\xED\xA0\x80"), false)         -- surrogate (ED lead)
    T.eq(compat.is_valid_utf8("\xE4\xB8"), false)             -- truncated 3-byte
    T.eq(compat.is_valid_utf8("\xE4\x00\xAD"), false)         -- bad 3-byte cont2
    T.eq(compat.is_valid_utf8("\xE4\xB8\x00"), false)         -- bad 3-byte cont3
    T.eq(compat.is_valid_utf8("\xF0\x80\x80\x80"), false)     -- overlong 4-byte (F0 lead)
    T.eq(compat.is_valid_utf8("\xF4\x90\x80\x80"), false)     -- >U+10FFFF (F4 lead)
    T.eq(compat.is_valid_utf8("\xF2\x00\x80\x80"), false)     -- bad 4-byte cont2
    T.eq(compat.is_valid_utf8("\xF2\x80\x00\x80"), false)     -- bad 4-byte cont3
    T.eq(compat.is_valid_utf8("\xF2\x80\x80\x00"), false)     -- bad 4-byte cont4
    T.eq(compat.is_valid_utf8("\xF0\x9F\x98"), false)         -- truncated 4-byte
    T.eq(compat.is_valid_utf8("\xF5\x80\x80\x80"), false)     -- out-of-range lead
    T.eq(compat.is_valid_utf8("\xFF"), false)                 -- 0xFF invalid lead
    -- Non-string rejection.
    T.eq(compat.is_valid_utf8(42), false)
    T.eq(compat.is_valid_utf8({}), false)
end)

s:test("compat.M.sanitize_utf8: pure-Lua fallback matches fan.sanitize_utf8", function()
    local compat = require("fan.compat")
    T.is_type(compat.sanitize_utf8, "function")
    -- Fast path (all-valid) returns the input unchanged.
    T.eq(compat.sanitize_utf8(nil), "")
    T.eq(compat.sanitize_utf8(""), "")
    T.eq(compat.sanitize_utf8("hello"), "hello")
    T.eq(compat.sanitize_utf8("caf\xC3\xA9"), "caf\xC3\xA9")
    T.eq(compat.sanitize_utf8("\xF0\x9F\x98\x80"), "\xF0\x9F\x98\x80")
    -- Repair path exercises the FFFD-and-advance branch.
    T.eq(compat.sanitize_utf8("\x80"), "\xEF\xBF\xBD")
    T.eq(compat.sanitize_utf8("ok\x80done"), "ok\xEF\xBF\xBDdone")
    T.eq(compat.sanitize_utf8("\x80\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD")
    -- Repair path with a valid prefix + valid tail (exercises out[]+=s:sub branch).
    T.eq(compat.sanitize_utf8("caf\xC3\xA9\x80\xE4\xB8\xAD"),
                              "caf\xC3\xA9\xEF\xBF\xBD\xE4\xB8\xAD")
    -- Truncated multi-byte: v1's `q += 1` fallback emits one FFFD per bad byte,
    -- so the two-byte truncated tail contributes two FFFDs.
    T.eq(compat.sanitize_utf8("a\xE4\xB8"), "a\xEF\xBF\xBD\xEF\xBF\xBD")
    -- Result is always valid UTF-8.
    T.eq(compat.is_valid_utf8(compat.sanitize_utf8("\x80mix\xFF\xE4\xB8\xAD")), true)
    -- Non-string rejection.
    T.is_nil(compat.sanitize_utf8(42))
    T.is_nil(compat.sanitize_utf8({}))
end)

s:test("compat.apply(): idempotent across repeated calls", function()
    local compat = require("fan.compat")
    local fan = require("fan")
    -- Multiple apply() invocations should all short-circuit through the
    -- `applied` guard and return the same fan table.
    T.eq(compat.apply(), fan)
    T.eq(compat.apply(), fan)
end)

os.exit(T.run(s))
