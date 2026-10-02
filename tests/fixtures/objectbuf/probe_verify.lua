-- objectbuf interop probe: verify
--
-- Loads a fixture produced by probe_emit.lua (possibly from a different
-- LuaFan major version) and checks, for every record:
--   * decode of the plain wire bytes succeeds;
--   * (non-opaque) decode/re-encode/decode stays structurally equal;
--   * (stable)    re-encode reproduces the exact original bytes;
--   * decode of the symbol-compressed bytes, using the carried symbol table,
--     succeeds, and the same stability rules hold for the symbol form.
--
-- Usage: <fan|lua> probe_verify.lua <fixture.lua>

local path = ...
if not path or path == "" then
  path = arg and arg[1]
end
assert(path, "usage: probe_verify.lua <fixture.lua>")

local fan = require("fan")
local ob = fan.objectbuf or require("fan.objectbuf")

local function unhex(s)
  return (s:gsub("%x%x", function(h) return string.char(tonumber(h, 16)) end))
end

local function dec(s, sym)
  local v = ob.decode(s, sym)
  assert(v ~= nil, "decode returned nil")
  return v
end

local function deep_equal(a, b, seen)
  if type(a) ~= type(b) then return false end
  if type(a) ~= "table" then return a == b end
  seen = seen or {}
  if seen[a] and seen[a][b] then return true end
  seen[a] = seen[a] or {}
  seen[a][b] = true
  for k, v in pairs(a) do
    if not deep_equal(v, b[k], seen) then return false end
  end
  for k in pairs(b) do
    if a[k] == nil then return false end
  end
  return true
end

local records = assert(loadfile(path))()
local checks = 0

for _, rec in ipairs(records) do
  local plain = unhex(rec.plain)
  local obj = dec(plain)

  if rec.opaque == 0 then
    assert(deep_equal(obj, dec(ob.encode(obj))), rec.name .. ": plain re-encode mismatch")
  end
  if rec.stable == 1 then
    assert(ob.encode(obj) == plain, rec.name .. ": plain bytes not reproduced")
  end
  checks = checks + 1

  if rec.sym ~= "" then
    local sym = dec(unhex(rec.symtab))
    local sym_bytes = unhex(rec.sym)
    local obj2 = dec(sym_bytes, sym)
    if rec.opaque == 0 then
      assert(deep_equal(obj, obj2), rec.name .. ": symbol decode mismatch")
    end
    local reenc = ob.encode(obj2, sym)
    if rec.stable == 1 then
      assert(reenc == sym_bytes, rec.name .. ": symbol bytes not reproduced")
    end
    if rec.opaque == 0 then
      assert(deep_equal(obj, dec(reenc, sym)), rec.name .. ": symbol re-encode mismatch")
    end
    checks = checks + 1
  end
end

print("INTEROP_OK checks=" .. checks .. " records=" .. #records)
