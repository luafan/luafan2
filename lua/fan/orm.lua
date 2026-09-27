--[[
  fan/orm.lua — LuaFan v2 ORM base.

  Design:
    - Driver-agnostic: takes a "driver" table with methods
        driver:exec(sql)           -> rows_affected or nil,err
        driver:query(sql, ...)     -> array of row-maps or nil,err
        driver:last_insert_rowid() -> integer (SQLite-specific placeholder)
        driver:begin/commit/rollback
        driver:quote_ident(name)   -> string (backticked / double-quoted)
        driver:placeholder(i)      -> '?' | '$1' etc
        driver:default_pk_type()   -> "INTEGER PRIMARY KEY AUTOINCREMENT" or
                                       equivalent
    - fan.orm.new_context(driver) returns a context object with:
        ctx:define(table_name, schema)   -> Model
        Model.insert{col=val, ...}       -> id, or nil,err
        Model.find_by{col=val, ...}      -> row-map or nil (not found)
        Model.list{where={col=val},limit=N,order=...} -> array
        Model.update(id, {col=val,...})  -> rows_affected or nil,err
        Model.delete(id)                 -> rows_affected or nil,err
        Model.raw_query(sql, ...)        -> passthrough
        Model.migrate()                  -> add missing columns (SQLite: ADD COLUMN)

  Schema definition (table -> {field_name = type_string, pk = "id"}):
    {
      id    = "integer primary key autoincrement",
      name  = "text not null",
      email = "text",
      created_at = "integer",
    }
]]

local M = {}

----------------------------------------------------------------------
-- SQLite driver adapter
----------------------------------------------------------------------
local sqlite_driver = {}
sqlite_driver.__index = sqlite_driver

function M.sqlite_driver(db)
  return setmetatable({db = db}, sqlite_driver)
end

function sqlite_driver:exec(sql)  return self.db:exec(sql) end
function sqlite_driver:query(sql, ...)
  return self.db:query(sql, ...)
end
function sqlite_driver:last_insert_rowid() return self.db:last_insert_rowid() end
function sqlite_driver:begin()    return self.db:begin() end
function sqlite_driver:commit()   return self.db:commit() end
function sqlite_driver:rollback() return self.db:rollback() end
function sqlite_driver:quote_ident(name) return '"' .. name:gsub('"', '""') .. '"' end
function sqlite_driver:placeholder(i)    return "?" end
function sqlite_driver:default_pk_type()
  return "INTEGER PRIMARY KEY AUTOINCREMENT"
end
-- introspection: table_info(name) returns { {name=..., type=..., notnull=...}, ... }
function sqlite_driver:table_info(tbl)
  local rows, err = self:query("PRAGMA table_info(" .. self:quote_ident(tbl) .. ")")
  if not rows then return nil, err end
  return rows
end

----------------------------------------------------------------------
-- MariaDB driver adapter
----------------------------------------------------------------------
local mariadb_driver = {}
mariadb_driver.__index = mariadb_driver

function M.mariadb_driver(db)
  return setmetatable({db = db}, mariadb_driver)
end

function mariadb_driver:exec(sql)  return self.db:exec(sql) end
function mariadb_driver:query(sql, ...)
  return self.db:query(sql, ...)
end
function mariadb_driver:last_insert_rowid() return self.db:last_insert_id() end
function mariadb_driver:begin()    return self.db:begin() end
function mariadb_driver:commit()   return self.db:commit() end
function mariadb_driver:rollback() return self.db:rollback() end
function mariadb_driver:quote_ident(name) return "`" .. name:gsub("`", "``") .. "`" end
function mariadb_driver:placeholder(i)    return "?" end
function mariadb_driver:default_pk_type()
  return "BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT"
end
-- introspection: use INFORMATION_SCHEMA.COLUMNS or SHOW COLUMNS.
function mariadb_driver:table_info(tbl)
  local rows, err = self:query("SHOW COLUMNS FROM " .. self:quote_ident(tbl))
  if not rows then return nil, err end
  -- normalise column names to match sqlite pragma keys (name/type)
  local out = {}
  for _, r in ipairs(rows) do
    out[#out + 1] = { name = r.Field, type = r.Type }
  end
  return out
end
-- MariaDB: use query() to substitute ? placeholders via escaping (the C
-- binding accepts positional args on both query and exec paths).
function mariadb_driver:exec_with_args(sql, args)
  local rows, err = self.db:query(sql, table.unpack(args))
  if not rows then return nil, err end
  return self.db:affected_rows()
end

----------------------------------------------------------------------
-- Context / Model
----------------------------------------------------------------------
local Model = {}

local function build_where(driver, where)
  if not where or next(where) == nil then return "", {} end
  local parts, args = {}, {}
  local i = 0
  for k, v in pairs(where) do
    i = i + 1
    parts[#parts + 1] = driver:quote_ident(k) .. " = " .. driver:placeholder(i)
    args[#args + 1]   = v
  end
  return " WHERE " .. table.concat(parts, " AND "), args
end

local function make_model(ctx, name, schema)
  local m = { _ctx = ctx, _name = name, _schema = schema }
  local driver = ctx.driver
  local qn = driver:quote_ident(name)

  function m.insert(fields)
    local cols, ph, args = {}, {}, {}
    local i = 0
    for k, v in pairs(fields) do
      i = i + 1
      cols[#cols + 1] = driver:quote_ident(k)
      ph[#ph + 1]     = driver:placeholder(i)
      args[#args + 1] = v
    end
    if #cols == 0 then
      return nil, "orm.insert: no fields provided"
    end
    local sql = "INSERT INTO " .. qn .. " (" .. table.concat(cols, ",")
             .. ") VALUES (" .. table.concat(ph, ",") .. ")"
    local ok, err = driver:exec_with_args(sql, args)
    if not ok then return nil, err end
    return driver:last_insert_rowid()
  end

  function m.find_by(where)
    local w, args = build_where(driver, where)
    local sql = "SELECT * FROM " .. qn .. w .. " LIMIT 1"
    local rows, err = driver:query(sql, table.unpack(args))
    if not rows then return nil, err end
    return rows[1]
  end

  function m.list(opts)
    opts = opts or {}
    local w, args = build_where(driver, opts.where)
    local order = opts.order and (" ORDER BY " .. opts.order) or ""
    local limit = opts.limit and (" LIMIT " .. tonumber(opts.limit)) or ""
    local sql = "SELECT * FROM " .. qn .. w .. order .. limit
    return driver:query(sql, table.unpack(args))
  end

  function m.update(id, fields)
    local sets, args = {}, {}
    local i = 0
    for k, v in pairs(fields) do
      i = i + 1
      sets[#sets + 1] = driver:quote_ident(k) .. " = " .. driver:placeholder(i)
      args[#args + 1] = v
    end
    if #sets == 0 then return 0 end
    i = i + 1
    args[#args + 1] = id
    local sql = "UPDATE " .. qn .. " SET " .. table.concat(sets, ",")
             .. " WHERE " .. driver:quote_ident(schema.pk or "id")
             .. " = " .. driver:placeholder(i)
    return driver:exec_with_args(sql, args)
  end

  function m.delete(id)
    local sql = "DELETE FROM " .. qn .. " WHERE "
              .. driver:quote_ident(schema.pk or "id")
              .. " = " .. driver:placeholder(1)
    return driver:exec_with_args(sql, {id})
  end

  function m.raw_query(sql, ...) return driver:query(sql, ...) end

  return m
end

----------------------------------------------------------------------
-- Driver-agnostic "exec with positional args" shim.
-- SQLite's fan.sqlite3 db:exec takes no params (uses sqlite3_exec), and
-- db:query takes varargs -> we use prepared statements via db:prepare.
----------------------------------------------------------------------
function sqlite_driver:exec_with_args(sql, args)
  local st, err = self.db:prepare(sql)
  if not st then return nil, err end
  if #args > 0 then
    local ok, be = st:bind_all(table.unpack(args))
    if not ok then st:finalize(); return nil, be end
  end
  local rc, se = st:step()
  if not rc then st:finalize(); return nil, se end
  local changes = self.db:changes()
  st:finalize()
  return changes
end

----------------------------------------------------------------------
-- Schema management (CREATE TABLE IF NOT EXISTS + ADD COLUMN migration)
----------------------------------------------------------------------
local function ensure_table(ctx, name, schema)
  local driver = ctx.driver
  local qn = driver:quote_ident(name)
  local cols = {}
  -- ensure pk comes first
  local pk = schema.pk or "id"
  if schema[pk] then
    cols[#cols + 1] = driver:quote_ident(pk) .. " " .. schema[pk]
  else
    cols[#cols + 1] = driver:quote_ident(pk) .. " " .. driver:default_pk_type()
  end
  for k, v in pairs(schema) do
    if k ~= "pk" and k ~= pk then
      cols[#cols + 1] = driver:quote_ident(k) .. " " .. v
    end
  end
  local sql = "CREATE TABLE IF NOT EXISTS " .. qn
            .. " (" .. table.concat(cols, ",") .. ")"
  local ok, err = driver:exec(sql)
  if not ok then return nil, err end

  -- migration: ADD COLUMN for anything in schema not yet present
  local info, ierr = driver:table_info(name)
  if not info then return nil, ierr end
  local present = {}
  for _, r in ipairs(info) do present[r.name] = true end
  for k, v in pairs(schema) do
    if k ~= "pk" and not present[k] then
      local addsql = "ALTER TABLE " .. qn .. " ADD COLUMN "
                   .. driver:quote_ident(k) .. " " .. v
      local ok2, err2 = driver:exec(addsql)
      if not ok2 then return nil, err2 end
    end
  end
  return true
end

----------------------------------------------------------------------
-- Public: new_context / define
----------------------------------------------------------------------
function M.new_context(driver)
  local ctx = { driver = driver, models = {} }

  function ctx:define(name, schema)
    local ok, err = ensure_table(self, name, schema)
    if not ok then return nil, err end
    local model = make_model(self, name, schema)
    self.models[name] = model
    return model
  end

  function ctx:transaction(fn)
    local ok, err = self.driver:begin()
    if not ok then return nil, err end
    local pok, perr = pcall(fn)
    if pok then
      local cok, cerr = self.driver:commit()
      if not cok then return nil, cerr end
      return true
    else
      self.driver:rollback()
      return nil, perr
    end
  end

  return ctx
end

return M
