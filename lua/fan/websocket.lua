--[[
  fan/websocket.lua — LuaFan v2 WebSocket (RFC 6455) support.

  Design: pure Lua on top of fan.tcp, integrated with fan.httpd. Server-side
  only in this iteration (client-side handshake can be layered later on
  fan.http). No permessage-deflate yet — that pairs with the M4 zlib feature
  gate. Frame layer supports:
    - text (0x1) and binary (0x2) data frames
    - control frames: ping (0x9), pong (0xA), close (0x8)
    - masked client -> server frames (RFC requires); unmasked server -> client
    - Extended payload length: 7 / 7+16 / 7+64 bit forms
    - RSV bits MUST be zero (RFC 6455 \u00a75.2) until deflate is negotiated
    - fragmentation: fragmented DATA frames are joined into one message
    - control frames MUST NOT be fragmented and MUST have payload <= 125

  API (server-side, called from a fan.httpd handler):
    local ws, err = websocket.accept(req, resp)
    -- ws:recv() -> msg_str, opcode  ("text" | "binary")  OR  nil, err
    --   ping/pong/close are handled automatically (pong replies to ping;
    --   close triggers our reply-close and turns the next recv into nil,"closed")
    -- ws:send(msg [, opcode])       -- opcode default "text"; safe across
    --                                  coroutines (serialised via an internal lock)
    -- ws:send_binary(msg)
    -- ws:ping([data])
    -- ws:close(code, reason)        -- sends close frame + tears down the tcp conn
    -- ws.closed                     -- boolean; true after the close handshake

  Cross-coroutine send safety: coroutines A and B both calling ws:send() must
  not interleave frame bytes on the wire. We queue senders on a busy flag and
  fan.sleep-poll until the previous send is done. Single-loop model = no true
  threads (M6 adds workers); this covers "cross-thread send" for v2 today.
]]

local fan = require "fan"

local M = {}

----------------------------------------------------------------------
-- base64 (RFC 4648) — small pure-Lua encoder, used for the handshake key
----------------------------------------------------------------------
local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function base64_encode(data)
  local out = {}
  local n = #data
  local i = 1
  while i <= n do
    local b1 = data:byte(i)
    local b2 = (i + 1 <= n) and data:byte(i + 1) or 0
    local b3 = (i + 2 <= n) and data:byte(i + 2) or 0
    local trip = b1 * 65536 + b2 * 256 + b3
    local c1 = (trip >> 18) & 0x3f
    local c2 = (trip >> 12) & 0x3f
    local c3 = (trip >>  6) & 0x3f
    local c4 =  trip        & 0x3f
    out[#out + 1] = B64:sub(c1 + 1, c1 + 1)
    out[#out + 1] = B64:sub(c2 + 1, c2 + 1)
    if i + 1 <= n then
      out[#out + 1] = B64:sub(c3 + 1, c3 + 1)
    else
      out[#out + 1] = "="
    end
    if i + 2 <= n then
      out[#out + 1] = B64:sub(c4 + 1, c4 + 1)
    else
      out[#out + 1] = "="
    end
    i = i + 3
  end
  return table.concat(out)
end

M.base64_encode = base64_encode

----------------------------------------------------------------------
-- SHA-1 (RFC 3174) — small pure-Lua impl for the handshake accept key
--   Not perf-critical: one 20-byte hash per connection.
----------------------------------------------------------------------
local band, bor, bxor, bnot = 0, 0, 0, 0  -- placeholder to appease older tools
local function _rotl32(x, n)
  x = x & 0xffffffff
  return ((x << n) | (x >> (32 - n))) & 0xffffffff
end

local function sha1_binary(msg)
  local h0, h1, h2, h3, h4 =
    0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0

  -- pre-processing: append 0x80, pad with zeros, append 64-bit big-endian length
  local ml = #msg * 8
  local m = msg .. "\128"
  local pad = (56 - (#m % 64)) % 64
  m = m .. string.rep("\0", pad)
  -- 64-bit length, big-endian (high 32 bits are 0 for lengths < 2^32 bits)
  m = m .. string.pack(">I4I4", (ml >> 32) & 0xffffffff, ml & 0xffffffff)

  local W = {}
  for chunk = 1, #m, 64 do
    for i = 0, 15 do
      W[i + 1] = string.unpack(">I4", m, chunk + i * 4)
    end
    for i = 17, 80 do
      W[i] = _rotl32(W[i - 3] ~ W[i - 8] ~ W[i - 14] ~ W[i - 16], 1)
    end
    local a, b, c, d, e = h0, h1, h2, h3, h4
    for i = 1, 80 do
      local f, k
      if i <= 20 then
        f = (b & c) | ((~b) & 0xffffffff & d); k = 0x5A827999
      elseif i <= 40 then
        f = b ~ c ~ d;                         k = 0x6ED9EBA1
      elseif i <= 60 then
        f = (b & c) | (b & d) | (c & d);       k = 0x8F1BBCDC
      else
        f = b ~ c ~ d;                         k = 0xCA62C1D6
      end
      local t = (_rotl32(a, 5) + f + e + k + W[i]) & 0xffffffff
      e = d; d = c; c = _rotl32(b, 30); b = a; a = t
    end
    h0 = (h0 + a) & 0xffffffff
    h1 = (h1 + b) & 0xffffffff
    h2 = (h2 + c) & 0xffffffff
    h3 = (h3 + d) & 0xffffffff
    h4 = (h4 + e) & 0xffffffff
  end
  return string.pack(">I4I4I4I4I4", h0, h1, h2, h3, h4)
end

M.sha1_binary = sha1_binary

----------------------------------------------------------------------
-- Handshake
----------------------------------------------------------------------
local WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

local function compute_accept(key)
  return base64_encode(sha1_binary(key .. WS_GUID))
end
M.compute_accept = compute_accept

----------------------------------------------------------------------
-- Frame codec
----------------------------------------------------------------------
-- read exactly n bytes from a conn via a small buffered reader (piggybacked
-- on the same pattern as fan/http.lua)
local function reader_new(conn)
  return { conn = conn, buf = "", pos = 1 }
end
local function reader_read_n(r, n)
  if n <= 0 then return "" end
  while (#r.buf - r.pos + 1) < n do
    local d = r.conn:receive()
    if not d then return nil, "eof" end
    if r.pos > 1 then r.buf = r.buf:sub(r.pos); r.pos = 1 end
    r.buf = r.buf .. d
  end
  local out = r.buf:sub(r.pos, r.pos + n - 1)
  r.pos = r.pos + n
  return out
end

local function apply_mask(payload, key)
  if not key or #key ~= 4 then return payload end
  local out = {}
  local k1, k2, k3, k4 = key:byte(1, 4)
  local kk = { k1, k2, k3, k4 }
  for i = 1, #payload do
    out[i] = string.char(payload:byte(i) ~ kk[((i - 1) % 4) + 1])
  end
  return table.concat(out)
end

-- read one frame; returns {fin, opcode, rsv1, payload} or nil, err
local function read_frame(r)
  local hdr, err = reader_read_n(r, 2)
  if not hdr then return nil, err end
  local b1, b2 = hdr:byte(1, 2)
  local fin    = (b1 & 0x80) ~= 0
  local rsv1   = (b1 & 0x40) ~= 0
  local rsv23  = b1 & 0x30      -- RSV2 | RSV3 must be 0 (no extension defines them)
  local opcode = b1 & 0x0f
  local masked = (b2 & 0x80) ~= 0
  local plen   = b2 & 0x7f

  if rsv23 ~= 0 then return nil, "protocol error: RSV2/RSV3 set" end

  if plen == 126 then
    local ext = reader_read_n(r, 2); if not ext then return nil, "eof" end
    plen = string.unpack(">I2", ext)
  elseif plen == 127 then
    local ext = reader_read_n(r, 8); if not ext then return nil, "eof" end
    plen = string.unpack(">I8", ext)
  end

  -- control frame checks (RFC 6455 \u00a75.5)
  local is_control = opcode >= 0x8
  if is_control then
    if not fin  then return nil, "protocol error: fragmented control frame" end
    if plen > 125 then return nil, "protocol error: oversized control frame" end
    if rsv1     then return nil, "protocol error: RSV1 set on control frame" end
  end

  local mask_key
  if masked then
    mask_key = reader_read_n(r, 4); if not mask_key then return nil, "eof" end
  end
  local payload = plen > 0 and reader_read_n(r, plen) or ""
  if plen > 0 and not payload then return nil, "eof" end
  if masked then payload = apply_mask(payload, mask_key) end

  return { fin = fin, opcode = opcode, rsv1 = rsv1, payload = payload }
end

-- encode one server -> client frame (unmasked). rsv1 flags per-message deflate.
local function encode_frame(fin, opcode, payload, rsv1)
  local b1 = (fin and 0x80 or 0) | (rsv1 and 0x40 or 0) | (opcode & 0x0f)
  local n = #payload
  local hdr
  if n < 126 then
    hdr = string.char(b1, n)
  elseif n < 65536 then
    hdr = string.char(b1, 126) .. string.pack(">I2", n)
  else
    hdr = string.char(b1, 127) .. string.pack(">I8", n)
  end
  return hdr .. payload
end

----------------------------------------------------------------------
-- Opcodes
----------------------------------------------------------------------
local OP_CONT   = 0x0
local OP_TEXT   = 0x1
local OP_BIN    = 0x2
local OP_CLOSE  = 0x8
local OP_PING   = 0x9
local OP_PONG   = 0xA

----------------------------------------------------------------------
-- WebSocket connection object
----------------------------------------------------------------------
local WS = {}
WS.__index = WS

local function new_ws(conn)
  return setmetatable({
    conn = conn,
    reader = reader_new(conn),
    closed = false,
    close_sent = false,
    send_busy = false,
  }, WS)
end

-- serialised send: any number of coroutines may call ws:send*; we ensure
-- one frame goes out at a time by parking behind send_busy.
local function send_locked(self, frame)
  while self.send_busy do fan.sleep(0.005) end
  if self.closed then return nil, "closed" end
  self.send_busy = true
  local ok, err = self.conn:send(frame)
  self.send_busy = false
  if not ok then return nil, err end
  return true
end

function WS:send(msg, opcode)
  if self.closed then return nil, "closed" end
  opcode = opcode or "text"
  local op = OP_TEXT
  if opcode == "binary" or opcode == OP_BIN then op = OP_BIN
  elseif opcode == "text" or opcode == OP_TEXT then op = OP_TEXT
  else return nil, "unknown opcode: " .. tostring(opcode) end

  local payload = msg or ""
  local rsv1 = false
  if self.deflate_enabled and #payload > 0 then
    -- permessage-deflate with server_no_context_takeover: deflate each
    -- message with a fresh zlib stream, then strip the terminating
    -- 0x00 0x00 0xFF 0xFF sync flush trailer (RFC 7692 \u00a77.2.1).
    local zlib = require("fan").zlib
    -- Z_SYNC_FLUSH: leaves the stream open and emits the required
    -- 00 00 FF FF sync marker. RFC 7692 \u00a77.2.1 says the sender MUST
    -- strip those 4 bytes before framing.
    local d, derr = zlib.deflate_raw(payload, nil, true)
    if not d then return nil, "deflate: " .. tostring(derr) end
    if d:sub(-4) == "\0\0\xff\xff" then
      d = d:sub(1, -5)
    end
    -- if the output was somehow shorter than the sync marker (empty
    -- payload), the sender must still emit at least one byte so the peer
    -- has a frame to decompress.
    if #d == 0 then d = "\0" end
    payload = d
    rsv1 = true
  end
  return send_locked(self, encode_frame(true, op, payload, rsv1))
end

function WS:send_binary(msg) return self:send(msg, "binary") end

function WS:ping(data)
  if self.closed then return nil, "closed" end
  return send_locked(self, encode_frame(true, OP_PING, data or ""))
end

-- v1 parity: proactively send a pong. Servers rarely need this (recv()
-- already answers incoming pings automatically), but a peer may use
-- unsolicited pongs as a keep-alive per RFC 6455 \u00a75.5.3.
function WS:pong(data)
  if self.closed then return nil, "closed" end
  return send_locked(self, encode_frame(true, OP_PONG, data or ""))
end

-- v1 parity: state string. We track four values so callers can
-- distinguish an in-flight close handshake from a fully closed
-- connection.
function WS:state()
  if self.closed then return "closed" end
  if self.close_sent then return "closing" end
  return "open"
end

-- v1 parity: :receive alias for :recv. v1 fan.httpd's request object
-- exposes websocket_receive, and callers commonly type that name.
function WS.receive(self) return self:recv() end

local function send_close_frame(self, code, reason)
  if self.close_sent or self.closed then return true end
  self.close_sent = true
  local payload = ""
  if code then
    payload = string.pack(">I2", code) .. (reason or "")
  end
  local ok, err = send_locked(self, encode_frame(true, OP_CLOSE, payload))
  return ok, err
end

function WS:close(code, reason)
  send_close_frame(self, code or 1000, reason)
  self.closed = true
  if self.conn then pcall(self.conn.close, self.conn) end
  return true
end

-- read next application message; ping/pong/close handled inline.
-- returns:  msg_string, "text"|"binary"   on success
--           nil, "closed"                 after peer close
--           nil, err                      on protocol/IO error
function WS:recv()
  if self.closed then return nil, "closed" end
  local msg_parts = {}
  local msg_op = nil        -- "text" or "binary"
  local msg_compressed = false  -- RSV1 on the initial data frame
  while true do
    local f, err = read_frame(self.reader)
    if not f then
      self.closed = true
      return nil, err
    end

    -- RSV1 is only meaningful on data frames when deflate was negotiated;
    -- on all other traffic it must be zero.
    if f.rsv1 and (f.opcode == OP_CONT or not self.deflate_enabled) then
      self.closed = true
      return nil, "protocol error: RSV1 set without permessage-deflate context"
    end

    if f.opcode == OP_PING then
      -- automatic pong echo (never compressed regardless of extension)
      send_locked(self, encode_frame(true, OP_PONG, f.payload))
    elseif f.opcode == OP_PONG then
      -- ignored (higher-level ping/pong tracking can hook here later)
    elseif f.opcode == OP_CLOSE then
      -- echo close and tear down
      local code, reason
      if #f.payload >= 2 then
        code = string.unpack(">I2", f.payload)
        reason = f.payload:sub(3)
      end
      send_close_frame(self, code or 1000, reason)
      self.closed = true
      if self.conn then pcall(self.conn.close, self.conn) end
      return nil, "closed"
    elseif f.opcode == OP_TEXT or f.opcode == OP_BIN then
      if msg_op then
        self.closed = true
        return nil, "protocol error: new data opcode inside fragmented message"
      end
      msg_op = (f.opcode == OP_TEXT) and "text" or "binary"
      msg_compressed = f.rsv1
      msg_parts[#msg_parts + 1] = f.payload
      if f.fin then
        local body = table.concat(msg_parts)
        if msg_compressed then
          -- append RFC 7692 \u00a77.2.2 sync-flush trailer and inflate
          local zlib = require("fan").zlib
          local inflated, ierr = zlib.inflate_raw(body .. "\0\0\xff\xff")
          if not inflated then
            self.closed = true
            return nil, "inflate: " .. tostring(ierr)
          end
          body = inflated
        end
        return body, msg_op
      end
    elseif f.opcode == OP_CONT then
      if not msg_op then
        self.closed = true
        return nil, "protocol error: continuation without initial data frame"
      end
      msg_parts[#msg_parts + 1] = f.payload
      if f.fin then
        local body = table.concat(msg_parts)
        if msg_compressed then
          local zlib = require("fan").zlib
          local inflated, ierr = zlib.inflate_raw(body .. "\0\0\xff\xff")
          if not inflated then
            self.closed = true
            return nil, "inflate: " .. tostring(ierr)
          end
          body = inflated
        end
        return body, msg_op
      end
    else
      self.closed = true
      return nil, "protocol error: unknown opcode " .. tostring(f.opcode)
    end
  end
end

----------------------------------------------------------------------
-- Public: accept() from a fan.httpd handler
----------------------------------------------------------------------
-- Perform the server-side WebSocket upgrade. `req` and `resp` come from
-- a fan.httpd handler; on the pure-Lua backend these are the same table
-- and expose the underlying fan.tcp `conn` under `resp.conn`. On the C
-- evhttp backend they are the proxy from fan/httpd.lua whose
-- `req:websocket_accept()` performs the handshake C-side and returns a
-- ws object. We route accordingly so v1 code that says
-- `websocket.accept(req, resp)` works on both backends.
--
-- Returns ws, nil on success; on handshake failure sends a 400 via resp and
-- returns nil, err. After success the caller should treat the response as
-- taken over (do NOT call resp:reply* anymore).
function M.accept(req, resp)
  if not req or not resp then return nil, "accept requires (req, resp)" end

  -- Backend detection: the pure-Lua backend exposes the raw fan.tcp
  -- connection under resp.conn (we write the 101 head there directly).
  -- The C evhttp backend hands us a proxy where .conn is absent; in
  -- that case we route through the C-side handshake which owns the
  -- underlying bufferevent. This shape check also avoids infinite
  -- recursion — the Lua backend's req:websocket_accept() calls
  -- websocket.accept(req, resp) back into this function.
  if not resp.conn and type(req.websocket_accept) == "function" then
    local ws, err = req:websocket_accept()
    if not ws then
      -- Best-effort 400 to match the Lua backend's error shape. The C
      -- backend only fails accept for header mismatches; a plain reply
      -- still works since we haven't sent any bytes yet on the wire.
      pcall(function()
        resp:reply(400, { ["Content-Type"] = "text/plain" },
                   "expected WebSocket upgrade")
      end)
      return nil, err
    end
    return ws, nil
  end

  -- Pure-Lua backend path: req/resp are tables built by httpd_lua on
  -- top of a fan.tcp conn. resp.conn is the raw connection we write
  -- the 101 head onto directly.
  if resp.sent_head then return nil, "accept: response head already sent" end
  local h = req.headers or {}
  local upg = (h["upgrade"] or ""):lower()
  local con = (h["connection"] or ""):lower()
  local key = h["sec-websocket-key"]
  local ver = h["sec-websocket-version"]
  if upg ~= "websocket" or not con:find("upgrade", 1, true) then
    pcall(resp.reply, resp, 400, { ["Content-Type"] = "text/plain" },
          "expected WebSocket upgrade")
    return nil, "not a websocket upgrade request"
  end
  if not key or ver ~= "13" then
    pcall(resp.reply, resp, 400, { ["Content-Type"] = "text/plain" },
          "unsupported WebSocket handshake")
    return nil, "bad websocket key/version"
  end
  local accept = compute_accept(key)

  -- Optional permessage-deflate negotiation (RFC 7692). We accept only when
  -- fan.zlib is compiled in; we always answer with
  --   permessage-deflate; server_no_context_takeover; client_no_context_takeover
  -- because our zlib_wrap primitives are one-shot (no per-connection stream).
  local deflate_enabled = false
  do
    local ext = h["sec-websocket-extensions"] or ""
    if ext ~= "" and fan.zlib and fan.zlib.available and fan.zlib.available() then
      -- header is a comma-separated list of "offers"; accept the first one
      -- that mentions permessage-deflate.
      for offer in ext:gmatch("[^,]+") do
        if offer:lower():find("permessage%-deflate") then
          deflate_enabled = true
          break
        end
      end
    end
  end

  local head_lines = {
    "HTTP/1.1 101 Switching Protocols",
    "Upgrade: websocket",
    "Connection: Upgrade",
    "Sec-WebSocket-Accept: " .. accept,
  }
  if deflate_enabled then
    head_lines[#head_lines + 1] =
      "Sec-WebSocket-Extensions: permessage-deflate; " ..
      "server_no_context_takeover; client_no_context_takeover"
  end
  head_lines[#head_lines + 1] = ""
  head_lines[#head_lines + 1] = ""
  local head = table.concat(head_lines, "\r\n")

  -- mark the response as taken over so httpd's serve_one does not add a 204
  resp.sent_head = true
  resp.finished = true
  local ok, err = resp.conn:send(head)
  if not ok then return nil, "handshake send: " .. tostring(err) end
  local ws = new_ws(resp.conn)
  ws.deflate_enabled = deflate_enabled
  return ws, nil
end

return M
