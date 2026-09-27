--[[
  test_framework.lua — LuaFan v2 minimal Lua test framework.
  Usage:
    local T = require("test_framework")
    local s = T.suite("My Suite")
    s:test("name", function() T.eq(1+1, 2) end)
    os.exit(T.run(s))
]]

local T = {}
T.__index = T

local function fmt(v)
  local t = type(v)
  if t == "string" then return string.format("%q", v) end
  return tostring(v)
end

-- assertions (raise on failure so the test body aborts)
function T.eq(a, b, msg)
  if a ~= b then error(msg or ("expected " .. fmt(b) .. ", got " .. fmt(a)), 2) end
end
function T.ne(a, b, msg)
  if a == b then error(msg or ("expected not " .. fmt(b)), 2) end
end
function T.truthy(v, msg)
  if not v then error(msg or ("expected truthy, got " .. fmt(v)), 2) end
end
function T.falsy(v, msg)
  if v then error(msg or ("expected falsy, got " .. fmt(v)), 2) end
end
function T.is_nil(v, msg)
  if v ~= nil then error(msg or ("expected nil, got " .. fmt(v)), 2) end
end
function T.not_nil(v, msg)
  if v == nil then error(msg or "expected non-nil", 2) end
end
function T.is_type(v, ty, msg)
  if type(v) ~= ty then error(msg or ("expected type " .. ty .. ", got " .. type(v)), 2) end
end
function T.error_raised(fn, msg)
  local ok = pcall(fn)
  if ok then error(msg or "expected function to raise", 2) end
end

local Suite = {}
Suite.__index = Suite

function T.suite(name)
  return setmetatable({ name = name, tests = {} }, Suite)
end

function Suite:test(name, fn)
  self.tests[#self.tests + 1] = { name = name, fn = fn }
  return self
end

-- run one or more suites; returns process exit code (0 pass, 1 fail)
function T.run(...)
  local suites = { ... }
  local total, passed, failed = 0, 0, 0
  for _, s in ipairs(suites) do
    print("Suite: " .. s.name)
    for _, tc in ipairs(s.tests) do
      total = total + 1
      local ok, err = pcall(tc.fn)
      if ok then
        passed = passed + 1
        print("  [ OK ] " .. tc.name)
      else
        failed = failed + 1
        print("  [FAIL] " .. tc.name .. "  -> " .. tostring(err))
      end
    end
  end
  print("")
  print(string.format("TOTAL: %d  PASSED: %d  FAILED: %d", total, passed, failed))
  print("RESULT: " .. (failed == 0 and "PASSED" or "FAILED"))
  return failed == 0 and 0 or 1
end

return T
