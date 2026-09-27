--[[
  fan/http.lua — LuaFan v2 HTTP/1.1 client dispatch shim.

  Public entry point for `require "fan.http"`. Picks between two backends
  per request:

    - "c"   -> fan.http_c (M13.C, libcurl multi + libevent; this file's default)
    - "lua" -> fan.http_lua (M4 pure-Lua backend on fan.tcp.bind)

  Selection rules (evaluated in order):
    1. If opts.backend == "lua" or "c" -> use that backend explicitly.
    2. Else if _G.__FAN_HTTP_BACKEND_DEFAULT is set -> honour it. Tests
       flip this to "lua" when they exercise features not yet on the C
       backend. It will disappear once every M13.C-* milestone lands.
    3. Else if fan.http_c is nil or fan.http_c.available == false ->
       fall back to "lua" (C backend not compiled in / disabled).
    4. Else -> "c".

  Both backends expose the same top-level surface:

      local http = require "fan.http"
      local resp = http.request{ url = ..., method = ..., headers = ...,
                                 body = ..., query = ..., timeout = ...,
                                 verify = ..., follow_redirects = ...,
                                 max_redirects = ... }
      -- resp = { status, reason, headers (lowercased), body }
      -- on error: nil, errmsg

      http.get(url [, opts])    -- and post/put/delete/head/patch/update
      http.escape(s)  / http.unescape(s)
      http.cookiejar(path) / http.cainfo(path) / http.capath(path)
      http.parse_url(url) / http.urlencode(s)

  Redirect following (301/302/303/307/308) is handled here in the shim
  regardless of backend, so C-backend requests share the same v1
  redirect semantics as the Lua backend (303 -> GET; POST 301/302 -> GET
  with body dropped).

  cookiejar / cainfo / capath act as module-scoped defaults: the shim
  reads M._cookiejar / M._cainfo / M._capath at request time and
  forwards them to whichever backend is chosen (only the C backend
  actually consumes them today).
]]

local fan = require "fan"

local http_lua = require "fan.http_lua"
local http_c   = fan.http_c   -- may be nil on older builds / non-libcurl builds

local M = {}

-- ---------------------------------------------------------------------------
-- Backend selection
-- ---------------------------------------------------------------------------
local function c_backend_available()
  return http_c ~= nil and http_c.available ~= false and http_c.request ~= nil
end

local function pick_backend(opts)
  local b = opts and opts.backend
  if b == "lua" or b == "c" then
    return b
  end
  local dflt = rawget(_G, "__FAN_HTTP_BACKEND_DEFAULT")
  if dflt == "lua" or dflt == "c" then
    return dflt
  end
  if not c_backend_available() then return "lua" end
  return "c"
end

-- ---------------------------------------------------------------------------
-- URL / query helpers (re-exported from the Lua backend so they're always
-- available even when the C backend is loaded; both are pure functions).
-- ---------------------------------------------------------------------------
local parse_url = http_lua.parse_url
local urlencode = http_lua.urlencode

local function build_query(t)
  local parts = {}
  for k, v in pairs(t) do
    parts[#parts + 1] = urlencode(k) .. "=" .. urlencode(v)
  end
  return table.concat(parts, "&")
end

-- Merge opts.query into opts.url (idempotent; if url already has "?" we
-- append with "&"). Returns the merged URL string.
local function merge_query(url, query)
  if not query or not next(query) then return url end
  local sep = url:find("?", 1, true) and "&" or "?"
  return url .. sep .. build_query(query)
end

-- ---------------------------------------------------------------------------
-- Redirect helpers
-- ---------------------------------------------------------------------------
local function is_redirect(code)
  return code == 301 or code == 302 or code == 303
      or code == 307 or code == 308
end

-- Resolve a Location header value against a base URL. Absolute URLs pass
-- through; scheme-relative (//host/...) is not supported (v1 didn't either);
-- root-relative (/path) and path-relative both re-authority against `base`.
local function resolve_location(base, loc)
  if loc:match("^%w+://") then return loc end
  local scheme, host, port = parse_url(base)
  if not scheme then return loc end  -- best-effort; will fail at the next hop
  local authority = host
      .. ((port ~= 80 and port ~= 443) and (":" .. port) or "")
  if loc:sub(1, 1) ~= "/" then loc = "/" .. loc end
  return scheme .. "://" .. authority .. loc
end

-- ---------------------------------------------------------------------------
-- Single-hop request against the C backend (no redirect handling).
-- Assembles the opts table the C module expects, threading module-scoped
-- cookiejar / cainfo / capath defaults through unless the caller already
-- specified them.
-- ---------------------------------------------------------------------------
local function c_do_once(opts)
  local c_opts = {
    url     = opts.url,
    method  = (opts.method or "GET"):upper(),
    headers = opts.headers,
    body    = opts.body,
    timeout = opts.timeout,
  }
  -- verify: allow either coarse `verify` (bool) or fine-grained
  -- verify_peer / verify_host to reach the C backend as-is.
  if opts.verify ~= nil       then c_opts.verify = opts.verify end
  if opts.verify_peer ~= nil  then c_opts.verify_peer = opts.verify_peer end
  if opts.verify_host ~= nil  then c_opts.verify_host = opts.verify_host end
  -- Module-scoped defaults for cookiejar / cainfo / capath — allow the
  -- per-call opts to override.
  c_opts.cookiejar = opts.cookiejar or M._cookiejar
  c_opts.cainfo    = opts.cainfo    or M._cainfo
  c_opts.capath    = opts.capath    or M._capath
  return http_c.request(c_opts)
end

-- ---------------------------------------------------------------------------
-- Public request. Same shape on either backend; redirects handled here.
-- ---------------------------------------------------------------------------
function M.request(opts)
  assert(type(opts) == "table" and opts.url, "http.request requires {url=...}")
  local follow = opts.follow_redirects
  if follow == nil then follow = true end
  local max_redirects = opts.max_redirects or 5

  local backend = pick_backend(opts)

  -- Lua backend already implements redirects internally; hand off with
  -- query merged so the semantics match either path.
  if backend == "lua" then
    local o = {}
    for k, v in pairs(opts) do o[k] = v end
    o.url = merge_query(opts.url, opts.query)
    o.query = nil                     -- already folded in
    -- Thread module-scoped defaults so downstream code that inspects
    -- them (or a future Lua backend that honours them) sees the same
    -- values as the C path.
    if o.cookiejar == nil then o.cookiejar = M._cookiejar end
    if o.cainfo    == nil then o.cainfo    = M._cainfo end
    if o.capath    == nil then o.capath    = M._capath end
    return http_lua.request(o)
  end

  -- C backend: single-hop then redirect loop up here in the shim.
  local url = merge_query(opts.url, opts.query)
  local method = opts.method
  local body   = opts.body
  local hops = 0
  while true do
    local o = {}
    for k, v in pairs(opts) do o[k] = v end
    o.url    = url
    o.method = method or opts.method
    o.body   = body
    o.query  = nil                    -- already folded into url
    local resp, err = c_do_once(o)
    if not resp then return nil, err end
    if follow and is_redirect(resp.status) and resp.headers["location"] then
      hops = hops + 1
      if hops > max_redirects then return nil, "too many redirects" end
      url = resolve_location(url, resp.headers["location"])
      -- 303 (and 301/302 with POST) switch to GET and drop body — same
      -- rules as the pure-Lua backend and v1.
      local cur_method = (method or opts.method or "GET"):upper()
      if resp.status == 303
          or ((resp.status == 301 or resp.status == 302) and cur_method == "POST") then
        method = "GET"
        body = nil
      end
    else
      return resp
    end
  end
end

-- ---------------------------------------------------------------------------
-- Convenience verbs. All route through M.request so the backend/redirect
-- logic stays in one place.
-- ---------------------------------------------------------------------------
local function verb(m)
  return function(url, o)
    o = o or {}
    o.url = url
    o.method = m
    return M.request(o)
  end
end
M.get    = verb("GET")
M.post   = verb("POST")
M.put    = verb("PUT")
M.delete = verb("DELETE")
M.head   = verb("HEAD")
M.patch  = verb("PATCH")
M.update = verb("UPDATE")

-- ---------------------------------------------------------------------------
-- URL parsing / escape helpers. Prefer the C backend's escape/unescape
-- (libcurl's curl_easy_escape / curl_easy_unescape) when available for
-- byte-for-byte parity with the v1 C module; fall back to the pure-Lua
-- implementation otherwise. Both implement RFC 3986 %xx encoding over
-- the unreserved set [A-Za-z0-9_.~-] (NOT form-urlencoded "+"-for-space).
-- ---------------------------------------------------------------------------
M.parse_url = parse_url
M.urlencode = urlencode

if c_backend_available() and http_c.escape then
  M.escape = http_c.escape
else
  M.escape = http_lua.escape
end

if c_backend_available() and http_c.unescape then
  M.unescape = http_c.unescape
else
  M.unescape = http_lua.unescape
end

-- ---------------------------------------------------------------------------
-- v1 fan.http compatibility surface: cookiejar / cainfo / capath setters.
-- Stored on M._cookiejar / M._cainfo / M._capath so the shim can forward
-- them to whichever backend runs the next request (the C backend actually
-- honours them; the Lua backend keeps the values but does not consume
-- them yet).
-- ---------------------------------------------------------------------------
function M.cookiejar(path) M._cookiejar = tostring(path) end
function M.cainfo(path)    M._cainfo    = tostring(path) end
function M.capath(path)    M._capath    = tostring(path) end

-- Expose backend selector for tests / introspection.
M._pick_backend = pick_backend

return M
