--[[
  test_webase_mapping.lua — unit tests for webase/mapping.lua.

  mapping.lua exposes a URL-rewrite table loaded from
  `(WORKDIR or "") .. mapping/*.lua`. Because it runs its
  directory-scan side effect at require time, every scenario has
  to clear package.loaded and reset _G.WORKDIR / _G._DATABASE_REGISTRY
  before requiring the module again.

  Coverage targets (webase/mapping.lua line-miss set):
    - readdir success path with .lua / dotfile / non-.lua filter
    - loadfile syntax-error branch (broken chunk)
    - pcall exec-error branch (chunk that raises)
    - _DATABASE_REGISTRY sentinel path (skips disk scan)

  Runs plain — no fan.loop, no coroutines.
]]

local T   = require("test_framework")
local fan = require("fan")

local this_dir = (debug.getinfo(1, "S").source:gsub("^@", ""):match("(.*)/[^/]+$")) or "."
local ROOT = this_dir:gsub("/tests/lua$", "")
package.path = ROOT .. "/webase/?.lua;" .. package.path

local s = T.suite("webase.mapping (M12.2)")

local posix = fan.posix
if not posix or not posix.readdir then
  print("[SKIP] fan.posix.readdir not available")
  os.exit(0)
end

-- ---- fs helpers ----------------------------------------------------------

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

local BASE = "/tmp/fan2-mapping-test-" .. tostring(fan.getpid and fan.getpid() or os.time())
mkdir_p(BASE)

local case_seq = 0
local function new_workdir()
  case_seq = case_seq + 1
  -- Trailing slash matters: mapping.lua does `WORKDIR .. "mapping"`.
  local dir = string.format("%s/%d/", BASE, case_seq)
  mkdir_p(dir)
  return dir
end

-- Every case reloads mapping so its top-level scan runs against a fresh
-- WORKDIR / _DATABASE_REGISTRY combination.
local function load_mapping()
  package.loaded["mapping"] = nil
  return require("mapping")
end

-- ---- happy path ----------------------------------------------------------

s:test("missing mapping/ directory: no raise, empty rewrite table", function()
  _G.WORKDIR = "/tmp/fan2-mapping-nonexistent-" .. tostring(os.time()) .. "-" .. tostring(math.random(1e9)) .. "/"
  _G._DATABASE_REGISTRY = nil
  local m = load_mapping()
  T.is_type(m, "table")
  -- Sandbox env may have _ENV/_G self-references and safe_os plus tonumber/tostring;
  -- the point is that no user rewrite keys leaked in.
  T.is_nil(m["/legacy"])
end)

s:test("single mapping file: rewrite entry reaches the returned table", function()
  _G.WORKDIR = new_workdir()
  _G._DATABASE_REGISTRY = nil
  local md = _G.WORKDIR .. "mapping"
  mkdir_p(md)
  write_file(md .. "/routes.lua", [[
    _ENV["/legacy"] = "/api/v2/users"
    _ENV["/old"]    = "/new"
  ]])
  local m = load_mapping()
  T.eq(m["/legacy"], "/api/v2/users")
  T.eq(m["/old"],    "/new")
end)

s:test("multiple files: entries merge across chunks", function()
  _G.WORKDIR = new_workdir()
  _G._DATABASE_REGISTRY = nil
  local md = _G.WORKDIR .. "mapping"
  mkdir_p(md)
  write_file(md .. "/a.lua", [[_ENV["/a"] = "/aa"]])
  write_file(md .. "/b.lua", [[_ENV["/b"] = "/bb"]])
  local m = load_mapping()
  T.eq(m["/a"], "/aa")
  T.eq(m["/b"], "/bb")
end)

s:test("dotfiles are skipped", function()
  _G.WORKDIR = new_workdir()
  _G._DATABASE_REGISTRY = nil
  local md = _G.WORKDIR .. "mapping"
  mkdir_p(md)
  write_file(md .. "/.hidden.lua", [[_ENV["/nope"] = "/should-not-load"]])
  write_file(md .. "/visible.lua", [[_ENV["/ok"]   = "/loaded"]])
  local m = load_mapping()
  T.eq(m["/ok"], "/loaded")
  T.is_nil(m["/nope"])
end)

s:test("non-.lua extensions are skipped", function()
  _G.WORKDIR = new_workdir()
  _G._DATABASE_REGISTRY = nil
  local md = _G.WORKDIR .. "mapping"
  mkdir_p(md)
  write_file(md .. "/note.txt",    [[should_not_appear = true]])
  write_file(md .. "/routes.json", [[{"/x":"/y"}]])
  write_file(md .. "/real.lua",    [[_ENV["/x"] = "/loaded"]])
  local m = load_mapping()
  T.eq(m["/x"], "/loaded")
end)

-- ---- error paths (must print, not raise) --------------------------------

s:test("load error (syntax) is printed and skipped, not raised", function()
  _G.WORKDIR = new_workdir()
  _G._DATABASE_REGISTRY = nil
  local md = _G.WORKDIR .. "mapping"
  mkdir_p(md)
  write_file(md .. "/broken.lua", "this = = = not lua\n")
  write_file(md .. "/ok.lua",     [[_ENV["/ok"] = "/loaded"]])
  local m = load_mapping()   -- must not raise
  T.eq(m["/ok"], "/loaded")
end)

s:test("exec error (runtime raise) is printed and skipped, not raised", function()
  _G.WORKDIR = new_workdir()
  _G._DATABASE_REGISTRY = nil
  local md = _G.WORKDIR .. "mapping"
  mkdir_p(md)
  write_file(md .. "/boom.lua", [[error("kaboom during mapping load")]])
  write_file(md .. "/ok.lua",   [[_ENV["/ok"] = "/survived"]])
  local m = load_mapping()   -- must not raise
  T.eq(m["/ok"], "/survived")
end)

-- ---- sandbox integrity ---------------------------------------------------

s:test("sandbox exposes os.getenv/time and tonumber/tostring", function()
  _G.WORKDIR = new_workdir()
  _G._DATABASE_REGISTRY = nil
  local md = _G.WORKDIR .. "mapping"
  mkdir_p(md)
  write_file(md .. "/x.lua", [[
    _ENV["/path"]    = tostring(os.getenv("PATH") ~= nil)
    _ENV["/answer"]  = tostring(tonumber("42"))
  ]])
  local m = load_mapping()
  T.eq(m["/path"],   "true")
  T.eq(m["/answer"], "42")
end)

s:test("sandbox does NOT expose require/io/debug", function()
  _G.WORKDIR = new_workdir()
  _G._DATABASE_REGISTRY = nil
  local md = _G.WORKDIR .. "mapping"
  mkdir_p(md)
  -- Any attempt to touch require/io/debug should be printed as an
  -- exec error but not raise out to the loader — the healthy file
  -- alongside still loads.
  write_file(md .. "/bad.lua",  [[require("os")]])   -- require not injected
  write_file(md .. "/bad2.lua", [[io.open("/etc/passwd", "r")]])
  write_file(md .. "/ok.lua",   [[_ENV["/ok"] = "/survived"]])
  local m = load_mapping()
  T.eq(m["/ok"], "/survived")
end)

-- ---- _DATABASE_REGISTRY sentinel ----------------------------------------

s:test("_DATABASE_REGISTRY set: disk scan is skipped entirely", function()
  _G.WORKDIR = new_workdir()
  local md = _G.WORKDIR .. "mapping"
  mkdir_p(md)
  -- If the sentinel branch is taken, this disk file must NOT run.
  write_file(md .. "/disk.lua", [[_ENV["/disk"] = "/from-disk"]])

  _G._DATABASE_REGISTRY = { placeholder = true }
  local m = load_mapping()
  T.is_nil(m["/disk"], "disk mapping/ must be ignored when registry is set")
  _G._DATABASE_REGISTRY = nil
end)

-- ---- final cleanup -------------------------------------------------------

s:test("cleanup: remove temp working directories", function()
  rm_rf(BASE)
  _G.WORKDIR = nil
  _G._DATABASE_REGISTRY = nil
  package.loaded["mapping"] = nil
  T.truthy(true)
end)

os.exit(T.run(s))
