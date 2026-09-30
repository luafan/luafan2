--[[
  fan/http_lua.lua — LuaFan v2 HTTP/1.1 client, pure-Lua backend (M4).

  This module is the fallback backend for fan.http; the top-level
  fan/http.lua is now a dispatch shim that picks between "c" (M13.C
  libcurl multi + libevent) and "lua" (this file). It is kept intact
  so tests can exercise the pure-Lua path (`_G.__FAN_HTTP_BACKEND_DEFAULT
  = "lua"` or `opts.backend = "lua"`) while the C backend fills in
  parity gaps across the M13.C-* milestones.

  Built on fan.tcp (+ TLS via fan.tcp.connect{ssl=true}). Pure Lua:
  "thin C, thick Lua".

  Contract (plan §4.5): GET/POST/PUT/DELETE/HEAD; headers/body/query; chunked
  response decoding; redirects; timeout; TLS with verify on/off; per-request
  isolation with no shared global state (each request owns its connection).

  API:
    local http = require "fan.http_lua"
    local resp = http.request{
      url = "http://host:port/path?x=1",   -- or https://
      method = "GET",                        -- default GET
      headers = { ["X-Foo"] = "bar" },       -- optional
      query = { a = 1, b = "two" },          -- optional, merged into URL
      body = "raw body string",              -- optional
      timeout = 10,                          -- optional seconds (per read/connect)
      verify = true,                         -- TLS peer/host verify (default true)
      follow_redirects = true,               -- default true
      max_redirects = 5,                     -- default 5
    }
    -- resp = { status=200, reason="OK", headers={...lowercased...}, body="..." }
    -- on error: nil, errmsg

  Must run inside a fan coroutine (fan.spawn / fan.loop).
]]

local fan = require "fan"

local M = {}

----------------------------------------------------------------------
-- URL parsing
----------------------------------------------------------------------
-- returns scheme, host, port, path_with_query (path defaults to "/")
local function parse_url(url)
  local scheme, rest = url:match("^(%w+)://(.*)$")
  if not scheme then return nil, "invalid url (no scheme): " .. tostring(url) end
  scheme = scheme:lower()
  local authority, pathq = rest:match("^([^/]*)(/.*)$")
  if not authority then authority, pathq = rest, "/" end
  -- strip userinfo if present (not supported, but tolerate)
  authority = authority:gsub("^[^@]*@", "")
  local host, port
  if authority:match("^%[") then
    -- IPv6 literal [::1]:port
    host, port = authority:match("^%[([^%]]+)%]:?(%d*)$")
  else
    host, port = authority:match("^([^:]+):?(%d*)$")
  end
  if not host then return nil, "invalid url authority: " .. tostring(authority) end
  if port == "" or port == nil then
    port = (scheme == "https") and 443 or 80
  else
    port = tonumber(port)
  end
  return scheme, host, port, pathq
end

local function urlencode(s)
  return (tostring(s):gsub("[^%w%-_%.~]", function(c)
    return string.format("%%%02X", string.byte(c))
  end))
end

local function build_query(t)
  local parts = {}
  for k, v in pairs(t) do
    parts[#parts + 1] = urlencode(k) .. "=" .. urlencode(v)
  end
  return table.concat(parts, "&")
end

----------------------------------------------------------------------
-- Buffered reader over a fan.tcp conn
----------------------------------------------------------------------
local Reader = {}
Reader.__index = Reader

local function new_reader(conn)
  return setmetatable({ conn = conn, buf = "", pos = 1, onreceive = nil }, Reader)
end

local function invoke_onreceive(reader, chunk)
  if not reader.onreceive or #chunk == 0 then return true end
  local ok, result = pcall(reader.onreceive, chunk)
  if not ok then
    reader.callback_error = "onreceive callback error: " .. tostring(result)
    return nil, reader.callback_error
  end
  if result == false then
    reader.callback_error = "onreceive callback canceled"
    return nil, reader.callback_error
  end
  return true
end

local function reader_error(reader, err)
  return nil, reader.callback_error or err
end

-- pull more bytes from the socket into the buffer; returns true or nil,err
function Reader:_fill()
  local data, err = self.conn:receive()
  if not data then return reader_error(self, err or "eof") end
  -- compact consumed prefix occasionally to bound memory
  if self.pos > 1 then
    self.buf = self.buf:sub(self.pos)
    self.pos = 1
  end
  self.buf = self.buf .. data
  return true
end

-- read a single line terminated by CRLF (CRLF stripped); returns line or nil,err
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

-- read exactly n bytes; returns string or nil,err
function Reader:read_n(n)
  while (#self.buf - self.pos + 1) < n do
    local ok, err = self:_fill()
    if not ok then return nil, err end
  end
  local out = self.buf:sub(self.pos, self.pos + n - 1)
  self.pos = self.pos + n
  local ok, err = invoke_onreceive(self, out)
  if not ok then return reader_error(self, err) end
  return out
end

-- read all remaining bytes until EOF; returns string (possibly empty)
function Reader:read_until_eof()
  local chunks = {}
  if self.pos <= #self.buf then
    local pending = self.buf:sub(self.pos)
    self.pos = #self.buf + 1
    local ok, err = invoke_onreceive(self, pending)
    if not ok then return reader_error(self, err) end
    chunks[#chunks + 1] = pending
  end
  while true do
    local data = self.conn:receive()
    if not data then break end
    local ok, err = invoke_onreceive(self, data)
    if not ok then return reader_error(self, err) end
    chunks[#chunks + 1] = data
  end
  return table.concat(chunks)
end

----------------------------------------------------------------------
-- Response parsing
----------------------------------------------------------------------
local function parse_status_line(line)
  -- HTTP/1.1 200 OK
  local ver, code, reason = line:match("^HTTP/(%d%.%d)%s+(%d%d%d)%s*(.*)$")
  if not code then return nil, "bad status line: " .. tostring(line) end
  return tonumber(code), reason, ver
end

local function read_headers(reader)
  local headers = {}
  while true do
    local line, err = reader:read_line()
    if not line then return nil, err end
    if line == "" then break end  -- blank line ends headers
    local k, v = line:match("^([^:]+):%s*(.*)$")
    if k then
      k = k:lower()
      -- fold duplicate headers (e.g. Set-Cookie) into a list-ish comma join
      if headers[k] then
        headers[k] = headers[k] .. ", " .. v
      else
        headers[k] = v
      end
    end
  end
  return headers
end

-- decode a chunked transfer body; returns body string or nil,err
local function read_chunked(reader)
  local chunks = {}
  while true do
    local line, err = reader:read_line()
    if not line then return nil, err end
    -- chunk size may carry extensions after ';'
    local hex = line:match("^(%x+)")
    if not hex then return nil, "bad chunk size: " .. tostring(line) end
    local size = tonumber(hex, 16)
    if size == 0 then
      -- consume trailing headers until blank line
      while true do
        local t = reader:read_line()
        if not t or t == "" then break end
      end
      break
    end
    local data, derr = reader:read_n(size)
    if not data then return nil, derr end
    chunks[#chunks + 1] = data
    -- trailing CRLF after each chunk
    reader:read_line()
  end
  return table.concat(chunks)
end

----------------------------------------------------------------------
-- Single request/response over one connection
----------------------------------------------------------------------
-- Accept UPDATE too, matching v1 fan.http.update (a non-standard verb
-- some legacy REST APIs used; equivalent to a PUT with different semantics).
local METHODS = { GET = true, POST = true, PUT = true, DELETE = true,
                  HEAD = true, PATCH = true, OPTIONS = true, UPDATE = true }

local function do_once(opts)
  local scheme, host, port, pathq = parse_url(opts.url)
  if not scheme then return nil, host end  -- host holds error msg
  if scheme ~= "http" and scheme ~= "https" then
    return nil, "unsupported scheme: " .. scheme
  end

  -- merge query table into the path
  if opts.query and next(opts.query) then
    local sep = pathq:find("?", 1, true) and "&" or "?"
    pathq = pathq .. sep .. build_query(opts.query)
  end

  local method = (opts.method or "GET"):upper()
  if not METHODS[method] then return nil, "unsupported method: " .. method end

  local verify = opts.verify
  if verify == nil then verify = true end

  -- M21 — Forward CA bundle knobs down to fan.tcp.connect so pure-Lua HTTPS
  -- can pin its own trust store.  Per-request opts.cainfo/opts.capath win;
  -- if unset we fall back to the module-scoped M._cainfo / M._capath (set
  -- via M.cainfo()/M.capath(), v1-compatible).  fan.tcp.connect (M21.2)
  -- routes through fan_tls_client_bev_ex when either is non-nil.  ssl_host
  -- (opts.ssl_host) lets callers override the SNI + verify hostname —
  -- useful when connecting to a raw IP or a private hostname aliased
  -- through /etc/hosts for the same origin cert.
  local cainfo = opts.cainfo or M._cainfo
  local capath = opts.capath or M._capath
  local conn, cerr = fan.tcp.connect(host, port, {
    ssl = (scheme == "https"),
    verify_peer = verify,
    verify_host = verify,
    ssl_host    = opts.ssl_host,
    cainfo      = cainfo,
    capath      = capath,
  })
  if not conn then return nil, "connect: " .. tostring(cerr) end

  -- build request
  local body = opts.body or ""
  local lines = {
    string.format("%s %s HTTP/1.1", method, pathq),
    "Host: " .. host .. ((port ~= 80 and port ~= 443) and (":" .. port) or ""),
    "Connection: close",  -- one request per connection: simple + isolated
    "Accept: */*",
  }
  local sent_headers = {}
  if opts.headers then
    for k, v in pairs(opts.headers) do
      lines[#lines + 1] = k .. ": " .. v
      sent_headers[k:lower()] = true
    end
  end
  if #body > 0 and not sent_headers["content-length"] then
    lines[#lines + 1] = "Content-Length: " .. #body
  end
  local request = table.concat(lines, "\r\n") .. "\r\n\r\n" .. body

  local ok, serr = conn:send(request)
  if not ok then conn:close(); return nil, "send: " .. tostring(serr) end

  local reader = new_reader(conn)
  local status_line, lerr = reader:read_line()
  if not status_line then conn:close(); return nil, "read status: " .. tostring(lerr) end
  local code, reason = parse_status_line(status_line)
  if not code then conn:close(); return nil, reason end

  local headers, herr = read_headers(reader)
  if not headers then conn:close(); return nil, herr end

  if opts.onheader ~= nil and type(opts.onheader) ~= "function" then
    conn:close(); return nil, "onheader must be a function"
  end
  if opts.onreceive ~= nil and type(opts.onreceive) ~= "function" then
    conn:close(); return nil, "onreceive must be a function"
  end
  if opts.onheader then
    local header_info = {
      status = code,
      responseCode = code,
      reason = reason,
      headers = headers,
    }
    local ok, result = pcall(opts.onheader, header_info)
    if not ok then
      conn:close(); return nil, "onheader callback error: " .. tostring(result)
    end
    if result == false then
      conn:close(); return nil, "onheader callback canceled"
    end
  end
  reader.onreceive = opts.onreceive
  -- M20.2: streaming mode (buffered=false) drops response body accumulation.
  -- Callers get the segments via onreceive; response.body is "". Default
  -- (buffered nil/true) preserves the M20.1 double-output contract.
  local buffered = opts.buffered
  if buffered == nil then buffered = true end

  -- body framing
  local resp_body = ""
  local no_body = (method == "HEAD") or (code == 204) or (code == 304)
      or (code >= 100 and code < 200)
  if not no_body then
    local te = (headers["transfer-encoding"] or ""):lower()
    if te:find("chunked", 1, true) then
      local b, berr = read_chunked(reader)
      if not b then conn:close(); return nil, berr end
      if buffered then resp_body = b end
    elseif headers["content-length"] then
      local n = tonumber(headers["content-length"])
      if n and n > 0 then
        local b, berr = reader:read_n(n)
        if not b then conn:close(); return nil, berr end
        if buffered then resp_body = b end
      end
    else
      -- read until EOF (Connection: close semantics)
      local b = reader:read_until_eof()
      if buffered then resp_body = b end
    end
  end

  conn:close()
  return {
    status       = code,
    -- M16.4: v1 fan.http named this field `responseCode`; keep as an alias
    -- so legacy code reading resp.responseCode works on both backends.
    responseCode = code,
    reason       = reason,
    headers      = headers,
    body         = resp_body,
  }
end

----------------------------------------------------------------------
-- Public request with redirect following
----------------------------------------------------------------------
local function is_redirect(code) return code == 301 or code == 302
    or code == 303 or code == 307 or code == 308 end

function M.request(opts)
  assert(type(opts) == "table" and opts.url, "http.request requires {url=...}")
  local follow = opts.follow_redirects
  if follow == nil then follow = true end
  local max_redirects = opts.max_redirects or 5

  local url = opts.url
  local method = opts.method
  local hops = 0
  while true do
    local o = {}
    for k, v in pairs(opts) do o[k] = v end
    o.url = url
    o.method = method or opts.method
    local resp, err = do_once(o)
    if not resp then return nil, err end
    if follow and is_redirect(resp.status) and resp.headers["location"] then
      hops = hops + 1
      if hops > max_redirects then return nil, "too many redirects" end
      local loc = resp.headers["location"]
      -- resolve relative redirect against current url
      if not loc:match("^%w+://") then
        local scheme, host, port = parse_url(url)
        local base = scheme .. "://" .. host
            .. ((port ~= 80 and port ~= 443) and (":" .. port) or "")
        if loc:sub(1, 1) ~= "/" then loc = "/" .. loc end
        loc = base .. loc
      end
      url = loc
      -- 303 (and commonly 301/302 for POST) switch to GET
      if resp.status == 303 or ((resp.status == 301 or resp.status == 302)
          and (method or opts.method or "GET"):upper() == "POST") then
        method = "GET"
        opts.body = nil
      end
    else
      return resp
    end
  end
end

-- ---------------------------------------------------------------------------
-- Convenience verbs.
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

M.parse_url = parse_url
M.urlencode = urlencode

-- ---------------------------------------------------------------------------
-- v1 fan.http compatibility surface: escape/unescape and the cookiejar /
-- cainfo / capath global setters. These are byte-compatible with the v1 C
-- module's public names so migrating code doesn't have to switch APIs.
--
-- escape/unescape match libcurl's curl_escape semantics: RFC 3986 %xx
-- encoding for everything outside [A-Za-z0-9_.~-]. NOT the older
-- application/x-www-form-urlencoded "+"-for-space form; that's what
-- curl_escape actually does contrary to what the name might suggest.
-- ---------------------------------------------------------------------------
local function is_unreserved(b)
  return (b >= 0x30 and b <= 0x39)   -- 0-9
      or (b >= 0x41 and b <= 0x5A)   -- A-Z
      or (b >= 0x61 and b <= 0x7A)   -- a-z
      or b == 0x2D or b == 0x2E      -- - .
      or b == 0x5F or b == 0x7E      -- _ ~
end

function M.escape(s)
  if type(s) ~= "string" then return nil end
  local out = {}
  for i = 1, #s do
    local b = s:byte(i)
    if is_unreserved(b) then out[#out + 1] = string.char(b)
    else                     out[#out + 1] = string.format("%%%02X", b) end
  end
  return table.concat(out)
end

function M.unescape(s)
  if type(s) ~= "string" then return nil end
  local ok, dec = pcall(function()
    return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
  end)
  if not ok then return nil end
  return dec
end

-- Global setters kept in module-level state (v1 stashed them in the Lua
-- registry via KEY_COOKIE_JAR / KEY_CAINFO / KEY_CAPATH). Same shape:
-- string arg, no return value, one call replaces the previous value.
--
-- M21 update — cainfo / capath are now consumed by the pure-Lua request
-- path (see the fan.tcp.connect call earlier in this module).  cookiejar
-- is still only wired up on the libcurl (C) backend; the Lua backend
-- treats it as a stashed value so v1 boot code that calls
-- M.cookiejar(path) unconditionally still works, and the C backend
-- picks it up when it's the active backend.
function M.cookiejar(path)
  M._cookiejar = tostring(path)
end

function M.cainfo(path)
  M._cainfo = tostring(path)
end

function M.capath(path)
  M._capath = tostring(path)
end

-- ---------------------------------------------------------------------------
-- Additional convenience verbs for v1 parity.
-- ---------------------------------------------------------------------------
M.patch  = verb("PATCH")
M.update = verb("UPDATE")

return M
