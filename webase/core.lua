-- webase/core.lua — main entry: init services, bind httpd, run event loop.
--
-- Usage (from webase directory):
--   WORKDIR=./ fan core.lua
-- Or with WORKDIR pointing at a project layout that ships handle/,
-- service/, mapping/, web/ folders plus a mime.types (webase looks at
-- WORKDIR + "mime.types" first, then a same-directory fallback).
--
-- Environment (see config.d/*.lua for canonical defaults):
--   SERVICE_HOST=0.0.0.0   bind interface
--   SERVICE_PORT=2201      bind port
--   WEBROOT=/root/web      static file root
--   SERVICE_WORKERS=0      reserved for future multi-worker mode; v2 has
--                          no drop-in equivalent of v1's fan.workers_init,
--                          so a >0 value currently prints a warning and
--                          continues single-threaded (the CPU-bound v2
--                          plan is fan.worker, added per-app).

print("core.lua")

_S = {conn_count = 0}
connmap = {}

local _S      = _S
local connmap = connmap

local fan     = require "fan"
local config  = require "config"

local httpd   = require "fan.httpd"

local route   = require "route"
local utils   = require "fan.utils"
local mapping = require "mapping"

local webfile = require "webfile"

local service = require "service"
print(service.start())

math.randomseed(utils.gettime())

-- v1 initialised worker threads here before binding HTTP so the worker
-- pool exists for accept-time dispatch. v2 uses per-process state and
-- has no equivalent single-VM worker pool; a caller who still sets
-- SERVICE_WORKERS>0 will see a one-line note and stay single-threaded.
local worker_count = tonumber(os.getenv("SERVICE_WORKERS")) or 0
if worker_count > 0 then
  if fan.workers_init then
    fan.workers_init(worker_count)
    print("workers: " .. tostring(fan.worker_count and fan.worker_count()))
  else
    print(string.format(
      "webase: SERVICE_WORKERS=%d requested but fan.workers_init is not " ..
      "available on this luafan2 build; continuing single-threaded. " ..
      "Use fan.worker + posix.fork in the service scripts if you need " ..
      "process-level fan-out.", worker_count))
  end
end

function onService(req, resp)
  req.path = mapping[req.path] or req.path

  if not route.web(req, resp) then
    return webfile.web(req, resp)
  end
end

-- v1 webase drove libevent's evhttp directly; luafan2's fan.httpd has
-- two backends and picks the C one (evhttp) by default. We force the
-- Lua backend here because:
--   1. webfile stores raw 16-byte MD5 digests on ETag. evhttp's
--      evhttp_add_header rejects non-token bytes (0x80+), so the C
--      backend 500s on every static file with an ETag.
--   2. The Lua backend surfaces `req.remote_addr` / `req.remoteip`
--      (webase M12.2 patch); the C backend has its own shape.
--   3. WebSocket + chunked reply are both feature-complete on the Lua
--      backend, which is what webase needs.
local serv2, bind_err = httpd.bind{
  host = config.service_host,
  port = config.service_port,
  onService = onService,
  backend = "lua",
}

if not serv2 then
  -- httpd.bind returns (nil, err) on failure. Raise a human-readable
  -- error so embedders (LuaBridge on the native host, Docker in the
  -- CI image) surface it instead of "attempt to index a nil value".
  error(string.format(
    "httpd.bind failed on %s:%s: %s",
    tostring(config.service_host), tostring(config.service_port),
    tostring(bind_err or "unknown")), 0)
end

if serv2.host and serv2.port then
  print(serv2.host, serv2.port)
else
  print(tostring(config.service_host), tostring(config.service_port))
end

fan.loop()

-- After event loop exits (triggered by fan.loopbreak / SIGTERM in an
-- embedder), run shutdown hooks so services can release DB handles and
-- external resources before lua_close. Kept as a global so service
-- modules can append to it: `table.insert(_SHUTDOWN_HOOKS, cleanup_fn)`.
if _SHUTDOWN_HOOKS then
  for _, fn in ipairs(_SHUTDOWN_HOOKS) do
    pcall(fn)
  end
end
