--[[
  fan/connector.lua — URL-based connector dispatch (v2 Lua layer).

  connector.connect(url [, opts]) -> conn | nil, err
    tcp://host:port           -> fan.tcp.connect(host, port, opts)
    fifo:///path or fifo:path -> fan.fifo.open{ name=path, mode=opts.mode or "w" }
    udp://host:port           -> wrapped fan.udp sock addressed at host:port
    popen:command args...     -> fan.popen.spawn{ command=rest }

  connector.bind(url, on_accept) -> server | nil, err
    tcp://host:port           -> fan.tcp.bind(host, port, on_accept)
    fifo:///path              -> fan.fifo.open{ name=path, mode="r", create=true }
                                 then spawns on_accept(fifo) once
    udp://host:port           -> fan.udp.new{ bind } + reader coroutine
                                 calling on_accept(sock, data, from_host, from_port)
                                 per datagram
    popen://…                 -> nil, err (popen has no accept semantics)

  The connector normalizes transports behind one interface so higher layers
  (http, rpc, …) don't care whether they talk TCP, a local FIFO, a UDP
  datagram socket, or a piped subprocess.

  UDP notes (v2 design decision):
    v1's fan.connector.udp wired a heavy reliable_udp (windowed reliable
    stream over UDP) into the connector layer. In v2 that policy stays
    opt-in: reliable UDP is `require("fan.reliable_udp")` directly by
    callers who want it, so this connector only exposes the raw datagram
    surface. Callers that want reliable delivery build their reliable_udp
    session on top of the sock this returns (or bypass connector).

  POPEN notes:
    v1's connector had no popen scheme at all; adding it here is a v2
    extension so URL-driven services (RPC / test harnesses / scripting
    layers) can uniformly say `popen:/usr/bin/env cat`. The `rest`
    portion after `popen:` is passed through to `fan.popen.spawn{
    command=rest }`, which supports either a whitespace-split string or
    an argv array. We accept both `popen:cmd args` and `popen:///cmd
    args` (both slashes are stripped after `popen:`).
]]
local fan = require("fan")

local M = {}

local function parse(url)
  -- scheme://rest  OR  scheme:rest
  local scheme, rest = url:match("^(%w+)://(.*)$")
  if not scheme then
    scheme, rest = url:match("^(%w+):(.*)$")
  end
  return scheme, rest
end

local function parse_host_port(rest)
  -- IPv6-in-brackets: [::1]:53, or plain host:port.
  local host, port = rest:match("^%[([^%]]+)%]:(%d+)$")
  if not host then
    host, port = rest:match("^([^:]+):(%d+)$")
  end
  if not host then return nil end
  local p = tonumber(port)
  if not p or p < 0 or p > 65535 then return nil end
  return host, p
end

----------------------------------------------------------------------
-- UDP wrapper: a small facade so callers get send()/recv()/close() in
-- one object regardless of connect vs bind.
--
-- For `connect(udp://host:port)`:
--   c:send(data)          -> sendto(host, port)
--   c:recv()              -> data, from_host, from_port (yields)
--   c:close()             -> release sock
--
-- For `bind(udp://host:port, on_accept)`:
--   The bound sock is returned. Every incoming datagram triggers a
--   coroutine call to on_accept(sock, data, from_host, from_port).
--   Errors from on_accept are logged to stderr; the reader keeps going.
----------------------------------------------------------------------
local udp_client_mt = { __index = {} }

function udp_client_mt.__index:send(data)
  if self._closed then return nil, "closed" end
  return self._sock:sendto(data, self._host, self._port)
end

function udp_client_mt.__index:recv()
  if self._closed then return nil, "closed" end
  return self._sock:recv()
end

function udp_client_mt.__index:close()
  if not self._closed then
    self._closed = true
    self._sock:close()
  end
end

local function udp_family_for_host(host)
  -- Numeric IPv6 literals contain ':' (and optionally '::'); numeric
  -- IPv4 doesn't. fan.udp.new picks inet6 when the family arg is
  -- explicit — auto-picking here keeps callers from having to know.
  if host:find(":", 1, true) then return "inet6" end
  return nil                              -- default = inet
end

local function udp_connect(host, port)
  local family = udp_family_for_host(host)
  local sock, err = fan.udp.new(nil, nil, family)
  if not sock then return nil, err end
  return setmetatable({
    _sock   = sock,
    _host   = host,
    _port   = port,
    _closed = false,
  }, udp_client_mt)
end

local function udp_bind(host, port, on_accept)
  local family = udp_family_for_host(host)
  local sock, err = fan.udp.new(host, port, family)
  if not sock then return nil, err end
  if type(on_accept) == "function" then
    fan.spawn(function()
      while true do
        local data, from_host, from_port = sock:recv()
        if data == nil then return end   -- sock closed / error
        local ok, cberr = pcall(on_accept, sock, data, from_host, from_port)
        if not ok then
          io.stderr:write("[fan.connector udp] on_accept error: ",
                          tostring(cberr), "\n")
        end
      end
    end)
  end
  return sock
end

----------------------------------------------------------------------
-- Scheme dispatch tables. Kept explicit so `connect` and `bind` fail
-- fast on schemes that don't support their operation (popen has no
-- server-side accept surface, for example).
----------------------------------------------------------------------

function M.connect(url, opts)
  opts = opts or {}
  local scheme, rest = parse(url)
  if not scheme then return nil, "invalid url: " .. tostring(url) end

  if scheme == "tcp" then
    local host, port = parse_host_port(rest)
    if not host then return nil, "tcp url needs host:port" end
    return fan.tcp.connect(host, port, opts)
  elseif scheme == "fifo" then
    local path = rest:gsub("^//", "")   -- fifo:///p or fifo:/p or fifo:p
    return fan.fifo.open{ name = path, mode = opts.mode or "w", create = opts.create }
  elseif scheme == "udp" then
    local host, port = parse_host_port(rest)
    if not host then return nil, "udp url needs host:port" end
    return udp_connect(host, port)
  elseif scheme == "popen" then
    -- popen:cmd args  or  popen:///cmd args  (both slashes are optional
    -- and stripped uniformly). The remainder is passed straight to
    -- fan.popen.spawn{command=rest}, which supports string or argv table.
    local cmd = rest:gsub("^//", "")
    if cmd == "" then return nil, "popen url needs a command" end
    return fan.popen.spawn{ command = cmd,
                            capture_stderr = opts.capture_stderr }
  end
  return nil, "unsupported scheme: " .. scheme
end

function M.bind(url, on_accept)
  local scheme, rest = parse(url)
  if not scheme then return nil, "invalid url: " .. tostring(url) end

  if scheme == "tcp" then
    local host, port = parse_host_port(rest)
    if not host then return nil, "tcp url needs host:port" end
    return fan.tcp.bind(host, port, on_accept)
  elseif scheme == "fifo" then
    local path = rest:gsub("^//", "")
    local f, err = fan.fifo.open{ name = path, mode = "r", create = true }
    if not f then return nil, err end
    fan.spawn(function() on_accept(f) end)
    return f
  elseif scheme == "udp" then
    local host, port = parse_host_port(rest)
    if not host then return nil, "udp url needs host:port" end
    return udp_bind(host, port, on_accept)
  elseif scheme == "popen" then
    return nil, "popen scheme has no bind: use connector.connect() to spawn"
  end
  return nil, "unsupported scheme: " .. scheme
end

return M
