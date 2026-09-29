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

----------------------------------------------------------------------
-- Active-row support (M16.2)
--
-- Rows returned by Model.insert / find_by / list are "live" objects: they
-- carry a metatable with :update() / :delete() / :remove() methods that
-- talk back to the database.
--
--   local u = User.find_by{ name = "alice" }
--   u.email = "new@x"
--   u:update()             -- auto-diff: only UPDATEs the columns that changed
--   u:update{ age = 30 }   -- explicit override: UPDATEs the given columns
--   u:delete()             -- DELETE WHERE pk = self[pk_field]
--   u:remove()             -- alias of :delete()
--
-- Baseline storage:
--   Each row's "originally-loaded values" snapshot lives in a weak-key map
--   attached to the row's metatable (row_mt.__attr_map[row] = attr_table).
--   Using a weak-key table lets attr be GC'd automatically when the row is
--   collected — we don't need explicit cleanup.  Using a table stored in
--   the metatable (rather than a hidden field inside the row itself) keeps
--   the row's own key namespace clean: `for k, v in pairs(row) do end`
--   sees exactly the SQL columns, nothing else.
--
-- Escape hatch:
--   Model.list{ raw = true }  returns plain maps (no metatable) for the
--   times you want cheap read-only bulk access.  Model.raw_query is also
--   always plain (semantics unknown, no pk to reason about).
----------------------------------------------------------------------

-- Build the row metatable for one specific Model.  Closes over driver,
-- table_name, pk_field, and schema so :update() / :delete() know what
-- SQL to emit.  Called once per ctx:define(); attach_row reuses it for
-- every row that model returns.
local function make_row_mt(driver, table_name, pk_field, schema)
  local qn  = driver:quote_ident(table_name)
  local qpk = driver:quote_ident(pk_field)

  local row_mt = {}
  -- Weak-key map: attr_map[row] = { column = original_value, ... }
  row_mt.__attr_map = setmetatable({}, { __mode = "k" })

  local function do_update_diff(self, attr)
    -- Emit UPDATE for columns whose current row value differs from the
    -- snapshot.  If nothing changed, return 0 (rows_affected style).
    local sets, args, i = {}, {}, 0
    for k, _ in pairs(schema) do
      if k ~= "pk" and self[k] ~= attr[k] then
        i = i + 1
        sets[#sets + 1] = driver:quote_ident(k) .. " = " .. driver:placeholder(i)
        args[#args + 1] = self[k]
      end
    end
    if i == 0 then return 0 end
    i = i + 1
    -- Use the snapshot's pk (not self's) so a diff that RENAMES the pk
    -- column still WHEREs against the on-disk row's identity.
    args[#args + 1] = attr[pk_field] ~= nil and attr[pk_field] or self[pk_field]
    local sql = "UPDATE " .. qn .. " SET " .. table.concat(sets, ",")
             .. " WHERE " .. qpk .. " = " .. driver:placeholder(i)
    local n, err = driver:exec_with_args(sql, args)
    if not n then return nil, err end
    -- Refresh the snapshot from the row's current state.  This makes
    -- successive u:update() calls diff against the last-flushed state,
    -- not the original SELECT.
    for k, _ in pairs(schema) do
      if k ~= "pk" then attr[k] = self[k] end
    end
    return n
  end

  local function do_update_override(self, attr, override)
    -- Explicit-columns mode: caller specified exactly which columns to
    -- UPDATE (and their new values).  Apply them to self AND the snapshot.
    -- Snapshot the pk BEFORE mutating self, so if the caller included the
    -- pk column in `override` (renaming a row), WHERE still targets the
    -- old row.
    local old_pk = self[pk_field]
    local sets, args, i = {}, {}, 0
    for k, v in pairs(override) do
      i = i + 1
      sets[#sets + 1] = driver:quote_ident(k) .. " = " .. driver:placeholder(i)
      args[#args + 1] = v
      self[k] = v
      if attr then attr[k] = v end
    end
    if i == 0 then return 0 end
    i = i + 1
    args[#args + 1] = old_pk
    local sql = "UPDATE " .. qn .. " SET " .. table.concat(sets, ",")
             .. " WHERE " .. qpk .. " = " .. driver:placeholder(i)
    return driver:exec_with_args(sql, args)
  end

  row_mt.__index = {
    update = function(self, override)
      local attr = row_mt.__attr_map[self]
      if override then
        return do_update_override(self, attr, override)
      end
      if not attr then
        -- Row was constructed without a baseline (e.g. detached copy).
        -- Fall back to "UPDATE every schema-known column with self's
        -- current value" so the call is not silently a no-op.  Rare —
        -- attach_row always installs a baseline.
        local synthetic = {}
        for k, _ in pairs(schema) do
          if k ~= "pk" and k ~= pk_field then synthetic[k] = self[k] end
        end
        return do_update_override(self, nil, synthetic)
      end
      return do_update_diff(self, attr)
    end,

    delete = function(self)
      local sql = "DELETE FROM " .. qn .. " WHERE " .. qpk
               .. " = " .. driver:placeholder(1)
      local n, err = driver:exec_with_args(sql, { self[pk_field] })
      if not n then return nil, err end
      -- Detach: further :update() / :delete() calls become plain-table
      -- errors ("attempt to call a nil value") rather than silently
      -- issuing SQL against a row that no longer exists.
      row_mt.__attr_map[self] = nil
      setmetatable(self, nil)
      return n
    end,
  }
  -- :remove is a v1-era alias for :delete
  row_mt.__index.remove = row_mt.__index.delete

  return row_mt
end

-- Attach the given row_mt to a row and record its baseline snapshot.
-- `pk_field` is passed explicitly so we can guarantee attr[pk_field] is
-- captured even in the rare case where the schema table's `pk` entry
-- points to a column name that has no corresponding schema entry (i.e.
-- the pk column type was not declared, only referenced via schema.pk).
local function attach_row(row, row_mt, schema, pk_field)
  local attr = {}
  for k, _ in pairs(schema) do
    if k ~= "pk" then attr[k] = row[k] end
  end
  -- Belt-and-suspenders: attr must contain the pk value for :update()'s
  -- WHERE clause to work if the caller ever renames the pk column.
  attr[pk_field] = row[pk_field]
  row_mt.__attr_map[row] = attr
  setmetatable(row, row_mt)
  return row
end

local function make_model(ctx, name, schema)
  local m = { _ctx = ctx, _name = name, _schema = schema }
  local driver = ctx.driver
  local qn = driver:quote_ident(name)
  local pk = schema.pk or "id"

  -- One row_mt per model (methods close over driver / table_name / pk / schema).
  local row_mt = make_row_mt(driver, name, pk, schema)
  m._row_mt = row_mt   -- exposed for tests / advanced users; treat as private

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

    -- M16.2: return the inserted row as an active-record object.  The row's
    -- column values come from the caller's `fields` plus the autoincrement
    -- pk pulled from the driver.  This is the v1-era shape: callers write
    --   local u = User.insert{ name = "alice" }
    --   u.email = "a@x"; u:update()
    -- Legacy callers who just want the id can still use  local u =
    -- User.insert{...}; local id = u.id  which is a one-token change.
    local row = {}
    for k, v in pairs(fields) do row[k] = v end
    if row[pk] == nil then
      row[pk] = driver:last_insert_rowid()
    end
    return attach_row(row, row_mt, schema, pk)
  end

  function m.find_by(where)
    local w, args = build_where(driver, where)
    local sql = "SELECT * FROM " .. qn .. w .. " LIMIT 1"
    local rows, err = driver:query(sql, table.unpack(args))
    if not rows then return nil, err end
    if rows[1] then attach_row(rows[1], row_mt, schema, pk) end
    return rows[1]
  end

  function m.list(opts)
    opts = opts or {}
    local w, args = build_where(driver, opts.where)
    local order = opts.order and (" ORDER BY " .. opts.order) or ""
    local limit = opts.limit and (" LIMIT " .. tonumber(opts.limit)) or ""
    local sql = "SELECT * FROM " .. qn .. w .. order .. limit
    local rows, err = driver:query(sql, table.unpack(args))
    if not rows then return nil, err end
    if not opts.raw then
      for _, row in ipairs(rows) do attach_row(row, row_mt, schema, pk) end
    end
    return rows
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
             .. " WHERE " .. driver:quote_ident(pk)
             .. " = " .. driver:placeholder(i)
    return driver:exec_with_args(sql, args)
  end

  function m.delete(id)
    local sql = "DELETE FROM " .. qn .. " WHERE "
              .. driver:quote_ident(pk)
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
