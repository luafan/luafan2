--[[
  fan/httpd_lua.lua — Pure-Lua HTTP/1.1 server backend for LuaFan v2.

  This is the original M4 implementation of fan.httpd, preserved verbatim
  as the fallback backend once the M14.C evhttp C backend lands. The public
  `fan.httpd` module is now a dispatch shim in `fan/httpd.lua` that picks
  between this file and the C backend per bind call.

  Design mirrors fan/http.lua (client): pure Lua, "thin C, thick Lua". Each
  accepted connection runs in its own fan coroutine (spawned by fan.tcp
  server_accept_cb), reads a single HTTP request, calls the user handler,
  and serialises the response.

  Contract (plan \u00a74.6):
    - request-line + header parsing (case-insensitive header lookup)
    - Content-Length request body
    - status codes / reason phrases; automatic Content-Length on reply
    - chunked reply via reply_start / reply_chunk / reply_end (fan.sleep
      may interleave between chunks and the connection stays live)
    - error handler contract: parse failure -> 400; handler error -> 500
    - one request per connection (Connection: close). Keep-alive is a
      later iteration of M4; the client side already sends Connection:
      close so this pairs correctly.

  API (also reachable via the dispatch shim with backend="lua"):
    local httpd = require "fan.httpd_lua"
    local server = httpd.bind{
      host = "127.0.0.1",    -- default 127.0.0.1
      port = 8080,           -- required
      ssl  = false,          -- true = HTTPS, requires cert + key
      cert = "server.pem",   -- PEM cert path (when ssl=true)
      key  = "server.key",   -- PEM key path  (when ssl=true)
      handler = function(req, resp)
        -- req  = { method, path, query, params, headers, body, http_version }
        --   req.headers is a lowercased-key table
        --   req.query  is the raw query string (may be empty)
        --   req.params  is the decoded query/form parameter map
        -- resp:reply(status, headers, body)                -- one-shot
        -- resp:reply_start(status, headers)                -- chunked mode
        -- resp:reply_chunk(data)                           -- 0 or more
        -- resp:reply_end()                                 -- terminate
      end,
    }
    -- server:close()
]]

local fan = require "fan"

local M = {}

----------------------------------------------------------------------
-- URL helpers
----------------------------------------------------------------------
local function urldecode(s)
  s = s:gsub("+", " ")
  return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

local function parse_query(qs)
  local t = {}
  if not qs or qs == "" then return t end
  for pair in qs:gmatch("[^&]+") do
    local k, v = pair:match("^([^=]*)=(.*)$")
    if k then
      t[urldecode(k)] = urldecode(v)
    else
      t[urldecode(pair)] = ""
    end
  end
  return t
end

local function split_path_query(target)
  local q = target:find("?", 1, true)
  if not q then return target, "" end
  return target:sub(1, q - 1), target:sub(q + 1)
end

----------------------------------------------------------------------
-- Buffered reader over a fan.tcp conn (same shape as http.lua's)
----------------------------------------------------------------------
local Reader = {}
Reader.__index = Reader

local function new_reader(conn)
  return setmetatable({ conn = conn, buf = "", pos = 1 }, Reader)
end

function Reader:_fill()
  local data, err = self.conn:receive()
  if not data then return nil, err or "eof" end
  if self.pos > 1 then
    self.buf = self.buf:sub(self.pos)
    self.pos = 1
  end
  self.buf = self.buf .. data
  return true
end

function Reader:read_line()
  while true do
    local nl = self.buf:find("\r\n", self.pos, true)
    if nl then
      local line = self.buf:sub(self.pos, nl - 1)
      self.pos = nl + 2
      return line
    end
    local ok, err = self:_fill()
    if not ok then return nil, err end
  end
end

function Reader:read_n(n)
  if n <= 0 then return "" end
  while (#self.buf - self.pos + 1) < n do
    local ok, err = self:_fill()
    if not ok then return nil, err end
  end
  local out = self.buf:sub(self.pos, self.pos + n - 1)
  self.pos = self.pos + n
  return out
end

----------------------------------------------------------------------
-- Request parsing
----------------------------------------------------------------------
local function parse_request(reader)
  local line, err = reader:read_line()
  if not line then return nil, "read request-line: " .. tostring(err) end
  local method, target, ver = line:match("^(%S+)%s+(%S+)%s+HTTP/(%d%.%d)$")
  if not method then return nil, "bad request-line: " .. line end

  local headers = {}
  while true do
    local hl, herr = reader:read_line()
    if not hl then return nil, "read header: " .. tostring(herr) end
    if hl == "" then break end
    local k, v = hl:match("^([^:]+):%s*(.*)$")
    if k then
      k = k:lower()
      if headers[k] then
        headers[k] = headers[k] .. ", " .. v
      else
        headers[k] = v
      end
    end
  end

  -- v1 parity: header lookups are case-insensitive even though keys are
  -- stored lowercased (v2 shape). v1 kept the wire spelling, so handlers
  -- reading req.headers["If-None-Match"] must keep working.
  setmetatable(headers, {
    __index = function(t, k)
      if type(k) == "string" then return rawget(t, k:lower()) end
      return nil
    end,
  })

  local body = ""
  local clen = tonumber(headers["content-length"])
  if clen and clen > 0 then
    local b, berr = reader:read_n(clen)
    if not b then return nil, "read body: " .. tostring(berr) end
    body = b
  end
  -- Transfer-Encoding: chunked request bodies are rare from browsers and are
  -- not decoded here; they will be surfaced to the handler as a 400 later if
  -- ever needed. HTTP/1.1 permits us to refuse unsupported TE.

  local path, qs = split_path_query(target)
  local query = parse_query(qs)
  -- v1 parity: req.params is the query string merged with an
  -- application/x-www-form-urlencoded body (v1 ran evhttp_parse_query_str
  -- over both); later entries win. Values are always strings.
  local params = {}
  for k, v in pairs(query) do params[k] = v end
  local ctype = headers["content-type"]
  if ctype and ctype:find("application/x-www-form-urlencoded", 1, true) == 1 then
    for k, v in pairs(parse_query(body)) do params[k] = v end
  end
  -- v1 fan.httpd's request object exposes :read() / :available() so
  -- streaming handlers can drain the body incrementally. Our body has
  -- already been fully read into a Lua string, so these methods just
  -- surface it in an incremental fashion. The `_body_pos` cursor is
  -- an internal implementation detail; callers should treat the
  -- request object as opaque outside its documented method set.
  local req = {
    method = method:upper(),
    path = path,
    target = target,        -- raw request-target incl. query
    query = qs,             -- v1: raw query string, not the parsed map
    params = params,        -- decoded query + form map
    headers = headers,
    body = body,
    http_version = ver,
    _body_pos = 1,
  }
  function req:available()
    return #self.body - (self._body_pos - 1)
  end
  function req:read(n)
    -- Match v1: no arg -> return whatever is left in one shot;
    -- integer arg -> return up to `n` bytes; returns nil when drained.
    local remaining = #self.body - (self._body_pos - 1)
    if remaining <= 0 then return nil end
    if n == nil then n = remaining end
    if type(n) ~= "number" or n <= 0 then
      error("read: n must be a positive number or nil", 2)
    end
    if n > remaining then n = remaining end
    local chunk = self.body:sub(self._body_pos, self._body_pos + n - 1)
    self._body_pos = self._body_pos + n
    return chunk
  end

  -- v1 fan.httpd parity: WebSocket surface on the request object.
  -- v1 mounted is_websocket_upgrade / websocket_accept / websocket_send /
  -- websocket_receive / websocket_ping / websocket_pong / websocket_close /
  -- websocket_state directly on the request. We reproduce that shape by
  -- delegating to fan.websocket; the underlying ws object is stashed on
  -- self._ws once :websocket_accept() completes the 101 handshake.

  -- Header check only: matches v1 is_websocket_upgrade_request().
  function req:is_websocket_upgrade()
    local h = self.headers or {}
    local upg = (h["upgrade"] or ""):lower()
    local con = (h["connection"] or ""):lower()
    return upg == "websocket"
       and con:find("upgrade", 1, true) ~= nil
       and h["sec-websocket-key"] ~= nil
       and h["sec-websocket-version"] == "13"
  end

  -- v1 :websocket_accept() takes no args (headers are auto-generated).
  -- It uses the response object stashed on the request by serve_one.
  -- Returns the ws object on success, nil+err on failure (which also
  -- sends a 400 via the response, so the handler can just return).
  function req:websocket_accept()
    if self._ws then return self._ws end
    local resp = self._resp
    if not resp then
      return nil, "websocket_accept: no response object attached"
    end
    local websocket = require("fan.websocket")
    local ws, err = websocket.accept(self, resp)
    if not ws then return nil, err end
    self._ws = ws
    return ws
  end

  -- Delegating helpers. Each one returns nil+err if the handshake has
  -- not been completed yet, matching v1's "not a websocket" rejection.
  local function need_ws(self, name)
    if not self._ws then
      return nil, name .. ": websocket handshake not accepted"
    end
    return self._ws
  end

  function req:websocket_send(msg, opcode)
    local ws, e = need_ws(self, "websocket_send"); if not ws then return nil, e end
    return ws:send(msg, opcode)
  end
  function req:websocket_receive()
    local ws, e = need_ws(self, "websocket_receive"); if not ws then return nil, e end
    return ws:recv()
  end
  function req:websocket_ping(data)
    local ws, e = need_ws(self, "websocket_ping"); if not ws then return nil, e end
    return ws:ping(data)
  end
  function req:websocket_pong(data)
    local ws, e = need_ws(self, "websocket_pong"); if not ws then return nil, e end
    return ws:pong(data)
  end
  function req:websocket_close(code, reason)
    local ws, e = need_ws(self, "websocket_close"); if not ws then return nil, e end
    return ws:close(code, reason)
  end
  function req:websocket_state()
    -- v1: before accept the state is "connecting"; after accept the ws
    -- object owns the state string.
    if not self._ws then return "connecting" end
    return self._ws:state()
  end

  return req
end

----------------------------------------------------------------------
-- Reason phrases (subset; anything unknown falls back to "OK")
----------------------------------------------------------------------
local REASONS = {
  [200] = "OK", [201] = "Created", [202] = "Accepted", [204] = "No Content",
  [301] = "Moved Permanently", [302] = "Found", [303] = "See Other",
  [304] = "Not Modified", [307] = "Temporary Redirect", [308] = "Permanent Redirect",
  [400] = "Bad Request", [401] = "Unauthorized", [403] = "Forbidden",
  [404] = "Not Found", [405] = "Method Not Allowed", [408] = "Request Timeout",
  [413] = "Payload Too Large", [414] = "URI Too Long", [415] = "Unsupported Media Type",
  [500] = "Internal Server Error", [501] = "Not Implemented",
  [502] = "Bad Gateway", [503] = "Service Unavailable",
}
local function reason_for(code) return REASONS[code] or "OK" end

----------------------------------------------------------------------
-- Response object
----------------------------------------------------------------------
local Response = {}
Response.__index = Response

local function new_response(conn)
  return setmetatable({
    conn = conn,
    sent_head = false,
    finished = false,
    chunked = false,
    -- Extra header lines accumulated via :addheader(k, v). Merged into
    -- whatever :reply / :reply_start receives. This lets v1-style code
    -- build the header set incrementally before choosing a reply mode.
    extra_headers = nil,
  }, Response)
end

-- v1 fan.httpd parity: addheader(name, value). Multiple calls with the
-- same key concatenate values comma-separated (HTTP folding), matching
-- v1 semantics. Any headers supplied later to :reply / :reply_start are
-- merged on top of these (caller wins).
function Response:addheader(name, value)
  if self.sent_head then error("addheader: response head already sent", 2) end
  if type(name) ~= "string" or value == nil then
    error("addheader: (name:string, value) required", 2)
  end
  self.extra_headers = self.extra_headers or {}
  local existing = self.extra_headers[name]
  if existing then
    self.extra_headers[name] = existing .. ", " .. tostring(value)
  else
    self.extra_headers[name] = tostring(value)
  end
end

-- Merge stashed headers with a fresh caller-provided table. Caller wins
-- when keys collide (case-insensitive), matching v1 evhttp behaviour.
local function merged_headers(self, headers)
  if not self.extra_headers then return headers end
  local out = {}
  for k, v in pairs(self.extra_headers) do out[k] = v end
  if headers then
    -- Drop any stashed header whose case-folded name matches a caller
    -- key, so the caller's value replaces (not appends).
    local caller_lower = {}
    for k in pairs(headers) do caller_lower[tostring(k):lower()] = true end
    for k in pairs(out) do
      if caller_lower[tostring(k):lower()] then out[k] = nil end
    end
    for k, v in pairs(headers) do out[k] = v end
  end
  return out
end

local function build_head(status, reason, headers, extra_lines)
  local out = { string.format("HTTP/1.1 %d %s", status, reason) }
  if headers then
    for k, v in pairs(headers) do
      out[#out + 1] = tostring(k) .. ": " .. tostring(v)
    end
  end
  if extra_lines then
    for _, l in ipairs(extra_lines) do out[#out + 1] = l end
  end
  return table.concat(out, "\r\n") .. "\r\n\r\n"
end

-- v1 callers use :reply(status, message, body) and :reply_start(status,
-- message): the second argument is a reason-phrase string, not a header
-- table. Ignore any non-table value so both call shapes work.
local function normalize_headers(headers)
  if headers ~= nil and type(headers) ~= "table" then return nil end
  return headers
end

-- one-shot reply: status, headers table, body string (v1: status, message, body)
function Response:reply(status, headers, body)
  if self.sent_head then error("reply: response head already sent", 2) end
  body = body or ""
  local reason = reason_for(status)
  headers = merged_headers(self, normalize_headers(headers))
  local h = {}
  local have_cl, have_conn = false, false
  if headers then
    for k, v in pairs(headers) do
      local lk = tostring(k):lower()
      if lk == "content-length" then have_cl = true end
      if lk == "connection"    then have_conn = true end
      h[k] = v
    end
  end
  if not have_cl   then h["Content-Length"] = #body end
  if not have_conn then h["Connection"]     = "close" end
  local head = build_head(status, reason, h)
  self.sent_head = true
  self.finished = true
  local ok, err = self.conn:send(head .. body)
  if not ok then return nil, err end
  return true
end

-- chunked reply: reply_start(headers) + reply_chunk(data)+ + reply_end()
function Response:reply_start(status, headers)
  if self.sent_head then error("reply_start: response head already sent", 2) end
  local reason = reason_for(status)
  headers = merged_headers(self, normalize_headers(headers))
  local h = {}
  local have_te, have_conn = false, false
  if headers then
    for k, v in pairs(headers) do
      local lk = tostring(k):lower()
      if lk == "content-length" then
        -- silently drop: chunked mode owns framing
      else
        if lk == "transfer-encoding" then have_te   = true end
        if lk == "connection"        then have_conn = true end
        h[k] = v
      end
    end
  end
  if not have_te   then h["Transfer-Encoding"] = "chunked" end
  if not have_conn then h["Connection"]        = "close" end
  local head = build_head(status, reason, h)
  self.sent_head = true
  self.chunked = true
  return self.conn:send(head)
end

function Response:reply_chunk(data)
  if not self.chunked then error("reply_chunk: not in chunked mode", 2) end
  if self.finished then error("reply_chunk: response already ended", 2) end
  if not data or #data == 0 then return true end
  local frame = string.format("%x\r\n", #data) .. data .. "\r\n"
  return self.conn:send(frame)
end

function Response:reply_end()
  if not self.chunked then error("reply_end: not in chunked mode", 2) end
  if self.finished then return true end
  self.finished = true
  return self.conn:send("0\r\n\r\n")
end

----------------------------------------------------------------------
-- The per-connection driver
----------------------------------------------------------------------
local function serve_one(handler, conn)
  local reader = new_reader(conn)
  local req, perr = parse_request(reader)
  local resp = new_response(conn)
  if not req then
    -- send a minimal 400 and go
    local msg = "Bad Request: " .. tostring(perr)
    pcall(resp.reply, resp, 400, { ["Content-Type"] = "text/plain" }, msg)
    return
  end
  -- v1 parity: expose the peer address on the request. v1 called this
  -- `req.remoteip`; we surface the same name plus `remote_addr`, which
  -- webase and other v1-era apps also read. getpeername may fail on a
  -- half-closed socket — in that case we leave both fields nil so
  -- authorisation checks fall through to header-based fallbacks.
  local peer_ok, peer_ip = pcall(function() return conn:getpeername() end)
  if peer_ok and peer_ip then
    req.remoteip    = peer_ip
    req.remote_addr = peer_ip
  end
  -- Stash the response on the request so v1 :websocket_accept() (which
  -- takes no arguments) can find it. Kept as an internal field.
  req._resp = resp
  local ok, err = pcall(handler, req, resp)
  if not ok then
    -- handler crashed; try to surface a 500 if the head has not been sent
    if not resp.sent_head then
      pcall(resp.reply, resp, 500, { ["Content-Type"] = "text/plain" },
            "Internal Server Error: " .. tostring(err))
    end
    -- else: response was partly on the wire, nothing safe to do but drop.
  elseif not resp.sent_head then
    -- handler returned without replying; default 204
    pcall(resp.reply, resp, 204, {}, "")
  elseif resp.chunked and not resp.finished then
    pcall(resp.reply_end, resp)
  end
end

----------------------------------------------------------------------
-- Public bind
----------------------------------------------------------------------
function M.bind(opts)
  assert(type(opts) == "table", "httpd.bind requires an options table")
  -- v1 fan.httpd used `onService`; v2 landed with `handler`. Accept both
  -- so v1 code migrates without changing the option name.
  local handler = opts.handler or opts.onService
  assert(type(handler) == "function",
         "httpd.bind requires opts.handler or opts.onService (a function)")
  local port = assert(tonumber(opts.port), "httpd.bind requires numeric opts.port")
  local host = opts.host or "127.0.0.1"

  local tcp_opts
  if opts.ssl then
    tcp_opts = { ssl = true, cert = opts.cert, key = opts.key }
  end

  -- Delegate to fan.tcp.bind; preserve its (server | nil, err) return.
  return fan.tcp.bind(host, port, function(conn)
    -- run the request/response cycle; guarantee close on the way out
    local ok, err = pcall(serve_one, handler, conn)
    conn:close()
    if not ok then
      -- surface unexpected coroutine-level errors via print so they are not lost
      io.stderr:write("httpd: connection error: " .. tostring(err) .. "\n")
    end
  end, tcp_opts)
end

-- expose helpers for tests / advanced users
M.parse_query = parse_query
M.urldecode   = urldecode

return M
