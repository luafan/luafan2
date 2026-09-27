--[[
  fan/log.lua — LuaFan v2 leveled logger (M8).

  Behavioural contract (v1 fan.log compat):
    - Five levels: ERROR=1 WARN=2 INFO=3 DEBUG=4 TRACE=5.
    - `log.<level>(...)` prints "[LEVEL] arg1 arg2 ..." via `log.adapter`
      **only when** the current level allows it.
    - `log.is<Level>(category)` returns true iff the level is enabled AND
      (no category given, or the category flag is set).
    - `log.adapter` is a hookable print-like function (default: print).

  Differences from v1 (why we rewrote instead of copying):
    - v1 pulled its configuration out of a project-wide `require "config"`
      module. luafan2 has no such module, so we replaced that coupling
      with explicit setters (`set_level`, `set_category`) plus an
      environment-variable convenience (`LUAFAN_LOG_LEVEL`).
    - v1 used the string keys "loglevel" / <category> directly on the
      config table; we keep the same names in `log.config` for anyone
      migrating v1 code by just aliasing `require("fan.log").config` to
      their old `config` table.
]]

local M = {}

-- ---- level table (public, so callers can spell M.LEVELS.DEBUG) -----------
M.LEVELS = {
    ERROR = 1,
    WARN  = 2,
    INFO  = 3,
    DEBUG = 4,
    TRACE = 5,
}

-- ---- configuration -------------------------------------------------------
-- Publicly readable so v1 code that used `config.loglevel = 3` still works
-- if it does `require("fan.log").config.loglevel = 3`. New code should
-- prefer the setters below.
M.config = { loglevel = M.LEVELS.INFO }

-- Honour LUAFAN_LOG_LEVEL at load time. Accepts the level name (case-
-- insensitive) or a bare integer. Anything unrecognised is ignored.
-- Exposed as M._parse_level so tests can hit every branch without
-- needing to fork a subprocess per input.
local function parse_level(v)
    if type(v) ~= "string" or v == "" then return nil end
    local n = tonumber(v)
    if n then return math.floor(n) end
    return M.LEVELS[v:upper()]
end
M._parse_level = parse_level
do
    local lv = parse_level(os.getenv("LUAFAN_LOG_LEVEL"))
    if lv then M.config.loglevel = lv end
end

function M.set_level(level)
    -- Accept either a number (1..5) or a name ("DEBUG").
    local n = type(level) == "number" and level or M.LEVELS[tostring(level):upper()]
    if not n then error("fan.log: unknown level " .. tostring(level), 2) end
    M.config.loglevel = n
end

function M.get_level()
    return M.config.loglevel
end

function M.set_category(name, enabled)
    -- v1 tested truthiness of config[category]; we normalise to boolean.
    M.config[name] = enabled and true or false
end

-- ---- output adapter ------------------------------------------------------
-- The adapter takes the level-prefixed tag as its first arg followed by
-- the caller's varargs, mirroring v1. Default is Lua's `print`.
M.adapter = print

function M.set_adapter(fn)
    if type(fn) ~= "function" then
        error("fan.log: adapter must be a function", 2)
    end
    M.adapter = fn
end

-- ---- level predicates ----------------------------------------------------
-- Factored so `is<Level>` and `<level>()` share one implementation. The
-- category check mirrors v1: no category always passes; a category name
-- requires `config[name]` to be truthy.
local function enabled(threshold, category)
    if M.config.loglevel < threshold then return false end
    if category == nil then return true end
    return M.config[category] and true or false
end

function M.isError(cat)  return enabled(M.LEVELS.ERROR, cat) end
function M.isWarn(cat)   return enabled(M.LEVELS.WARN,  cat) end
function M.isInfo(cat)   return enabled(M.LEVELS.INFO,  cat) end
function M.isDebug(cat)  return enabled(M.LEVELS.DEBUG, cat) end
function M.isTrace(cat)  return enabled(M.LEVELS.TRACE, cat) end

-- ---- emitters ------------------------------------------------------------
-- v1's emitters don't take a category argument; only `is<Level>` do. So
-- we keep the emitter shape as (...) — matches v1 exactly.
local function emit(tag, threshold, ...)
    if M.config.loglevel >= threshold then M.adapter(tag, ...) end
end

function M.error(...) emit("[ERROR]", M.LEVELS.ERROR, ...) end
function M.warn(...)  emit("[WARN]",  M.LEVELS.WARN,  ...) end
function M.info(...)  emit("[INFO]",  M.LEVELS.INFO,  ...) end
function M.debug(...) emit("[DEBUG]", M.LEVELS.DEBUG, ...) end
function M.trace(...) emit("[TRACE]", M.LEVELS.TRACE, ...) end

return M
