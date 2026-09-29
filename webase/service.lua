-- webase/service.lua — long-lived background services loaded from
-- `(WORKDIR or "") .. service`.
--
-- Each file becomes a "service module" whose top-level assignments (name,
-- onStart, onStop, getStatus, ...) are collected into service_map. The
-- returned module supports:
--   svc.list()          -> array of service tables
--   svc.get(name)       -> single service table by name
--   svc.start()         -> pcall onStart on every service, stop on first fail
--   svc.stop()          -> pcall onStop on every service
--   svc.<any>(name?, ...) -> call the on<Any> method on `name` (or every
--                            service if `name` is nil); returns pcall result
--
-- We also normalise every `onXxx` handler to a lowercase alias `xxx`, so
-- callers can write `svc.status(name)` in addition to `svc.getStatus(name)`.
--
-- v1 change: v1 used `lfs.attributes` + `lfs.dir` to walk the directory;
-- v2 uses fan.posix.readdir + fan.posix.stat. The traversal shape is
-- identical: filename filter (dotfile-skip + MODULE_EXT match), then
-- loadfile with a per-file environment table.

local fan   = require "fan"
local posix = fan.posix

local print   = print
local pcall   = pcall
local require = require
local json    = require "json"
local string  = string

local MODULE_EXT = MODULE_EXT or ".lua"
local MODULE_LOAD_MODE = MODULE_LOAD_MODE or "bt"

local service_map = {}

local function load_path(path)
  local st = posix.stat(path)
  if not st then return end
  if st.mode == "directory" then
    local entries = posix.readdir(path) or {}
    for _, name in ipairs(entries) do
      if name:sub(1,1) ~= "." and name:sub(-#MODULE_EXT) == MODULE_EXT then
        load_path(string.format("%s/%s", path, name))
      end
    end
  else
    -- Fresh sandboxed env; delegate to _G for library lookups so services
    -- can `require "fan"` etc. This matches v1 exactly.
    local m = setmetatable({}, { __index = _G })
    local func, msg = loadfile(path, MODULE_LOAD_MODE, m)
    if not func then
      print("[service] load error: " .. path .. ": " .. tostring(msg))
    else
      local ok, err = pcall(func)
      if not ok then
        print("[service] exec error: " .. path .. ": " .. tostring(err))
      end
    end

    m.name = m.name or path:match("([^/]+)" .. MODULE_EXT:gsub("%.","%%.") .. "$")
    if m.name then
      service_map[m.name] = m

      -- Normalise onXxx -> xxx aliases in a second pass so the loop
      -- above doesn't observe its own inserts (v1 does the same).
      local tmp = {}
      for k,v in pairs(m) do
        if string.find(k:lower(), "on", 1, true) == 1 then
          tmp[k:sub(3):lower()] = v
        end
      end

      for k,v in pairs(tmp) do
        m[k] = v
      end
    end
  end
end

if _SERVICE_REGISTRY then
  -- Optional in-memory service map (for embedded / bundled apps). Same
  -- sentinel name webase v1 used.
  for modname, bytecode in pairs(_SERVICE_REGISTRY) do
    local m = setmetatable({}, { __index = _G })
    local chunk, load_err = load(bytecode, "@" .. modname, "b", m)
    if not chunk then
      print("[service] bundle load error: " .. modname .. ": " .. tostring(load_err))
    else
      local ok, err = pcall(chunk)
      if not ok then
        print("[service] bundle exec error: " .. modname .. ": " .. tostring(err))
      end
    end
    m.name = m.name or modname:match("([^%.]+)$")
    if m.name then
      service_map[m.name] = m
      local tmp = {}
      for k,v in pairs(m) do
        if string.find(k:lower(), "on", 1, true) == 1 then
          tmp[k:sub(3):lower()] = v
        end
      end
      for k,v in pairs(tmp) do
        m[k] = v
      end
    end
  end
else
  load_path((WORKDIR or "") .. "service")
end

local function eval(method, name, ...)
  if name then
    if service_map[name] and service_map[name][method] then
      return pcall(service_map[name][method], ...)
    else
      return false, string.format("[%s.%s] not found", name, method)
    end
  else
    for k,v in pairs(service_map) do
      if v[method] then
        local st, msg = pcall(v[method])
        if not st then
          return st, msg
        end
      else
        -- v1 quirk: dump the service's own fields to stdout when a
        -- broadcast method is missing. Kept for parity because some
        -- v1 debugging tools rely on the trace.
        for k2, v2 in pairs(v) do
          print(k2, v2)
        end
        return false, string.format("[%s] not found", method)
      end
    end

    return true
  end
end

local mt = {}

function mt:__index(key)
  return function(name, ...)
    return eval(key, name, ...)
  end
end

local function list()
  local t = {}
  for _, v in pairs(service_map) do
    table.insert(t, v)
  end

  return t
end

local function get(name)
  return service_map[name]
end

return setmetatable({
    list = list,
    get = get,
  }, mt)
