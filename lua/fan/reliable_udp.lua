--[[
  fan/reliable_udp.lua — LuaFan v2 reliable UDP transport.

  A self-contained reliable message layer built on top of fan.udp. It provides
  ordered, reliable, message-oriented delivery over an unreliable datagram
  socket, with:

    * fragmentation + reassembly of messages larger than one datagram,
    * per-frame sequence numbers with a sliding send/receive window,
    * cumulative + selective (bitmask) acknowledgements,
    * timeout-based retransmission of unacked frames,
    * out-of-order tolerance and duplicate suppression,
    * window-based flow control (send blocks when the window is full),
    * a session registry with idle-expiry sweeping.

  Design goals vs. v1's modules/fan/reliable_udp.lua: v1 was a bag of protocol
  primitives tightly coupled to fan.udpd's connection object and could not run
  or be tested on its own. This rewrite is a single, coherent, independently
  testable module: two sessions over a loopback fan.udp socket exchange data
  end-to-end with no external driver.

  Wire format (little-endian, via string.pack "<"):
    DATA:  I1 type=1 | I4 seq | I2 frag_total | I2 frag_idx | payload
    ACK:   I1 type=2 | I4 ack_base | I4 sack_bits
    FIN:   I1 type=3
  ack_base is the next in-order sequence the receiver still needs; sack_bits
  marks frames received out of order in [ack_base+1 .. ack_base+32].

  API:
    local ru = require "fan.reliable_udp"
    local reg = ru.new_registry(sock [, opts])   -- sock is a fan.udp socket
    local sess = reg:session(host, port)          -- get/create a peer session
    sess:send(message)                            -- reliable, ordered (yields on full window)
    local msg = sess:recv()                       -- next complete message (yields)
    reg:pump()                                    -- drive the socket recv loop (spawn this)
    reg:tick()                                    -- run one retransmit/expiry pass (spawn a ticker)
    sess:close()                                  -- send FIN + drop session
    reg:sweep()                                   -- drop idle-expired sessions

  All blocking calls must run inside fan coroutines (fan.spawn / fan.loop).
]]

local fan = require "fan"

local string_pack, string_unpack = string.pack, string.unpack
local string_sub = string.sub
local table_concat = table.concat
local floor = math.floor

local TYPE_DATA = 1
local TYPE_ACK  = 2
local TYPE_FIN  = 3

local DATA_HEAD = 1 + 4 + 2 + 2  -- type + seq + frag_total + frag_idx
local ACK_LEN   = 1 + 4 + 4

-- Defaults; overridable via registry opts.
local DEFAULT_OPTS = {
  mtu          = 1200,   -- max datagram payload (safe under common path MTU)
  window       = 16,     -- sliding window size in frames
  rto          = 0.2,    -- retransmit timeout (seconds)
  max_retries  = 8,      -- give up a frame after this many resends
  idle_ttl     = 30,     -- session idle expiry (seconds)
}

local SEQ_MOD = 0x100000000  -- sequence numbers are u32; wrap-safe compares below

-- Wrap-safe "is a strictly after b" for u32 sequence space.
local function seq_gt(a, b)
  return ((a - b) % SEQ_MOD) < (SEQ_MOD / 2) and a ~= b
end
local function seq_ge(a, b)
  return a == b or seq_gt(a, b)
end

----------------------------------------------------------------------
-- Session
----------------------------------------------------------------------
local Session = {}
Session.__index = Session

local function new_session(reg, host, port)
  local now = fan.gettime()
  local s = setmetatable({
    reg          = reg,
    host         = host,
    port         = port,
    -- send side
    send_base    = 0,           -- oldest unacked seq
    send_next    = 0,           -- next seq to assign
    unacked      = {},          -- seq -> { frame=, sent_at=, retries= }
    -- receive side
    recv_base    = 0,           -- cumulative ACK boundary (all below received)
    deliver_base = 0,           -- next seq awaiting message assembly/delivery
    received     = {},          -- seq -> true (frames seen, for recv_base/SACK)
    recv_buf     = {},          -- seq -> { frag_total=, frag_idx=, payload= }
    msg_queue    = {},          -- assembled messages ready for recv()
    -- lifecycle
    last_activity = now,
    closed        = false,
    finished      = false,      -- peer sent FIN
  }, Session)
  return s
end

function Session:_touch()
  self.last_activity = fan.gettime()
end

-- Raw send of a framed datagram to the peer.
function Session:_emit(frame)
  local ok = self.reg.sock:sendto(frame, self.host, self.port)
  self:_touch()
  return ok
end

-- Build + send an ACK for the current receive state.
function Session:_send_ack()
  local bits = 0
  for i = 1, 32 do
    local seq = (self.recv_base + i) % SEQ_MOD
    if self.received[seq] then
      bits = bits | (1 << (i - 1))
    end
  end
  self:_emit(string_pack("<I1I4I4", TYPE_ACK, self.recv_base, bits))
end

-- Fragment `message` into frames and queue them for reliable delivery.
-- Blocks (via fan.sleep) while the send window is full (flow control). Using
-- fan.sleep — not a raw coroutine.yield resumed elsewhere — keeps this session
-- built entirely on fan's coroutine primitives, so it composes with fan.udp
-- recv() and fan.sleep without cross-resume hazards.
function Session:send(message)
  if self.closed then return nil, "closed" end
  assert(type(message) == "string", "message must be a string")

  local body = self.reg.mtu - DATA_HEAD
  local total = floor((#message + body - 1) / body)
  if total < 1 then total = 1 end  -- allow empty message as a single frame
  assert(total <= 0xFFFF, "message too large")

  local poll = math.max(self.reg.rto / 8, 0.005)
  for idx = 1, total do
    -- flow control: wait until the window has room
    while ((self.send_next - self.send_base) % SEQ_MOD) >= self.reg.window do
      if self.closed then return nil, "closed" end
      fan.sleep(poll)
    end

    local seq = self.send_next
    self.send_next = (self.send_next + 1) % SEQ_MOD
    local part = string_sub(message, (idx - 1) * body + 1, idx * body)
    local frame = string_pack("<I1I4I2I2", TYPE_DATA, seq, total, idx) .. part
    self.unacked[seq] = {
      frame = frame, sent_at = fan.gettime(), retries = 0,
    }
    self:_emit(frame)
  end
  return true
end

-- Deliver an assembled message. recv() polls msg_queue via fan.sleep, so we
-- simply enqueue here; no cross-coroutine resume is performed (that would clash
-- with fan's C-level coroutine park/wake used by fan.udp recv / fan.sleep).
function Session:_deliver(msg)
  self.msg_queue[#self.msg_queue + 1] = msg
end

-- Advance the cumulative ACK boundary (recv_base) over every contiguous frame
-- we have received. This is independent of message assembly: recv_base marks
-- "all frames below this are received", which is what the sender's sliding
-- window needs to advance. Frames stay buffered for assembly.
function Session:_advance_recv_base()
  while self.received[self.recv_base] do
    -- consumed: drop the membership marker so `received` cannot grow without
    -- bound over a long-lived session. Duplicate detection below recv_base
    -- relies on seq_gt(recv_base, seq), not on this set.
    self.received[self.recv_base] = nil
    self.recv_base = (self.recv_base + 1) % SEQ_MOD
  end
end

-- From deliver_base, assemble and deliver every complete message whose frames
-- are all present. A message starts at a frame with frag_idx == 1 and spans
-- frag_total contiguous frames. This runs behind recv_base so a large message
-- (more frames than the window) still lets recv_base — and thus the sender's
-- window — advance frame by frame, avoiding the assemble/window deadlock.
function Session:_try_assemble()
  while true do
    local pkt = self.recv_buf[self.deliver_base]
    if not pkt then break end
    if pkt.frag_idx ~= 1 then
      -- misaligned start (should not happen with correct sequencing); skip it
      self.recv_buf[self.deliver_base] = nil
      self.deliver_base = (self.deliver_base + 1) % SEQ_MOD
      goto continue
    end
    local total = pkt.frag_total
    local have = true
    for i = 0, total - 1 do
      local seq = (self.deliver_base + i) % SEQ_MOD
      local p = self.recv_buf[seq]
      if not p or p.frag_total ~= total or p.frag_idx ~= i + 1 then
        have = false
        break
      end
    end
    if not have then break end
    local parts = {}
    for i = 0, total - 1 do
      local seq = (self.deliver_base + i) % SEQ_MOD
      parts[i + 1] = self.recv_buf[seq].payload
      self.recv_buf[seq] = nil
    end
    self.deliver_base = (self.deliver_base + total) % SEQ_MOD
    self:_deliver(table_concat(parts))
    ::continue::
  end
end

-- Handle an incoming DATA frame.
function Session:_on_data(seq, frag_total, frag_idx, payload)
  self:_touch()
  -- Already consumed (seq is before recv_base): re-ACK so the sender advances,
  -- then drop the duplicate.
  if seq_gt(self.recv_base, seq) then
    self:_send_ack()
    return
  end
  -- Far outside the receive window: drop silently; the sender will retransmit
  -- once its window slides forward.
  local offset = (seq - self.recv_base) % SEQ_MOD
  if offset >= self.reg.window + 32 then
    return
  end
  if not self.received[seq] then
    self.received[seq] = true
    self.recv_buf[seq] = { frag_total = frag_total, frag_idx = frag_idx, payload = payload }
  end
  self:_advance_recv_base()
  self:_try_assemble()
  self:_send_ack()
end

-- Handle an incoming ACK frame.
function Session:_on_ack(ack_base, sack_bits)
  self:_touch()
  -- Cumulative: everything before ack_base is acknowledged.
  while seq_gt(ack_base, self.send_base) do
    self.unacked[self.send_base] = nil
    self.send_base = (self.send_base + 1) % SEQ_MOD
  end
  -- Selective: clear individually-acked frames in the SACK window.
  for i = 1, 32 do
    if (sack_bits & (1 << (i - 1))) ~= 0 then
      local seq = (ack_base + i) % SEQ_MOD
      self.unacked[seq] = nil
    end
  end
  -- send() polls the window via fan.sleep and observes the advanced send_base.
end

-- Dispatch a raw datagram received for this session.
function Session:_on_frame(data)
  if #data < 1 then return end
  local t = string_unpack("<I1", data)
  if t == TYPE_DATA then
    if #data < DATA_HEAD then return end
    local _, seq, frag_total, frag_idx = string_unpack("<I1I4I2I2", data)
    local payload = string_sub(data, DATA_HEAD + 1)
    self:_on_data(seq, frag_total, frag_idx, payload)
  elseif t == TYPE_ACK then
    if #data < ACK_LEN then return end
    local _, ack_base, sack_bits = string_unpack("<I1I4I4", data)
    self:_on_ack(ack_base, sack_bits)
  elseif t == TYPE_FIN then
    -- Peer is done sending; recv() observes `finished` on its next poll and
    -- returns nil,"eof" once the message queue drains.
    self.finished = true
    self:_touch()
  end
end

-- Retransmit timed-out unacked frames; returns true if anything was resent.
function Session:_retransmit(now)
  local resent = false
  -- Snapshot the keys first: _emit may (in tightly-coupled test harnesses, or
  -- via a synchronously-delivered ACK) mutate self.unacked, and mutating a
  -- table mid-`pairs` raises "invalid key to 'next'". Iterating a captured key
  -- list and re-fetching each entry is safe against concurrent deletion.
  local seqs = {}
  for seq in pairs(self.unacked) do seqs[#seqs + 1] = seq end
  for _, seq in ipairs(seqs) do
    local u = self.unacked[seq]
    if u and now - u.sent_at >= self.reg.rto then
      if u.retries >= self.reg.max_retries then
        -- give up on this frame; surface the error to the registry
        self.unacked[seq] = nil
        if self.reg.on_error then
          self.reg.on_error("frame " .. seq .. " exceeded max_retries")
        end
      else
        u.retries = u.retries + 1
        u.sent_at = now
        self:_emit(u.frame)
        resent = true
      end
    end
  end
  return resent
end

-- Block until the next complete message arrives (or the peer FINs). Polls the
-- assembled-message queue via fan.sleep so it composes cleanly with fan's
-- coroutine scheduler (pump/tick run in sibling coroutines).
function Session:recv()
  local poll = math.max(self.reg.rto / 8, 0.005)
  while true do
    if #self.msg_queue > 0 then
      return table.remove(self.msg_queue, 1)
    end
    if self.finished then return nil, "eof" end
    if self.closed then return nil, "closed" end
    fan.sleep(poll)
  end
end

function Session:close()
  if self.closed then return end
  self.closed = true
  -- best-effort FIN so the peer can stop waiting
  pcall(function() self:_emit(string_pack("<I1", TYPE_FIN)) end)
  self.reg.sessions[self.host .. "\0" .. self.port] = nil
end

----------------------------------------------------------------------
-- Registry
----------------------------------------------------------------------
local Registry = {}
Registry.__index = Registry

-- new_registry(sock [, opts]) — sock is a fan.udp socket (already bound for a
-- server, or unbound for a pure client). opts overrides DEFAULT_OPTS.
local function new_registry(sock, opts)
  opts = opts or {}
  local r = setmetatable({
    sock        = sock,
    sessions    = {},   -- "host\0port" -> Session
    mtu         = opts.mtu or DEFAULT_OPTS.mtu,
    window      = opts.window or DEFAULT_OPTS.window,
    rto         = opts.rto or DEFAULT_OPTS.rto,
    max_retries = opts.max_retries or DEFAULT_OPTS.max_retries,
    idle_ttl    = opts.idle_ttl or DEFAULT_OPTS.idle_ttl,
    on_error    = opts.on_error,     -- optional function(err)
    on_session  = opts.on_session,   -- optional function(sess) for new inbound peers
    running     = false,
  }, Registry)
  return r
end

local function key_of(host, port) return host .. "\0" .. port end

function Registry:session(host, port)
  local k = key_of(host, port)
  local s = self.sessions[k]
  if not s then
    s = new_session(self, host, port)
    self.sessions[k] = s
  end
  return s
end

-- Drive the socket: receive datagrams and dispatch them to sessions. Spawn this
-- in a coroutine; it runs until the registry is stopped or the socket closes.
function Registry:pump()
  self.running = true
  while self.running do
    local data, host, port = self.sock:recv()
    if not data then break end  -- socket closed / error
    local k = key_of(host, port)
    local s = self.sessions[k]
    if not s then
      s = new_session(self, host, port)
      self.sessions[k] = s
      if self.on_session then
        local ok, err = pcall(self.on_session, s)
        if not ok and self.on_error then self.on_error(err) end
      end
    end
    s:_on_frame(data)
  end
  self.running = false
end

function Registry:stop()
  self.running = false
end

-- Run one retransmission + idle-expiry pass across all sessions.
function Registry:tick()
  local now = fan.gettime()
  -- Snapshot sessions: _retransmit -> _emit may add/remove sessions (e.g. a
  -- test harness that delivers synchronously, or a close during send).
  local list = {}
  for _, s in pairs(self.sessions) do list[#list + 1] = s end
  for _, s in ipairs(list) do
    if not s.closed then s:_retransmit(now) end
  end
  self:sweep(now)
end

-- Drop sessions idle longer than idle_ttl and with no pending work.
function Registry:sweep(now)
  now = now or fan.gettime()
  local drop = {}
  for k, s in pairs(self.sessions) do
    local idle = now - s.last_activity
    local busy = next(s.unacked) ~= nil or #s.msg_queue > 0
    if idle >= self.idle_ttl and not busy then
      drop[#drop + 1] = k
    end
  end
  for _, k in ipairs(drop) do
    self.sessions[k] = nil
  end
  return #drop
end

-- Convenience: run a background ticker coroutine that calls tick() every
-- `interval` seconds until the registry stops. Call inside fan.spawn.
function Registry:run_ticker(interval)
  interval = interval or (self.rto / 2)
  self.running = true
  while self.running do
    self:tick()
    fan.sleep(interval)
  end
end

return {
  new_registry = new_registry,
  Session      = Session,
  Registry     = Registry,
  DEFAULT_OPTS = DEFAULT_OPTS,
  TYPE_DATA    = TYPE_DATA,
  TYPE_ACK     = TYPE_ACK,
  TYPE_FIN     = TYPE_FIN,
  seq_gt       = seq_gt,
  seq_ge       = seq_ge,
}
