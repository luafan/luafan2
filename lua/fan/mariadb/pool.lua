--[[
  fan/mariadb/pool.lua — LuaFan v2 MariaDB connection pool (M5.5.b, sync).

  A minimal connection pool that keeps up to `max_size` live connections
  and hands them out to callers. Because M5.5.b is still synchronous, the
  pool is a plain LIFO stack of idle MYSQL* handles + a live-count cap.
  M5.5.c will add cooperative "wait for slot" via fan.spawn / park when
  async wait lands; the API shape is stable for both phases.

  API:
    local pool = mariadb.pool.new{
      -- connection options passed straight through to mariadb.connect{}
      unix_socket = "...",
      host = ..., port = ..., user = ..., password = ...,
      database = ..., charset = ..., autocommit = ...,
      -- pool options
      max_size    = 8,     -- hard cap on live conns (default 8)
      idle_ping   = false, -- true: mysql_ping() acquired conns; drop dead ones
    }
    pool:with(function(db) ... end) -> value, err
      Acquire a conn, run the callback under pcall, release. On success
      returns the callback's return value(s). On error, the connection is
      still returned to the pool (unless the error was a MySQL server-gone
      error, in which case it's discarded).
    pool:acquire() -> db, err                (rare direct use)
    pool:release(db)                         (returns db to the pool)
    pool:close()                             (closes ALL live conns; R13
      guarantees synchronous close so no dangling handles outlive the pool)
    pool:stats() -> {live=N, idle=N, max=N}

  Errors are (nil, "mariadb.pool: ...").
]]
local mariadb = require("fan.mariadb")

local M = {}
local Pool = {}
Pool.__index = Pool

local CONN_KEYS = {
  "host","port","user","password","database","unix_socket","charset","autocommit",
}

function M.new(opts)
  if type(opts) ~= "table" then
    return nil, "mariadb.pool.new: opts must be a table"
  end
  local conn = {}
  for _, k in ipairs(CONN_KEYS) do conn[k] = opts[k] end
  local p = setmetatable({
    conn_opts = conn,
    max_size  = tonumber(opts.max_size) or 8,
    idle_ping = opts.idle_ping and true or false,
    idle      = {},   -- stack of idle db handles
    live      = 0,    -- count of live handles (idle + acquired)
    closed    = false,
  }, Pool)
  if p.max_size < 1 then
    return nil, "mariadb.pool.new: max_size must be >= 1"
  end
  return p
end

local function new_conn(p)
  local db, err = mariadb.connect(p.conn_opts)
  if not db then return nil, "mariadb.pool: connect: " .. tostring(err) end
  return db
end

function Pool:acquire()
  if self.closed then return nil, "mariadb.pool: closed" end
  while #self.idle > 0 do
    local db = table.remove(self.idle)
    if self.idle_ping then
      local ok = db:ping()
      if not ok then
        pcall(db.close, db)
        self.live = self.live - 1
      else
        return db
      end
    else
      return db
    end
  end
  if self.live >= self.max_size then
    return nil, "mariadb.pool: no idle conn and cap reached (max=" ..
      tostring(self.max_size) .. ")"
  end
  local db, err = new_conn(self)
  if not db then return nil, err end
  self.live = self.live + 1
  return db
end

-- Server-gone / connection-lost error markers we should NOT return to the
-- pool because the socket is unusable.
local FATAL_MARKERS = {
  "server has gone away",
  "Lost connection",
  "Broken pipe",
  "Connection reset",
}
local function looks_fatal(err)
  if type(err) ~= "string" then return false end
  for _, m in ipairs(FATAL_MARKERS) do
    if err:find(m, 1, true) then return true end
  end
  return false
end

function Pool:release(db, err)
  if self.closed then
    pcall(db.close, db); self.live = self.live - 1; return
  end
  if err and looks_fatal(err) then
    pcall(db.close, db); self.live = self.live - 1; return
  end
  self.idle[#self.idle + 1] = db
end

function Pool:with(fn)
  local db, err = self:acquire()
  if not db then return nil, err end
  local ok, r1, r2, r3 = pcall(fn, db)
  if ok then
    self:release(db)
    return r1, r2, r3
  end
  -- error path: r1 holds the error message
  self:release(db, tostring(r1))
  return nil, tostring(r1)
end

-- Synchronous close: R13 regression — every conn must be actually closed
-- before pool:close() returns; no reliance on Lua GC ordering.
function Pool:close()
  if self.closed then return end
  self.closed = true
  for i = #self.idle, 1, -1 do
    local db = self.idle[i]
    pcall(db.close, db)
    self.idle[i] = nil
  end
  self.live = 0
end

function Pool:stats()
  return { live = self.live, idle = #self.idle, max = self.max_size }
end

return M
