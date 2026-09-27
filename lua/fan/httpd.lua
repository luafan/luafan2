--[[
  fan/httpd.lua — LuaFan v2 HTTP/1.1 server dispatch shim.

  Public entry point for `require "fan.httpd"`. Picks between two backends
  per bind call:

    - "c"   -> fan.httpd_c (M14.C, evhttp-based; this file's default)
    - "lua" -> fan.httpd_lua (M4 pure-Lua backend on fan.tcp.bind)

  Selection rules (evaluated in order):
    1. If opts.backend == "lua" or "c" -> use that backend explicitly.
    2. Else if _G.__FAN_HTTPD_BACKEND_DEFAULT is set -> honour it. Tests
       flip this to "lua" when they exercise features not yet on the C
       backend (WebSocket upgrade in M14.C-d). It will disappear once
       every M14.C-* milestone lands.
    3. Else if fan.httpd_c is unavailable (older build) -> "lua".
    4. Else -> "c".

  Since M14.C-c the C backend serves HTTPS natively (via evhttp's bevcb
  hook + fan_tls_server_bev). opts.ssl / opts.cert / opts.key are
  forwarded transparently to httpd_c.bind.

  The Lua backend implements the full v1 fan.httpd contract including
  chunked reply, addheader, WebSocket. The C backend for M14.C-a only
  implements: bind, request:method/path/query/headers/body/available/read,
  response:reply(status, headers, body). Chunked / addheader / HTTPS /
  WebSocket come in later M14.C milestones.

  Both backends expose the same top-level function:
      httpd.bind{ host, port, handler = fn, ssl = ..., onService = ... }
  The shim normalises `onService` -> `handler` before delegating, matching
  v1 fan.httpd where `onService` was the canonical option name.

  On the C backend the user handler is called as fn(req) (v1 single-arg
  shape); the shim wraps it to also pass `req` as the second argument so
  the same handler signature works on both backends without change:
      handler(req, resp) where resp is the same userdata as req.
]]

local fan = require "fan"

local httpd_lua = require "fan.httpd_lua"
local httpd_c   = fan.httpd_c   -- may be nil on very old builds; guarded below

local M = {}

-- Choose the backend for a single bind call. Returns "c" or "lua".
local function pick_backend(opts)
  if opts.backend == "lua" or opts.backend == "c" then
    return opts.backend
  end
  local dflt = rawget(_G, "__FAN_HTTPD_BACKEND_DEFAULT")
  if dflt == "lua" or dflt == "c" then
    return dflt
  end
  if not httpd_c then
    -- C backend not compiled in (e.g. someone built with an older luafan).
    return "lua"
  end
  return "c"
end

-- Wrap a v1-style two-argument handler for the C backend. The C backend
-- passes a single req userdata (with response methods mounted on it). To
-- preserve the v1 request surface — including WebSocket helpers like
-- req:websocket_send / :receive / :ping / :pong / :close / :state that
-- expect access to a Lua-side ws object — we hand the user handler a
-- thin proxy table:
--
--   proxy.__index falls through to the C req userdata (methods + fields),
--   proxy provides Lua-only extras (websocket_* delegates + _ws stash).
--
-- The proxy is passed as BOTH arguments so `handler(req, resp)` still
-- observes v1 shape and same object identity.
local ws_proxy_mt = {}
ws_proxy_mt.__index = function(t, k)
  -- 1. Lua-side entries first (e.g. _ws stash, websocket_send delegates)
  local m = rawget(ws_proxy_mt, "__methods")[k]
  if m ~= nil then return m end
  -- 2. Field / method on the underlying C req userdata
  local u = rawget(t, "__req")
  if u ~= nil then
    -- Method access (`req:foo(...)`) — return the callable that binds
    -- the C userdata as self.
    local mm = u[k]
    if type(mm) == "function" then
      return function(_, ...) return mm(u, ...) end
    end
    return mm
  end
  return nil
end

local ws_proxy_methods = {}
ws_proxy_mt.__methods = ws_proxy_methods

-- req:websocket_accept() — delegate to the C userdata's :websocket_accept,
-- then stash the returned ws on the proxy for later req:websocket_* calls.
function ws_proxy_methods:websocket_accept()
  if rawget(self, "_ws") then return rawget(self, "_ws") end
  local u = rawget(self, "__req")
  local ws, err = u:websocket_accept()
  if not ws then return nil, err end
  rawset(self, "_ws", ws)
  return ws
end

local function _need_ws(proxy, name)
  local ws = rawget(proxy, "_ws")
  if not ws then
    return nil, name .. ": websocket handshake not accepted"
  end
  return ws
end

function ws_proxy_methods:websocket_send(msg, opcode)
  local ws, e = _need_ws(self, "websocket_send"); if not ws then return nil, e end
  return ws:send(msg, opcode)
end
function ws_proxy_methods:websocket_receive()
  local ws, e = _need_ws(self, "websocket_receive"); if not ws then return nil, e end
  return ws:recv()
end
function ws_proxy_methods:websocket_ping(data)
  local ws, e = _need_ws(self, "websocket_ping"); if not ws then return nil, e end
  return ws:ping(data)
end
function ws_proxy_methods:websocket_pong(data)
  local ws, e = _need_ws(self, "websocket_pong"); if not ws then return nil, e end
  return ws:pong(data)
end
function ws_proxy_methods:websocket_close(code, reason)
  local ws, e = _need_ws(self, "websocket_close"); if not ws then return nil, e end
  return ws:close(code, reason)
end
function ws_proxy_methods:websocket_state()
  local ws = rawget(self, "_ws")
  if not ws then return "connecting" end
  return ws:state()
end

local function wrap_c_handler(user_handler)
  return function(req)
    local proxy = setmetatable({ __req = req }, ws_proxy_mt)
    return user_handler(proxy, proxy)
  end
end

function M.bind(opts)
  assert(type(opts) == "table", "httpd.bind requires an options table")
  local handler = opts.handler or opts.onService
  assert(type(handler) == "function",
         "httpd.bind requires opts.handler or opts.onService (a function)")

  local backend = pick_backend(opts)

  if backend == "lua" then
    -- Direct passthrough — httpd_lua already accepts `onService` alias
    -- and `ssl=true` with cert/key.
    return httpd_lua.bind(opts)
  end

  -- C backend path.
  local c_opts = {
    host    = opts.host or "127.0.0.1",
    port    = assert(tonumber(opts.port), "httpd.bind requires numeric opts.port"),
    handler = wrap_c_handler(handler),
    -- M14.C-c: forward TLS options. httpd_c.bind requires cert+key when
    -- ssl or either path is set; validation happens C-side.
    ssl     = opts.ssl,
    cert    = opts.cert,
    key     = opts.key,
    -- M14.C-j: forward the /metrics scrape path (opt-in). C side rejects
    -- anything that doesn't start with '/'.
    metrics = opts.metrics,
  }
  return httpd_c.bind(c_opts)
end

-- Re-export helpers from the pure-Lua backend. These are pure functions
-- (parse_query / urldecode) with no runtime state, so it's safe to share
-- them regardless of which backend serves a bind call.
M.parse_query = httpd_lua.parse_query
M.urldecode   = httpd_lua.urldecode

-- Expose backend selector for tests + introspection.
M._pick_backend = pick_backend

-- M14.C-j: fan.httpd.metrics() forwards to httpd_c.metrics when the C
-- backend is available (which owns the process-global counters).
-- Returns a flat table {requests_total=..., bytes_sent=..., ...}.
-- Callable at any time (before/after bind, without a bind at all).
if httpd_c and httpd_c.metrics then
  M.metrics = httpd_c.metrics
else
  -- No C backend: return an empty snapshot so callers don't crash on
  -- older builds. Structure matches httpd_c.metrics shape.
  M.metrics = function()
    return {
      uptime_seconds = 0,
      requests_total = 0, requests_active = 0,
      bytes_sent = 0, bytes_received = 0, errors_total = 0,
      connections_total = 0, keepalive_reused = 0,
      requests_get = 0, requests_post = 0, requests_put = 0,
      requests_delete = 0, requests_other = 0,
      responses_2xx = 0, responses_3xx = 0,
      responses_4xx = 0, responses_5xx = 0,
    }
  end
end

return M
