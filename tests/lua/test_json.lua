--[[
  test_json.lua — M5.1 JSON contract tests (fan.json, pure Lua).

  Coverage (plan \u00a74.8):
    - primitive round-trips (null / bool / string / number / integer)
    - empty containers: array vs object disambiguated by markers
    - nesting (arrays of objects, objects of arrays)
    - UTF-8 encode/decode (multi-byte, surrogate-pair \\uXXXX\\uXXXX)
    - escapes: control chars, ", \\, /, \\b \\f \\n \\r \\t, \\uXXXX
    - large integers preserved (up to Lua's precision)
    - float round-trips within IEEE 754 double precision
    - decode surfaces line/col on error, refuses trailing garbage
    - reject NaN/Inf on encode
    - json.null sentinel survives encode+decode
]]
local T = require("test_framework")
local json = require("fan.json")

local s = T.suite("fan.json (M5)")

s:test("primitive round-trips", function()
  T.eq(json.encode(nil),        "null")
  T.eq(json.encode(json.null),  "null")
  T.eq(json.encode(true),       "true")
  T.eq(json.encode(false),      "false")
  T.eq(json.encode(0),          "0")
  T.eq(json.encode(-1),         "-1")
  T.eq(json.encode(1),          "1")
  T.eq(json.encode("hi"),       '"hi"')
  T.eq(json.decode("null"),     json.null)
  T.eq(json.decode("true"),     true)
  T.eq(json.decode("false"),    false)
  T.eq(json.decode('"hi"'),     "hi")
  T.eq(json.decode("42"),       42)
  T.eq(json.decode("-3"),       -3)
end)

s:test("empty containers are unambiguously encoded", function()
  T.eq(json.encode(json.array()),  "[]")
  T.eq(json.encode(json.object()), "{}")
  -- decode disambiguates by literal
  local a = json.decode("[]"); T.truthy(json.is_array(a));  T.eq(#a, 0)
  local o = json.decode("{}"); T.truthy(json.is_object(o))
end)

s:test("array of primitives round-trips", function()
  local a = json.array{1, 2, 3, "x", true, json.null}
  local enc = json.encode(a)
  T.eq(enc, '[1,2,3,"x",true,null]')
  local dec = json.decode(enc)
  T.eq(#dec, 6); T.eq(dec[1], 1); T.eq(dec[4], "x")
  T.eq(dec[5], true); T.eq(dec[6], json.null)
end)

s:test("object with sorted keys round-trips deterministically", function()
  local o = json.object{ b = 2, a = 1, c = 3 }
  T.eq(json.encode(o), '{"a":1,"b":2,"c":3}')
end)

s:test("bare table looks-like-array is emitted as JSON array", function()
  T.eq(json.encode({10, 20, 30}), "[10,20,30]")
  -- but bare empty table defaults to object (JavaScript intuition)
  T.eq(json.encode({}), "{}")
end)

s:test("deep nesting: arrays of objects, objects of arrays", function()
  local v = {
    items = json.array{
      json.object{ id = 1, tags = json.array{"a", "b"} },
      json.object{ id = 2, tags = json.array() },
    },
    meta = json.object{ count = 2 },
  }
  local enc = json.encode(v)
  local dec = json.decode(enc)
  T.eq(#dec.items, 2)
  T.eq(dec.items[1].id, 1)
  T.eq(dec.items[1].tags[2], "b")
  T.eq(#dec.items[2].tags, 0)
  T.eq(dec.meta.count, 2)
end)

s:test("string escapes: control chars, quote, backslash, forward slash", function()
  local raw = 'a"b\\c\nd\te'
  local enc = json.encode(raw)
  T.eq(enc, '"a\\"b\\\\c\\nd\\te"')
  T.eq(json.decode(enc), raw)
  -- forward slash may be either / or \/ ; we choose /, but decode both
  T.eq(json.decode('"a/b"'),    "a/b")
  T.eq(json.decode('"a\\/b"'),  "a/b")
  -- control chars < 0x20 encode as \\uXXXX
  local low = string.char(0x01, 0x1f)
  T.eq(json.encode(low), '"\\u0001\\u001f"')
  T.eq(json.decode('"\\u0001\\u001f"'), low)
end)

s:test("UTF-8 round-trip (multi-byte) survives without \\u escaping", function()
  -- LuaFan v2 emits UTF-8 verbatim (JSON permits both raw UTF-8 and \\u)
  local raw = "\xe4\xb8\xad\xe6\x96\x87 caf\xc3\xa9"  -- 中文 café
  local enc = json.encode(raw)
  -- must survive decode
  T.eq(json.decode(enc), raw)
end)

s:test("UTF-8 decode from \\uXXXX including surrogate pairs", function()
  -- U+4E2D = 中, U+6587 = 文, U+1F600 = 😀 (surrogate pair D83D DE00)
  T.eq(json.decode('"\\u4e2d\\u6587"'), "\xe4\xb8\xad\xe6\x96\x87")
  T.eq(json.decode('"\\uD83D\\uDE00"'), "\xf0\x9f\x98\x80")
  -- unpaired surrogate must fail cleanly
  local ok, err = json.decode('"\\uD83D"')
  T.is_nil(ok); T.not_nil(err)
end)

s:test("integers preserved (within Lua's number precision)", function()
  T.eq(json.decode("12345678901234"), 12345678901234)
  -- Lua 5.3 integer subtype survives round-trip
  local enc = json.encode(12345678901234)
  T.eq(enc, "12345678901234")
end)

s:test("floats round-trip within IEEE 754 doubles", function()
  local samples = { 3.14, -0.5, 1e-10, 1e15, 1.5, 0.1 + 0.2 }
  for _, n in ipairs(samples) do
    local dec = json.decode(json.encode(n))
    -- allow tiny rounding; assert same bit pattern via string round-trip
    T.eq(json.encode(dec), json.encode(n))
  end
end)

s:test("reject NaN and Infinity on encode", function()
  local nan = 0 / 0
  local inf = math.huge
  T.error_raised(function() json.encode(nan) end)
  T.error_raised(function() json.encode(inf) end)
  T.error_raised(function() json.encode(-inf) end)
end)

s:test("decode surfaces line/col info in error messages", function()
  local ok, err = json.decode('{"a": 1, "b": }')
  T.is_nil(ok); T.not_nil(err); T.truthy(err:find("line 1"))
end)

s:test("decode refuses trailing garbage", function()
  local ok, err = json.decode('{"a":1} garbage')
  T.is_nil(ok); T.not_nil(err); T.truthy(err:find("trailing"))
end)

s:test("tostring(json.null) returns 'null', not 'table: 0x...'", function()
  -- The __tostring metamethod on the null sentinel makes it play nice with
  -- print(), string.format("%s", ...), string concat, log libraries, etc.
  -- Without this, `log.info("field a =", body.a)` would emit noisy
  -- "table: 0x7f..." for a JSON null field.  Downstream code depends on
  -- this — do not remove the __tostring binding.
  T.eq(tostring(json.null), "null")
  T.eq(tostring(json.decode("null")), "null")            -- same sentinel
  T.eq(string.format("%s", json.null), "null")
  T.eq("[" .. tostring(json.null) .. "]", "[null]")
  -- Sanity: it really is a table under the hood (not userdata)
  T.eq(type(json.null), "table")
  -- And decode always yields the exact same table instance
  T.eq(json.null == json.decode("null"), true)
end)

s:test("json.null sentinel survives object round-trip", function()
  local o = json.object{ x = json.null, y = 1 }
  local enc = json.encode(o)
  T.eq(enc, '{"x":null,"y":1}')
  local dec = json.decode(enc)
  T.eq(dec.x, json.null)
  T.eq(dec.y, 1)
end)

s:test("indent option emits pretty-printed output that still round-trips", function()
  local v = json.object{ a = json.array{1, 2}, b = "x" }
  local enc = json.encode(v, { indent = "  " })
  T.truthy(enc:find("\n"))
  T.eq(json.encode(json.decode(enc)), '{"a":[1,2],"b":"x"}')
end)

s:test("object key type check: non-string keys are rejected", function()
  local o = json.object()
  o[42] = "wrong"
  T.error_raised(function() json.encode(o) end)
end)

s:test("json.is_nonempty_string classifies strings", function()
  -- true only for a Lua string with length > 0
  T.eq(json.is_nonempty_string("hi"),    true)
  T.eq(json.is_nonempty_string("a"),     true)
  T.eq(json.is_nonempty_string(" "),     true)  -- whitespace still non-empty
  T.eq(json.is_nonempty_string(""),      false)
  T.eq(json.is_nonempty_string(nil),     false)
  T.eq(json.is_nonempty_string(42),      false)
  T.eq(json.is_nonempty_string(true),    false)
  T.eq(json.is_nonempty_string({"x"}),   false)
  T.eq(json.is_nonempty_string(json.null), false)
  -- Zero args behaves like nil (LUA_TNONE at index 1)
  T.eq(json.is_nonempty_string(),        false)
end)

s:test("json.is_present distinguishes nil / null-sentinel / real values", function()
  -- Real values are present
  T.eq(json.is_present("hi"),   true)
  T.eq(json.is_present(""),     true)   -- empty string IS present (has type)
  T.eq(json.is_present(0),      true)   -- zero IS present
  T.eq(json.is_present(false),  true)   -- false IS present
  T.eq(json.is_present({}),     true)   -- table IS present
  T.eq(json.is_present({1,2}),  true)
  -- nil and json.null are NOT present
  T.eq(json.is_present(nil),       false)
  T.eq(json.is_present(json.null), false)
  T.eq(json.is_present(),          false)  -- no argument
end)

s:test("json.is_present + decode: null-sentinel survives, is_present picks it up", function()
  local body = json.decode('{"a": null, "b": 42, "c": ""}')
  -- b.a is the null sentinel, NOT Lua nil, so `body.a == nil` is FALSE:
  T.eq(body.a == nil,               false)
  T.eq(body.a,                      json.null)
  -- The idiomatic "did the caller pass a real value here?" check:
  T.eq(json.is_present(body.a),     false)   -- explicit null
  T.eq(json.is_present(body.b),     true)    -- 42
  T.eq(json.is_present(body.c),     true)    -- "" is present
  T.eq(json.is_present(body.missing), false) -- absent key -> nil -> not present
  -- Contrast with is_nonempty_string on the same body:
  T.eq(json.is_nonempty_string(body.a), false)  -- null sentinel is a table, not string
  T.eq(json.is_nonempty_string(body.c), false)  -- "" is empty
  T.eq(json.is_nonempty_string(json.encode(body.b)), true)  -- "42" is non-empty
end)

os.exit(T.run(s))
