--[[
  test_log.lua — contract tests for fan.log (M8).

  Coverage goals:
    - default level (INFO) gates trace/debug off, info/warn/error on;
    - set_level accepts int or name; unknown name raises;
    - set_category / is<Level>(category) truth table;
    - set_adapter overrides the print sink and receives the tagged args;
    - emitters produce "[LEVEL]" prefix + varargs exactly (v1 shape);
    - LUAFAN_LOG_LEVEL: covered by set_level/get_level round-trip
      (we can't unset+re-require the module mid-process reliably; the
      env-var branch is exercised in a subprocess test).
]]
local T = require("test_framework")

-- Fresh copy per test by clearing the cached module. Each test re-requires
-- so state (config, adapter) starts at defaults.
local function fresh()
    package.loaded["fan.log"] = nil
    return require("fan.log")
end

local s = T.suite("fan.log (M8)")

-- ---------------------------------------------------------------------------
s:test("default level is INFO: info/warn/error on, debug/trace off", function()
    local log = fresh()
    T.truthy(log.isError())
    T.truthy(log.isWarn())
    T.truthy(log.isInfo())
    T.falsy(log.isDebug())
    T.falsy(log.isTrace())
end)

-- ---------------------------------------------------------------------------
s:test("set_level accepts integer", function()
    local log = fresh()
    log.set_level(log.LEVELS.TRACE)
    T.truthy(log.isTrace())
    T.eq(log.get_level(), 5)
end)

s:test("set_level accepts case-insensitive name", function()
    local log = fresh()
    log.set_level("debug")
    T.eq(log.get_level(), 4)
    log.set_level("WARN")
    T.eq(log.get_level(), 2)
end)

s:test("set_level rejects unknown name", function()
    local log = fresh()
    T.error_raised(function() log.set_level("BOGUS") end)
end)

-- ---------------------------------------------------------------------------
s:test("set_level(0) silences every level", function()
    local log = fresh()
    log.set_level(0)
    T.falsy(log.isError())
    T.falsy(log.isWarn())
    T.falsy(log.isInfo())
    T.falsy(log.isDebug())
    T.falsy(log.isTrace())
end)

-- ---------------------------------------------------------------------------
s:test("category gates isDebug when level allows it", function()
    local log = fresh()
    log.set_level("DEBUG")
    T.truthy(log.isDebug())                -- no category -> pass
    T.falsy(log.isDebug("sql"))            -- category unset -> false
    log.set_category("sql", true)
    T.truthy(log.isDebug("sql"))
    log.set_category("sql", false)
    T.falsy(log.isDebug("sql"))
end)

-- ---------------------------------------------------------------------------
s:test("set_adapter captures the tagged output", function()
    local log = fresh()
    local seen = {}
    log.set_adapter(function(...) seen[#seen + 1] = { ... } end)

    log.info("hello", 42, true)
    T.eq(#seen, 1)
    T.eq(seen[1][1], "[INFO]")
    T.eq(seen[1][2], "hello")
    T.eq(seen[1][3], 42)
    T.eq(seen[1][4], true)
end)

s:test("set_adapter rejects non-function", function()
    local log = fresh()
    T.error_raised(function() log.set_adapter("not a fn") end)
end)

-- ---------------------------------------------------------------------------
s:test("emitters below current level do not call the adapter", function()
    local log = fresh()
    local seen = 0
    log.set_adapter(function() seen = seen + 1 end)

    log.set_level("WARN")
    log.debug("hidden")
    log.info("hidden too")
    log.warn("shown")
    log.error("shown")
    T.eq(seen, 2)
end)

s:test("all five emitters produce their tag when enabled", function()
    local log = fresh()
    log.set_level("TRACE")
    local tags = {}
    log.set_adapter(function(tag) tags[#tags + 1] = tag end)

    log.trace("t"); log.debug("d"); log.info("i"); log.warn("w"); log.error("e")
    T.eq(table.concat(tags, ","), "[TRACE],[DEBUG],[INFO],[WARN],[ERROR]")
end)

-- ---------------------------------------------------------------------------
-- parse_level: cover every branch (name, integer, nil, unknown, empty).
s:test("_parse_level: name -> level number", function()
    local log = fresh()
    T.eq(log._parse_level("DEBUG"), 4)
    T.eq(log._parse_level("debug"), 4)
    T.eq(log._parse_level("ERROR"), 1)
end)

s:test("_parse_level: numeric string -> floored integer", function()
    local log = fresh()
    T.eq(log._parse_level("3"), 3)
    T.eq(log._parse_level("2.9"), 2)
end)

s:test("_parse_level: nil / empty / unknown -> nil", function()
    local log = fresh()
    T.is_nil(log._parse_level(nil))
    T.is_nil(log._parse_level(""))
    T.is_nil(log._parse_level("BOGUS"))
    T.is_nil(log._parse_level(42))       -- non-string, non-nil
end)

-- ---------------------------------------------------------------------------
-- Env-var path: we shell out a fresh `./fan` with LUAFAN_LOG_LEVEL set,
-- inspect log.get_level() from that subprocess. This is the only way to
-- exercise the load-time env-var branch without contaminating the parent.
s:test("LUAFAN_LOG_LEVEL sets the initial level", function()
    -- arg[0] is our own script path; the running executable is what invoked
    -- us. On our test rig that's the built ./fan, so we can spawn it.
    local fan_bin = os.getenv("FAN_BIN") or arg[-1] or "./fan"
    if not fan_bin then
        print("[SKIP] no path to ./fan available for env-var subprocess test")
        return
    end
    local cmd = string.format(
        "LUAFAN_LOG_LEVEL=DEBUG %q -e 'print(require(\"fan.log\").get_level())'",
        fan_bin)
    local f = io.popen(cmd, "r")
    if not f then
        print("[SKIP] io.popen not available")
        return
    end
    local out = f:read("*a")
    f:close()
    T.truthy(out:match("^%s*4%s*$"), "expected level 4, got: " .. tostring(out))
end)

os.exit(T.run(s))
