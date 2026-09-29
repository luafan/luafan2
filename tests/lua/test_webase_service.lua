--[[
  test_webase_service.lua — unit tests for webase/service.lua.

  service.lua walks (WORKDIR or "") .. "service/" via fan.posix.readdir
  + fan.posix.stat, loads every *.lua into a per-file sandbox, and
  exposes a metatable-driven dispatcher `svc.<method>(name, ...)`.
  Coverage of webase/service.lua from test_webase.lua alone is 64.9%
  (27 miss / 77) because the integration path only ever registers
  services indirectly; it never actually calls into them via
  `svc.<method>`. These unit tests cover the miss set directly:

    - load_path directory recursion
    - loadfile syntax-error branch
    - pcall exec-error branch
    - onXxx -> xxx alias normalisation
    - `svc.<method>(name)`  found + method exists  -> pcall true
    - `svc.<method>(name)`  service missing        -> false, err
    - `svc.<method>(name)`  method missing on svc  -> false, err
    - `svc.<method>()`      broadcast, all-pass    -> true
    - `svc.<method>()`      broadcast, one fails   -> false, err
    - `svc.<method>()`      broadcast, method missing -> dump + false, err
    - list()                -> array
    - get(name)             -> single table
    - _SERVICE_REGISTRY bundle path (skips disk)
    - bundle load error / bundle exec error branches
]]

local T   = require("test_framework")
local fan = require("fan")

local this_dir = (debug.getinfo(1, "S").source:gsub("^@", ""):match("(.*)/[^/]+$")) or "."
local ROOT = this_dir:gsub("/tests/lua$", "")
-- webase/service.lua does `require "json"` which the webase tree ships
-- as a thin shim; add webase/ so both requires resolve.
package.path = ROOT .. "/webase/?.lua;" .. package.path

local s = T.suite("webase.service (M12.2)")

local posix = fan.posix
if not posix or not posix.readdir or not posix.stat then
  print("[SKIP] fan.posix.readdir/stat not available")
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

local BASE = "/tmp/fan2-service-test-" .. tostring(fan.getpid and fan.getpid() or os.time())
mkdir_p(BASE)

local case_seq = 0
local function new_workdir()
  case_seq = case_seq + 1
  -- Trailing slash matters: service.lua does `WORKDIR .. "service"`.
  local dir = string.format("%s/%d/", BASE, case_seq)
  mkdir_p(dir)
  return dir
end

-- Every case reloads service so its top-level scan runs against a
-- fresh WORKDIR / _SERVICE_REGISTRY combination.
local function load_service()
  package.loaded["service"] = nil
  return require("service")
end

-- ---- disk load path ------------------------------------------------------

s:test("missing service/ directory: load_path is a no-op", function()
  _G.WORKDIR = "/tmp/fan2-service-nonexistent-" .. tostring(os.time()) .. "-" .. tostring(math.random(1e9)) .. "/"
  _G._SERVICE_REGISTRY = nil
  local svc = load_service()
  T.is_type(svc, "table")
  T.is_type(svc.list, "function")
  T.eq(#svc.list(), 0)
end)

s:test("single service file: loaded, listed, gettable by name", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local sd = _G.WORKDIR .. "service"
  mkdir_p(sd)
  write_file(sd .. "/greeter.lua", [[
    name = "greeter"
    function onHello(who) return "hi " .. tostring(who) end
  ]])
  local svc = load_service()
  local one = svc.get("greeter")
  T.not_nil(one)
  T.eq(one.name, "greeter")
  -- onXxx should be mirrored to xxx.
  T.is_type(one.hello, "function")
  local lst = svc.list()
  T.eq(#lst, 1)
  T.eq(lst[1].name, "greeter")
end)

s:test("recursive directory: nested *.lua files are loaded", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local sd = _G.WORKDIR .. "service"
  -- v1-quirk-preserving detail: service.lua's `if name endswith .lua`
  -- filter fires for BOTH files and directories, so only subdirectories
  -- named "*.lua" get recursed into. This is by design (v1 shipped it
  -- that way, and the fixture layout depends on it — `handle/*.lua/
  -- foo.lua` layouts are rare but exist). Test the recursion path with
  -- a directory that matches the filter.
  mkdir_p(sd .. "/nested.lua")
  write_file(sd .. "/a.lua",            [[name = "a"; function onPing() return "a-ok" end]])
  write_file(sd .. "/nested.lua/b.lua", [[name = "b"; function onPing() return "b-ok" end]])
  local svc = load_service()
  T.not_nil(svc.get("a"))
  T.not_nil(svc.get("b"), "b.lua under nested.lua/ must be picked up via recursion")
end)

s:test("dotfiles and non-.lua entries are skipped", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local sd = _G.WORKDIR .. "service"
  mkdir_p(sd)
  write_file(sd .. "/.hidden.lua", [[name = "hidden"]])
  write_file(sd .. "/notes.txt",   "just notes")
  write_file(sd .. "/real.lua",    [[name = "real"]])
  local svc = load_service()
  T.not_nil(svc.get("real"))
  T.is_nil(svc.get("hidden"))
end)

-- ---- error paths on disk ------------------------------------------------

s:test("load error (syntax) is printed and skipped, not raised", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local sd = _G.WORKDIR .. "service"
  mkdir_p(sd)
  write_file(sd .. "/broken.lua", "this = = = not lua\n")
  write_file(sd .. "/ok.lua",     [[name = "ok"]])
  local svc = load_service()   -- must not raise
  T.not_nil(svc.get("ok"))
end)

s:test("exec error (runtime raise) is printed and skipped, not raised", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local sd = _G.WORKDIR .. "service"
  mkdir_p(sd)
  write_file(sd .. "/boom.lua", [[error("kaboom during service load")]])
  write_file(sd .. "/ok.lua",   [[name = "ok"]])
  local svc = load_service()   -- must not raise
  T.not_nil(svc.get("ok"))
end)

-- ---- dispatcher: `svc.method(name, ...)` --------------------------------

s:test("dispatch: found service + method returns pcall result", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local sd = _G.WORKDIR .. "service"
  mkdir_p(sd)
  write_file(sd .. "/echo.lua", [[
    name = "echo"
    function onPing(x) return "pong:" .. tostring(x) end
  ]])
  local svc = load_service()
  local ok, result = svc.ping("echo", "hello")
  T.eq(ok, true)
  T.eq(result, "pong:hello")
end)

s:test("dispatch: unknown service returns false + '[name.method] not found'", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local svc = load_service()
  local ok, err = svc.ping("does_not_exist")
  T.eq(ok, false)
  T.truthy(err:find("not found"))
end)

s:test("dispatch: known service, unknown method returns false + err", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local sd = _G.WORKDIR .. "service"
  mkdir_p(sd)
  write_file(sd .. "/e.lua", [[name = "e"; function onOnly() return true end]])
  local svc = load_service()
  local ok, err = svc.missing_method("e")
  T.eq(ok, false)
  T.truthy(err:find("not found"))
end)

-- ---- dispatcher: broadcast `svc.method()` (name = nil) ------------------

s:test("broadcast: every service's method runs, all-pass returns true", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local sd = _G.WORKDIR .. "service"
  mkdir_p(sd)
  write_file(sd .. "/a.lua", [[
    name = "a"
    _G.HITS = _G.HITS or {}
    function onStart() _G.HITS.a = (_G.HITS.a or 0) + 1 end
  ]])
  write_file(sd .. "/b.lua", [[
    name = "b"
    function onStart() _G.HITS.b = (_G.HITS.b or 0) + 1 end
  ]])
  _G.HITS = nil
  local svc = load_service()
  local ok = svc.start()
  T.eq(ok, true)
  T.eq(_G.HITS.a, 1)
  T.eq(_G.HITS.b, 1)
  _G.HITS = nil
end)

s:test("broadcast: one service raises -> propagates (false, err)", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local sd = _G.WORKDIR .. "service"
  mkdir_p(sd)
  -- Only one service so ordering doesn't matter (readdir order is
  -- undefined; if the failing service ran second the surviving-first
  -- assertion would still be flaky).
  write_file(sd .. "/boom.lua", [[
    name = "boom"
    function onStart() error("service boom") end
  ]])
  local svc = load_service()
  local ok, err = svc.start()
  T.eq(ok, false)
  T.truthy(err and tostring(err):find("service boom"))
end)

s:test("broadcast: missing method on one service dumps + returns false, err", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local sd = _G.WORKDIR .. "service"
  mkdir_p(sd)
  -- Single service that lacks the requested method -> hits the
  -- "for k2,v2 in pairs(v) do print end" v1-quirk branch, then
  -- returns false, "[method] not found".
  write_file(sd .. "/plain.lua", [[
    name = "plain"
    function onKnown() return true end
  ]])
  local svc = load_service()
  local ok, err = svc.unknownBroadcast()
  T.eq(ok, false)
  T.truthy(err and tostring(err):find("not found"))
end)

-- ---- alias normalisation -------------------------------------------------

s:test("aliases: onXxx methods are mirrored to lowercase xxx", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = nil
  local sd = _G.WORKDIR .. "service"
  mkdir_p(sd)
  write_file(sd .. "/al.lua", [[
    name = "al"
    function onGetStatus() return "ok" end
  ]])
  local svc = load_service()
  local m = svc.get("al")
  T.is_type(m.onGetStatus, "function")
  T.is_type(m.getstatus,   "function", "lowercase alias for onGetStatus")
end)

-- ---- _SERVICE_REGISTRY bundle path --------------------------------------

s:test("_SERVICE_REGISTRY: bundle bytecode takes precedence over disk", function()
  _G.WORKDIR = new_workdir()
  local sd = _G.WORKDIR .. "service"
  mkdir_p(sd)
  -- Disk file that would set a distinctive flag. If the registry
  -- branch is honored, this must NOT execute.
  write_file(sd .. "/disk.lua", [[name = "disk"]])

  -- Bundle: pre-compiled bytecode strings. Use a plain source string
  -- that `load(...)` accepts (mode "b" per service.lua, but Lua
  -- accepts source-with-"b" only if it's a binary chunk — so we pass
  -- pre-dumped bytecode).
  local chunk = load([[
    name = "bundled"
    function onHello() return "from-bundle" end
  ]])
  local bytecode = string.dump(chunk)
  _G._SERVICE_REGISTRY = { ["bundled_mod"] = bytecode }
  local svc = load_service()
  T.not_nil(svc.get("bundled"), "bundled service loaded from registry")
  T.is_nil(svc.get("disk"),     "disk service must be ignored")
  local ok, out = svc.hello("bundled")
  T.eq(ok, true)
  T.eq(out, "from-bundle")
  _G._SERVICE_REGISTRY = nil
end)

s:test("_SERVICE_REGISTRY: bad bytecode prints and moves on", function()
  _G.WORKDIR = new_workdir()
  _G._SERVICE_REGISTRY = {
    ["broken_mod"] = "\x1b\x00\x00\x00 not real bytecode",
  }
  -- Add a good one alongside so we can assert the load loop continued.
  local good = load([[name = "survived"]])
  _G._SERVICE_REGISTRY["ok_mod"] = string.dump(good)
  local svc = load_service()
  T.not_nil(svc.get("survived"))
  _G._SERVICE_REGISTRY = nil
end)

s:test("_SERVICE_REGISTRY: bundle exec error is printed, not raised", function()
  _G.WORKDIR = new_workdir()
  local boom = load([[error("bundle boom")]])
  local good = load([[name = "healthy"]])
  _G._SERVICE_REGISTRY = {
    ["boom_mod"]    = string.dump(boom),
    ["healthy_mod"] = string.dump(good),
  }
  local svc = load_service()   -- must not raise
  T.not_nil(svc.get("healthy"))
  _G._SERVICE_REGISTRY = nil
end)

s:test("_SERVICE_REGISTRY: entry without explicit name uses modname tail", function()
  _G.WORKDIR = new_workdir()
  local chunk = load([[function onPing() return true end]])
  _G._SERVICE_REGISTRY = {
    ["app.sub.autoname"] = string.dump(chunk),
  }
  local svc = load_service()
  -- Fallback name = last "." segment = "autoname".
  T.not_nil(svc.get("autoname"))
  _G._SERVICE_REGISTRY = nil
end)

-- ---- final cleanup -------------------------------------------------------

s:test("cleanup: remove temp working directories", function()
  rm_rf(BASE)
  _G.WORKDIR = nil
  _G._SERVICE_REGISTRY = nil
  package.loaded["service"] = nil
  T.truthy(true)
end)

os.exit(T.run(s))
