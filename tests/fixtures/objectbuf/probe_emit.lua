-- objectbuf interop probe: emit
--
-- Builds a deterministic corpus, and for each value prints:
--   plain  : hex(objectbuf.encode(value))
--   sym    : hex(objectbuf.encode(value, objectbuf.symbol(value)))
--   symtab : hex(objectbuf.encode(objectbuf.symbol(value)))
--   stable : 1 when re-encoding the decoded value must reproduce the exact
--            same bytes (true for scalars, arrays and single-key tables,
--            whose section order does not depend on Lua's string-hash seed)
--   opaque : 1 when the value contains a table used as a key (structural
--            comparison is not meaningful; byte-exact re-encode is checked)
--
-- Output is a Lua table literal, consumed by probe_verify.lua and by
-- tests/lua/test_objectbuf_v1_interop.lua. The same script runs under LuaFan
-- v1 (docker image luafan/luafan-ubuntu) and under luafan2, which makes the
-- resulting vectors a genuine cross-version regression fixture.

local fan = require("fan")
local ob = fan.objectbuf or require("fan.objectbuf")

local function hex(s)
  return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

local corpus = {}
local function add(name, value, stable, opaque)
  corpus[#corpus + 1] = {
    name = name, value = value, stable = stable and 1 or 0, opaque = opaque and 1 or 0,
  }
end

-- boolean ------------------------------------------------------------------
add("scalar_false", false, true)
add("scalar_true", true, true)

-- u30 integer boundaries ---------------------------------------------------
add("int_0", 0, true)
add("int_1", 1, true)
add("int_127", 127, true)
add("int_128", 128, true)
add("int_255", 255, true)
add("int_256", 256, true)
add("int_16383", 16383, true)
add("int_16384", 16384, true)
add("int_2097151", 2097151, true)
add("int_2097152", 2097152, true)
add("int_268435455", 268435455, true)
add("int_268435456", 268435456, true)
add("int_u32max", 4294967295, true)

-- negative / out-of-u30 integers (stored as D64) ---------------------------
add("int_neg1", -1, true)
add("int_neg128", -128, true)
add("int_neg129", -129, true)
add("int_neg32768", -32768, true)
add("int_neg32769", -32769, true)
add("int_i32min", -2147483648, true)
add("int_i32min_m1", -2147483649, true)
add("int_2pow32", 4294967296, true)
add("int_2pow40", 1099511627776, true)
add("int_2pow53", 9007199254740992, true)

-- doubles ------------------------------------------------------------------
add("float_pi", 3.14, true)
add("float_neg", -0.5, true)
add("float_tiny", 1e-10, true)
add("float_big", 1e15, true)
add("float_round", 0.1 + 0.2, true)
add("float_negzero", -0.0, true)

-- strings ------------------------------------------------------------------
add("str_empty", "", true)
add("str_short", "x", true)
add("str_digit", "1", true)
add("str_mid", "hello world", true)
add("str_utf8", "h\195\169llo\226\134\146", true)   -- "héllo→"
add("str_nul", "a\0b", true)
add("str_250", string.rep("a", 250), true)
add("str_2000", string.rep("b", 2000), true)

-- arrays (order-independent) ----------------------------------------------
add("arr_empty", {}, true)
add("arr_one", { 1 }, true)
add("arr_ints", { 1, 2, 3 }, true)
add("arr_strs", { "a", "b", "c" }, true)
add("arr_mixed", { 1, "x", true, false }, true)
add("arr_hole", { 1, [3] = 3 }, true)
add("arr_nested", { { 1 }, { 2 } }, true)
add("arr_floats", { 1.5, 2.5, -3.25 }, true)
add("arr_bytes", { "\0", "\1\2", "\255" }, true)

local shared_sub = { t = 1 }
add("arr_shared", { shared_sub, shared_sub }, true)

-- single-key tables (order-independent)
add("map_one", { a = 1 }, true)
add("map_one_str", { k = "v" }, true)

-- multi-key / nested / cyclic (hash order not guaranteed) ------------------
add("map_multi", { a = 1, b = 2, c = 3 }, false)
add("map_bools", { x = true, y = false }, false)
add("map_nested", { outer = { inner = { leaf = 42, items = { 1, 2, 3 } } } }, false)

local cyc = {}
cyc.self = cyc
cyc.name = "cycle"
add("tbl_cycle", cyc, false)

local deep = { leaf = 1 }
for _ = 1, 12 do deep = { n = deep } end
add("tbl_deep", deep, true)

add("tbl_numbers", {
  1234556789, 12345.6789, -1234556789, -12345.6789, 0, "asdfa",
  nested = { e = "nested value" },
}, false)

-- table used as a key (opaque structural identity) -------------------------
add("map_tablekey", { [{ 1, 2 }] = "x" }, true, true)

-- symbol pressure: few distinct strings/ints repeated many times -----------
local users = {}
for i = 1, 200 do
  users[i] = { status = "active", role = "member", region = "eu", id = i }
end
add("symbol_pressure", { users = users }, false)

local reps = {}
for i = 1, 500 do reps[i] = "dup" end
add("symbol_dup_strings", reps, true)

io.write("return {\n")
for _, e in ipairs(corpus) do
  local plain = hex(ob.encode(e.value))
  local symhex, symtabhex = "", ""
  local ok, sym = pcall(ob.symbol, e.value)
  if ok and type(sym) == "table" then
    symhex = hex(ob.encode(e.value, sym))
    symtabhex = hex(ob.encode(sym))
  end
  io.write(string.format(
    "  {name=%q, stable=%d, opaque=%d, plain=%q, sym=%q, symtab=%q},\n",
    e.name, e.stable, e.opaque, plain, symhex, symtabhex))
end
io.write("}\n")
