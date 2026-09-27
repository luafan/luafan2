--[[
  test_config.lua — M14.E fan.config contract tests.

  Covers:
    - top-level `config` shim resolves to `fan.config`
    - missing directory → empty table + "not found" print, no raise
    - dotfile skip (".hidden.lua" not loaded)
    - non-.lua extension skip
    - single-file load: variables reach the returned table
    - multi-file load: all variables merged
    - env sandbox: `os`/`tonumber`/`weaktable`/`WORKDIR` reachable inside
      chunks but NOT leaked to the returned table
    - load error (syntax) is printed and skipped, not raised
    - exec error (runtime raise) is printed and skipped, not raised
    - _CONFIG_D_REGISTRY in-memory bundle path takes precedence over
      disk; ignores WORKDIR/config.d entirely
]]
local T = require("test_framework")
local fan = require("fan")

local s = T.suite("fan.config (M14.E)")

local posix = fan.posix
if not posix or not posix.readdir then
  print("[SKIP] fan.posix.readdir not available")
  os.exit(0)
end

-- Helpers ---------------------------------------------------------------

local function mkdir_p(dir)
  assert(os.execute("mkdir -p '" .. dir .. "'"))
end

local function rm_rf(dir)
  os.execute("rm -rf '" .. dir .. "'")
end

local function write_file(path, content)
  local f = assert(io.open(path, "w"))
  f:write(content)
  f:close()
end

-- Each test gets a private WORKDIR under /tmp/fan2-cfg-test-<pid>/<N>/.
-- The trailing slash matters: v1's configd_dir = WORKDIR .. "config.d",
-- so WORKDIR must end with "/" for the concatenation to hit the right
-- directory. Tests that verify no-slash behavior override WORKDIR
-- explicitly.
local BASE = "/tmp/fan2-cfg-test-" .. tostring(fan.getpid and fan.getpid() or os.time())
mkdir_p(BASE)

local case_seq = 0
local function new_workdir()
  case_seq = case_seq + 1
  local dir = string.format("%s/%d/", BASE, case_seq)
  mkdir_p(dir)
  return dir
end

-- Fresh module load: config caches into package.loaded, and its side
-- effects (reading _G.WORKDIR / _G._CONFIG_D_REGISTRY at require-time)
-- mean we must reset both slots before every scenario.
local function load_config()
  package.loaded["fan.config"] = nil
  package.loaded["config"]      = nil
  return require("fan.config")
end

-- ---------------------------------------------------------------------
-- The suite tears down BASE at the end via a final case. Individual
-- cases don't need to clean up — new_workdir() gives each one a fresh
-- subdirectory, and they're all under BASE.

s:test("top-level `config` shim resolves to fan.config", function()
  _G.WORKDIR = new_workdir()
  _G._CONFIG_D_REGISTRY = nil
  package.loaded["fan.config"] = nil
  package.loaded["config"]      = nil
  local via_fan  = require("fan.config")
  local via_flat = require("config")
  T.eq(via_flat, via_fan, "shim must return the same table (package.loaded shares state)")
end)

s:test("missing config.d directory → empty table, no raise", function()
  _G.WORKDIR = "/tmp/fan2-cfg-test-nonexistent-" .. tostring(os.time()) .. "-" .. tostring(math.random(1e9)) .. "/"
  _G._CONFIG_D_REGISTRY = nil
  local cfg = load_config()
  T.is_type(cfg, "table")
  T.eq(next(cfg), nil, "returned table must be empty when directory does not exist")
end)

s:test("single file: vars reach the returned table", function()
  _G.WORKDIR = new_workdir()
  _G._CONFIG_D_REGISTRY = nil
  local cd = _G.WORKDIR .. "config.d"
  mkdir_p(cd)
  write_file(cd .. "/base.lua", [[
    port      = 8080
    hostname  = "localhost"
    verbose   = tonumber("1") == 1
    homepath  = WORKDIR
    devpath   = os.getenv("PATH") ~= nil
  ]])
  local cfg = load_config()
  T.eq(cfg.port,     8080)
  T.eq(cfg.hostname, "localhost")
  T.eq(cfg.verbose,  true)
  T.eq(cfg.homepath, _G.WORKDIR)  -- WORKDIR visible in sandbox
  T.eq(cfg.devpath,  true)         -- os.getenv visible in sandbox
end)

s:test("multiple files: variables merged across chunks", function()
  _G.WORKDIR = new_workdir()
  _G._CONFIG_D_REGISTRY = nil
  local cd = _G.WORKDIR .. "config.d"
  mkdir_p(cd)
  write_file(cd .. "/a.lua", "alpha = 1\nshared = 'from-a'\n")
  write_file(cd .. "/b.lua", "beta = 2\n")
  -- v1 does not define ordering (readdir order). We only assert both
  -- variables reached the table; overwrite semantics are inherently
  -- filesystem-order-dependent.
  local cfg = load_config()
  T.eq(cfg.alpha, 1)
  T.eq(cfg.beta,  2)
  T.truthy(cfg.shared, "shared should be set by whichever file ran last")
end)

s:test("dotfiles are skipped", function()
  _G.WORKDIR = new_workdir()
  _G._CONFIG_D_REGISTRY = nil
  local cd = _G.WORKDIR .. "config.d"
  mkdir_p(cd)
  write_file(cd .. "/.hidden.lua",   "should_not_appear = true\n")
  write_file(cd .. "/visible.lua",   "should_appear = true\n")
  local cfg = load_config()
  T.eq(cfg.should_appear, true)
  T.is_nil(cfg.should_not_appear, "hidden.lua must not be loaded")
end)

s:test("non-.lua extensions are skipped", function()
  _G.WORKDIR = new_workdir()
  _G._CONFIG_D_REGISTRY = nil
  local cd = _G.WORKDIR .. "config.d"
  mkdir_p(cd)
  write_file(cd .. "/note.txt",      "should_not_appear = true\n")
  write_file(cd .. "/config.json",   "should_not_appear2 = true\n")
  write_file(cd .. "/real.lua",      "loaded = 1\n")
  local cfg = load_config()
  T.eq(cfg.loaded, 1)
  T.is_nil(cfg.should_not_appear)
  T.is_nil(cfg.should_not_appear2)
end)

s:test("env injected keys are NOT leaked to the returned table", function()
  _G.WORKDIR = new_workdir()
  _G._CONFIG_D_REGISTRY = nil
  local cd = _G.WORKDIR .. "config.d"
  mkdir_p(cd)
  write_file(cd .. "/x.lua", "answer = 42\n")
  local cfg = load_config()
  T.eq(cfg.answer, 42)
  -- v1 explicitly filters os/tonumber/weaktable/WORKDIR out of the
  -- return value so config consumers don't accidentally treat the
  -- sandbox scaffolding as user-supplied config.
  T.is_nil(cfg.os,        "os must not leak into returned table")
  T.is_nil(cfg.tonumber,  "tonumber must not leak into returned table")
  T.is_nil(cfg.weaktable, "weaktable must not leak into returned table")
  T.is_nil(cfg.WORKDIR,   "WORKDIR must not leak into returned table")
end)

s:test("load error (syntax) is printed and skipped, not raised", function()
  _G.WORKDIR = new_workdir()
  _G._CONFIG_D_REGISTRY = nil
  local cd = _G.WORKDIR .. "config.d"
  mkdir_p(cd)
  write_file(cd .. "/broken.lua", "this = = = not lua\n")
  write_file(cd .. "/ok.lua",     "ok_flag = true\n")
  local cfg = load_config()   -- must not raise
  T.eq(cfg.ok_flag, true, "the other file still loads after a syntax error")
end)

s:test("exec error (runtime raise) is printed and skipped, not raised", function()
  _G.WORKDIR = new_workdir()
  _G._CONFIG_D_REGISTRY = nil
  local cd = _G.WORKDIR .. "config.d"
  mkdir_p(cd)
  write_file(cd .. "/boom.lua", "error('kaboom during config load')\n")
  write_file(cd .. "/ok.lua",   "ok_flag = 'survived'\n")
  local cfg = load_config()   -- must not raise
  T.eq(cfg.ok_flag, "survived")
end)

s:test("_CONFIG_D_REGISTRY takes precedence over disk", function()
  _G.WORKDIR = new_workdir()
  local cd = _G.WORKDIR .. "config.d"
  mkdir_p(cd)
  -- Write a disk file that would set a distinctive flag. If the
  -- registry path is honored, this file MUST NOT be executed.
  write_file(cd .. "/disk.lua", "disk_flag = true\n")

  _G._CONFIG_D_REGISTRY = {
    ["mem-a.lua"] = "from_mem_a = 1\n",
    ["mem-b.lua"] = "from_mem_b = 'yes'\n",
  }
  local cfg = load_config()
  T.eq(cfg.from_mem_a, 1)
  T.eq(cfg.from_mem_b, "yes")
  T.is_nil(cfg.disk_flag, "disk config.d must be ignored when registry is set")
  _G._CONFIG_D_REGISTRY = nil
end)

s:test("_CONFIG_D_REGISTRY bundle errors are printed, not raised", function()
  _G.WORKDIR = new_workdir()
  _G._CONFIG_D_REGISTRY = {
    ["broken.lua"] = "@@@ not lua @@@",
    ["boom.lua"]   = "error('bundle exec crash')",
    ["ok.lua"]     = "survived = true",
  }
  local cfg = load_config()
  T.eq(cfg.survived, true, "the healthy bundle still loads")
  _G._CONFIG_D_REGISTRY = nil
end)

-- Final teardown --------------------------------------------------------
s:test("cleanup: remove test working directories", function()
  rm_rf(BASE)
  -- reset globals so subsequent test files don't inherit our state
  _G.WORKDIR = nil
  _G._CONFIG_D_REGISTRY = nil
  package.loaded["fan.config"] = nil
  package.loaded["config"]      = nil
  T.truthy(true)
end)

os.exit(T.run(s))
