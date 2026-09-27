--[[
  fan.config — sandboxed loader for config.d/*.lua bundles (M14.E).

  Port of v1's `require "config"` (tmp/luafan/modules/config.lua). Same
  contract, one dependency swap (v1 used `lfs.dir`; v2 uses
  `fan.posix.readdir` — see src/sys/posix.c :: l_readdir).

  Behavior:
    1. If _G._CONFIG_D_REGISTRY is a table {name = source_string, ...},
       load each entry as a chunk named "@config.d/<name>" and IGNORE
       the on-disk directory. Used for embedded / bundled configs.
    2. Otherwise scan `(WORKDIR or "") .. "config.d"` for files whose
       name does not start with "." and ends with MODULE_EXT (default
       ".lua"). Load each via `loadfile(path, MODULE_LOAD_MODE, env)`.
    3. Errors during load or pcall are printed and skipped — never
       propagated. This matches v1: config is best-effort, not a hard
       dependency.
    4. Missing directory prints "config [<path>] not found, ignored."
       and returns an empty table (also matches v1).

  Sandbox env:
    { os, tonumber, weaktable, WORKDIR }
  (WORKDIR is the ambient global; `weaktable` is a shared __mode="v"
  table config chunks can use for cached cross-file references.)

  Returned table contains every key the config chunks assigned to
  the env, EXCLUDING the four injected keys above. Config chunks are
  responsible for their own defaults.

  Compat rationale (why keep this exact shape):
    v1 code often does `local cfg = require "config"` and then reads
    `cfg.maria_host`, `cfg.pool_size`, etc. Any deviation from v1's
    env keys or filtering would silently corrupt those apps.
]]

local fan = require("fan")
local posix = fan.posix

local MODULE_EXT       = _G.MODULE_EXT       or ".lua"
local MODULE_LOAD_MODE = _G.MODULE_LOAD_MODE or "bt"

local configd_dir = (_G.WORKDIR or "") .. "config.d"

local env = {
  os        = os,
  tonumber  = tonumber,
  weaktable = {},
  WORKDIR   = _G.WORKDIR,
}
setmetatable(env.weaktable, { __mode = "v" })

local function load_registry(registry)
  for name, src in pairs(registry) do
    local chunk, err = load(src, "@config.d/" .. name, "t", env)
    if not chunk then
      print(string.format("[config] bundle load error %s: %s", name, tostring(err)))
    else
      local ok, ret = pcall(chunk)
      if not ok then
        print(string.format("[config] bundle exec error %s: %s", name, tostring(ret)))
      end
    end
  end
end

local function load_dir(dir)
  local entries, err = posix.readdir(dir)
  if not entries then
    -- v1 checks `attr and attr.mode == "directory"`; we can't tell mode
    -- from readdir failure alone, but the observable behavior is the
    -- same: print a "not found" note and return.
    print(string.format("config [%s] not found, ignored.", dir))
    return
  end
  -- v1 iterates in filesystem order (readdir order). Preserve that by
  -- iterating the array as-is. Skip "." and ".." implicitly via the
  -- dotfile filter.
  for _, name in ipairs(entries) do
    if name:sub(1, 1) ~= "." and name:sub(-#MODULE_EXT) == MODULE_EXT then
      local path = string.format("%s/%s", dir, name)
      local chunk, load_err = loadfile(path, MODULE_LOAD_MODE, env)
      if not chunk then
        print(string.format("[config] load error %s: %s", path, tostring(load_err)))
      else
        local ok, ret = pcall(chunk)
        if not ok then
          print(string.format("[config] exec error %s: %s", path, tostring(ret)))
        end
      end
    end
  end
end

if type(_G._CONFIG_D_REGISTRY) == "table" then
  load_registry(_G._CONFIG_D_REGISTRY)
else
  load_dir(configd_dir)
end

local t = {}
for k, v in pairs(env) do
  if k ~= "os" and k ~= "tonumber" and k ~= "weaktable" and k ~= "WORKDIR" then
    t[k] = v
  end
end

return t
