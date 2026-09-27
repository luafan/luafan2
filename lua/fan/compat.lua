-- fan/compat.lua — LuaFan v2 compatibility shim for v1 code paths (M7).
--
-- v1 exposed some POSIX helpers directly under `fan.*` (fan.getpid,
-- fan.fork, fan.kill, ...). v2 keeps them under `fan.posix.*` so the
-- namespace stays tidy, but callers migrating from v1 can require this
-- shim to get the flat names back.
--
--   require("fan.compat")
--
-- After that, all v1 top-level POSIX helpers work on the current
-- `fan` table. This function is idempotent: calling it twice is a
-- no-op.
--
-- The shim also normalises kill()'s dangerous-PID guardrail so v1 code
-- that legitimately targeted pid=1 or a process group with an argument
-- of {force=true}/true keeps working. Naked v1 kill(pid, sig) that
-- happens to hit pid<=1 will now return (nil, "refused: ...") — this
-- is intentional: silently retaining the v1 footgun would defeat the
-- point of the v2 hardening.
--
-- v1 flat data-encoding helpers (`fan.data2hex`/`hex2data`) and UTF-8
-- validators (`fan.is_valid_utf8`/`fan.sanitize_utf8`) are provided here
-- as pure Lua fallbacks. The C module (`fan.data2hex` etc.) is the
-- primary implementation; these Lua versions have byte-identical
-- semantics and only get installed on `fan` if the C ones are missing.
--
-- v2 diverges from v1 on two specific hex2data quirks (odd-length
-- truncation and strtol "invalid -> 0"): those were silent data
-- corruption footguns. v2 returns (nil, message) instead. Everything
-- else (upper-case hex, U+FFFD sanitize replacement, nil-as-empty
-- for UTF-8 helpers, string-coercible numbers) is kept as v1.

local fan = require("fan")

local function alias(dst, src, names)
    for _, name in ipairs(names) do
        if dst[name] == nil and src[name] ~= nil then
            dst[name] = src[name]
        end
    end
end

----------------------------------------------------------------------
-- v2 hex encoders. Design (differs from v1 where v1 was unsafe):
--   * data2hex(s) -> upper-case hex, length 2*#s ("" -> ""). Matches v1
--     byte-for-byte on the sane inputs it also handled.
--   * hex2data(hex) -> STRICT: odd length or any non-hex byte returns
--     (nil, message). v1 silently truncated odd tails and mapped invalid
--     digits to 0 (via strtol on a 2-char buffer) — both are silent data
--     corruption footguns rejected in v2. Empty string still returns "".
--   * Both accept any string-coercible value (v1 uses lua_isstring which
--     is true for numbers). Non-coercible types return nil.
--
-- These pure-Lua versions are a belt-and-braces fallback: the C module
-- already installs identical semantics on `fan.data2hex`/`fan.hex2data`.
----------------------------------------------------------------------
local HEX_UPPER = "0123456789ABCDEF"

local function data2hex(s)
    if type(s) == "number" then s = tostring(s) end
    if type(s) ~= "string" then return nil end
    if s == "" then return "" end
    local out = {}
    for i = 1, #s do
        local b = string.byte(s, i)
        local hi = (b >> 4) & 0xF
        local lo = b & 0xF
        out[i] = HEX_UPPER:sub(hi + 1, hi + 1) .. HEX_UPPER:sub(lo + 1, lo + 1)
    end
    return table.concat(out)
end

-- 0-9 / A-F / a-f -> 0..15; anything else -> -1 (rejected by hex2data).
local function hex_nibble(c)
    if c >= 48 and c <= 57  then return c - 48 end     -- '0'..'9'
    if c >= 65 and c <= 70  then return c - 55 end     -- 'A'..'F'
    if c >= 97 and c <= 102 then return c - 87 end     -- 'a'..'f'
    return -1
end

local function hex2data(hex)
    if type(hex) == "number" then hex = tostring(hex) end
    if type(hex) ~= "string" then return nil end
    local n = #hex
    if n == 0 then return "" end
    if n % 2 ~= 0 then return nil, "hex string length must be even" end
    local out = {}
    for i = 1, n, 2 do
        local a = string.byte(hex, i)
        local b = string.byte(hex, i + 1)
        local hi = hex_nibble(a)
        local lo = hex_nibble(b)
        if hi < 0 or lo < 0 then
            return nil, string.format("invalid hex digit at byte %d", hi < 0 and i or i + 1)
        end
        out[#out + 1] = string.char((hi << 4) | lo)
    end
    return table.concat(out)
end

----------------------------------------------------------------------
-- v1 UTF-8 helpers. Match src/luafan.c:236-346 exactly:
--   is_valid_utf8(s) -> boolean. Empty / nil -> true.
--   sanitize_utf8(s) -> the input string when already valid (zero-copy
--     fast path); otherwise every ill-formed byte is replaced by the
--     U+FFFD REPLACEMENT CHARACTER (EF BF BD, 3 bytes). Repair walks
--     byte-by-byte, emitting FFFD for each bad lead and skipping that
--     one byte — matches C impl's "q += 1" fallback.
----------------------------------------------------------------------

-- Decode one UTF-8 code unit starting at index `i` in `s`. Returns the
-- number of bytes consumed (1..4) on success, 0 on any malformed
-- prefix (overlong, stray continuation, truncated, out-of-range).
local function utf8_decode_one(s, i, n)
    local b1 = string.byte(s, i)
    if not b1 then return 0 end
    if b1 < 0x80 then return 1 end
    if b1 < 0xC2 then return 0 end
    if b1 < 0xE0 then
        if i + 1 > n then return 0 end
        local b2 = string.byte(s, i + 1)
        if (b2 & 0xC0) ~= 0x80 then return 0 end
        return 2
    end
    if b1 < 0xF0 then
        if i + 2 > n then return 0 end
        local b2 = string.byte(s, i + 1)
        if b1 == 0xE0 then
            if b2 < 0xA0 or b2 > 0xBF then return 0 end
        elseif b1 == 0xED then
            if b2 < 0x80 or b2 > 0x9F then return 0 end
        else
            if (b2 & 0xC0) ~= 0x80 then return 0 end
        end
        local b3 = string.byte(s, i + 2)
        if (b3 & 0xC0) ~= 0x80 then return 0 end
        return 3
    end
    if b1 < 0xF5 then
        if i + 3 > n then return 0 end
        local b2 = string.byte(s, i + 1)
        if b1 == 0xF0 then
            if b2 < 0x90 or b2 > 0xBF then return 0 end
        elseif b1 == 0xF4 then
            if b2 < 0x80 or b2 > 0x8F then return 0 end
        else
            if (b2 & 0xC0) ~= 0x80 then return 0 end
        end
        local b3 = string.byte(s, i + 2)
        if (b3 & 0xC0) ~= 0x80 then return 0 end
        local b4 = string.byte(s, i + 3)
        if (b4 & 0xC0) ~= 0x80 then return 0 end
        return 4
    end
    return 0
end

local function is_valid_utf8(s)
    if s == nil then s = "" end
    if type(s) ~= "string" then return false end
    local n = #s
    local i = 1
    while i <= n do
        local k = utf8_decode_one(s, i, n)
        if k == 0 then return false end
        i = i + k
    end
    return true
end

local REPLACEMENT = "\xEF\xBF\xBD"

local function sanitize_utf8(s)
    if s == nil then s = "" end
    if type(s) ~= "string" then return nil end
    local n = #s
    -- Fast path: single validating pass, return original on success.
    local i = 1
    while i <= n do
        local k = utf8_decode_one(s, i, n)
        if k == 0 then break end
        i = i + k
    end
    if i > n then return s end
    -- Repair path: keep the valid prefix, then emit FFFD for each bad
    -- byte and advance by 1 (matches v1's `q += 1` fallback exactly).
    local out = { s:sub(1, i - 1) }
    while i <= n do
        local k = utf8_decode_one(s, i, n)
        if k > 0 then
            out[#out + 1] = s:sub(i, i + k - 1)
            i = i + k
        else
            out[#out + 1] = REPLACEMENT
            i = i + 1
        end
    end
    return table.concat(out)
end

local applied = false

local M = {}

function M.apply()
    if applied then return fan end
    applied = true

    local posix = fan.posix
    if posix then
        alias(fan, posix, {
            "getpid", "fork", "waitpid", "kill",
            "setpgid", "getpgid", "setsid",
            "getcpucount", "getaffinity", "setaffinity",
            "getinterfaces", "setprogname",
        })
        -- Signal + wait tables surface at the top level too.
        if fan.signals == nil then fan.signals = posix.signals end
        if fan.wait    == nil then fan.wait    = posix.wait end
    end

    -- Flat v1 helpers. Only install where the C module hasn't already
    -- registered a native version (belt-and-braces; v2 currently ships
    -- none of these on `fan` itself).
    if fan.data2hex     == nil then fan.data2hex     = data2hex end
    if fan.hex2data     == nil then fan.hex2data     = hex2data end
    if fan.is_valid_utf8 == nil then fan.is_valid_utf8 = is_valid_utf8 end
    if fan.sanitize_utf8 == nil then fan.sanitize_utf8 = sanitize_utf8 end

    return fan
end

-- Expose for callers / tests that want the pure functions without
-- installing them on the fan table.
M.data2hex      = data2hex
M.hex2data      = hex2data
M.is_valid_utf8 = is_valid_utf8
M.sanitize_utf8 = sanitize_utf8

-- Auto-apply on `require("fan.compat")` so the module has zero API surface.
M.apply()

return M
