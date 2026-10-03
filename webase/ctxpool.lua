-- ctxpool.lua — compatibility facade for the retired webase ctxpool.
--
-- The v1 module recursively scanned WORKDIR/database/, merged the returned
-- schema tables, and exposed a pool whose pop() returned an ORM context.
-- LuaFan v2 keeps that application-facing facade while using its split
-- primitives: fan.mariadb.pool for connections and fan.orm for models.
--
-- Compatibility surface:
--   local ctxpool = require "ctxpool"
--   local ctx = ctxpool:pop()
--   local User = ctx.models.users
--   ctxpool:push(ctx)
--   ctxpool:safe(function(ctx) ... end)

local fan = require("fan")
local posix = fan.posix
local mariadb_pool = require("fan.mariadb.pool")
local orm = require("fan.orm")

local MODULE_EXT = _G.MODULE_EXT or ".lua"
local MODULE_LOAD_MODE = _G.MODULE_LOAD_MODE or "bt"
local workdir = _G.WORKDIR or ""
local database_dir = workdir .. "database"
local config = require("config")

local function env_or_config(key, env_name)
  local value = config[key]
  if value ~= nil then return value end
  return os.getenv(env_name)
end

local function load_schemas()
  local schemas = {}

  local function load_file(path)
    local env = setmetatable({ WORKDIR = workdir }, { __index = _G })
    local chunk, err = loadfile(path, MODULE_LOAD_MODE, env)
    if not chunk then
      print("[ctxpool] load error: " .. path .. ": " .. tostring(err))
      return
    end
    local ok, returned = pcall(chunk)
    if not ok then
      print("[ctxpool] exec error: " .. path .. ": " .. tostring(returned))
      return
    end
    if type(returned) ~= "table" then return end
    local single_name = returned.name or returned.table
    if single_name and type(returned.schema) == "table" then
      schemas[single_name] = returned.schema
      return
    end
    for name, schema in pairs(returned) do
      if type(name) == "string" and type(schema) == "table" then
        schemas[name] = schema
      end
    end
  end

  local function load_path(path)
    local attr = posix.stat(path)
    if not attr then return end
    if attr.mode == "directory" then
      local entries = posix.readdir(path)
      if not entries then return end
      table.sort(entries)
      for _, name in ipairs(entries) do
        if name:sub(1, 1) ~= "." then
          load_path(path .. "/" .. name)
        end
      end
    elseif attr.mode == "file" and path:sub(-#MODULE_EXT) == MODULE_EXT then
      load_file(path)
    end
  end

  load_path(database_dir)
  return schemas
end

local schemas = load_schemas()
local maria_socket = env_or_config("maria_socket", "MARIA_SOCKET")
local pool_opts = {
  port = tonumber(env_or_config("maria_port", "MARIA_PORT")),
  user = env_or_config("maria_user", "MARIA_USERNAME") or "root",
  password = env_or_config("maria_passwd", "MARIA_PASSWORD") or "",
  database = env_or_config("maria_database", "MARIA_DATABASE_NAME"),
  charset = env_or_config("maria_charset", "MARIA_CHARSET") or "utf8mb4",
  max_size = tonumber(env_or_config("maria_pool_size", "MARIA_POOL_COUNT")) or 10,
  idle_ping = true,
}
if maria_socket then
  pool_opts.unix_socket = maria_socket
else
  pool_opts.host = env_or_config("maria_host", "MARIA_HOST") or "127.0.0.1"
end

local backend, pool_err = mariadb_pool.new(pool_opts)
if not backend then
  error("ctxpool: cannot create MariaDB pool: " .. tostring(pool_err), 0)
end

local facade = {
  pool = backend,
  schemas = schemas,
  map = setmetatable({}, { __mode = "k" }),
  index = 0,
}

function facade:pop()
  local db, err = self.pool:acquire()
  if not db then return nil, err end
  local ctx = self.map[db]
  if not ctx then
    local driver = orm.mariadb_driver(db)
    ctx = orm.new_context(driver)
    ctx.models = {}
    for name, schema in pairs(self.schemas) do
      local model, define_err = ctx:define(name, schema)
      if not model then
        self.pool:release(db, define_err)
        return nil, "ctxpool: define " .. name .. ": " .. tostring(define_err)
      end
      ctx.models[name] = model
      -- v1 callers commonly accessed models directly on the context;
      -- keep both ctx.models[name] and ctx[name] for compatibility.
      ctx[name] = model
    end
    self.index = self.index + 1
    ctx.index = self.index
    ctx._ctxpool_db = db
    self.map[db] = ctx
  end
  return ctx
end

function facade:push(ctx, err)
  if type(ctx) ~= "table" or not ctx._ctxpool_db then
    return nil, "ctxpool: invalid context"
  end
  self.pool:release(ctx._ctxpool_db, err)
  return true
end

function facade:safe(fn, ...)
  if type(fn) ~= "function" then return nil, "ctxpool: safe expects function" end
  local ctx, err = self:pop()
  if not ctx then return nil, err end
  local results = table.pack(xpcall(fn, debug.traceback, ctx, ...))
  local ok = results[1]
  self:push(ctx, ok and nil or results[2])
  if not ok then return nil, results[2] end
  table.remove(results, 1)
  return table.unpack(results, 1, results.n - 1)
end

function facade:close()
  return self.pool:close()
end

function facade:stats()
  return self.pool:stats()
end

return facade
