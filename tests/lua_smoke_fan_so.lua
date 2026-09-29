-- lua_smoke_fan_so.lua — M15 cross-interpreter smoke for build/fan.so.
--
-- Run this under a STOCK Lua interpreter (e.g. /usr/bin/lua5.3), NOT our
-- own thin `fan` executable, to verify the second half of the dual-form
-- delivery promise: build/fan.so must be dlopen()able by any host Lua via
-- `require("fan")`, exactly like v1's fan.so was.
--
-- The test itself is deliberately minimal: pull `fan` off the C module,
-- then pull one pure-Lua submodule (`fan.http`) so we also exercise the
-- LUA_PATH side.  No network or event-loop I/O — the smoke check is only
-- "does the module dlopen + luaopen cleanly and expose the public API".
-- Anything deeper is already covered by the standard test_*.lua suites
-- running under ./fan.
--
-- Environment expected by the runner:
--   LUA_CPATH  = ".../build/?.so;;"
--   LUA_PATH   = ".../lua/?.lua;.../lua/?/init.lua;;"
--
-- Exit non-zero on any failure so run_tests.sh can propagate it.

local fan  = require("fan")
local http = require("fan.http")

assert(type(fan)       == "table",    "require(fan) did not return a table")
assert(type(fan.loop)  == "function", "fan.loop missing (C module)")
assert(type(fan.tcp)   == "table",    "fan.tcp missing (C module)")
assert(type(http)      == "table",    "require(fan.http) did not return a table")
assert(type(http.get)  == "function", "fan.http.get missing (Lua module)")

print("stock lua5.3 require(fan) OK")
