-- fan/worker.lua — LuaFan v2 process-based worker pool (M6).
--
-- Design (v2, follows docs/luafan-v2-plan.md §1 "sys/posix" + §2):
--   - Master forks N slave processes; each slave gets its own lua_State
--     (fresh, since fork() copies the whole process image but child
--     execution starts from the fork() call site).
--   - Master ↔ slave communication is a TCP loopback socket per slave
--     (the plan's "message passing over TCP/FIFO"). We use fan.tcp so
--     both sides drive the same event loop and the receive side yields
--     the master's caller coroutine.
--   - Wire format is length-prefixed objectbuf frames:
--       frame := u32-length | objectbuf({task_key, fn_name, ...args})
--     Reply: objectbuf({task_key, ok, ...results}). task_key is a
--     monotonically increasing per-slave string.
--   - Master call flow:
--       1. pick least-loaded live slave (round-robin fallback);
--       2. fan_coro_park (yield) the caller into the slave's task map;
--       3. slave receives → dispatches funcs[name] → sends reply;
--       4. master receive coroutine wakes the parked caller with
--          the decoded results.
--   - Slave lifecycle: master keeps slave_pids; :terminate() SIGTERMs
--     each and waitpid()s in a nested spawn. R19-equivalent: closing
--     the master listener while calls are in flight resumes each
--     parked caller with (false, "slave dead").
--   - N<=0 short-circuits to an inline dispatcher (no fork, direct
--     function calls). This is the "single-threaded zero cost" mode
--     called out by the plan.

local fan       = require("fan")
local posix     = fan.posix
local objectbuf = require("fan.objectbuf")

assert(posix, "fan.worker requires fan.posix (M6.1)")

local M = {}

-- ---- Inline dispatcher (slaves == 0) --------------------------------------
local Inline = {}
Inline.__index = Inline
function Inline:call(name, ...)
    local fn = self.funcs[name]
    if not fn then return false, "no such function: " .. tostring(name) end
    local ok, r1, r2, r3 = pcall(fn, ...)
    if not ok then return false, r1 end
    return true, r1, r2, r3
end
function Inline:terminate() end
function Inline:size() return { slaves = 0, inline = true } end
function Inline:wait_slaves() end   -- inline is always "ready"

-- ---- Frame codec ----------------------------------------------------------
-- Wire format: 4-byte big-endian length prefix | objectbuf(payload).
--
-- We don't use fan.stream because the raw string interface of fan.tcp:
-- receive is trivially compatible with a hand-rolled 4-byte header.

local function encode_frame(t)
    local body = objectbuf.encode(t)
    local n = #body
    local hdr = string.char(
        math.floor(n / 16777216) % 256,
        math.floor(n / 65536)    % 256,
        math.floor(n / 256)      % 256,
                    n            % 256)
    return hdr .. body
end

-- Try to pull one frame from a receive-buffer string. Returns
-- (decoded, consumed_bytes) on success, or (nil, need_more_bytes) when
-- the buffer doesn't yet hold a complete frame, or (nil, nil, err) on
-- decode error.
local function try_decode_from_buf(buf)
    local n = #buf
    if n < 4 then return nil, 4 - n end
    local a, b, c, d = buf:byte(1, 4)
    local len = a * 16777216 + b * 65536 + c * 256 + d
    if n < 4 + len then return nil, 4 + len - n end
    local body = buf:sub(5, 4 + len)
    local ok, decoded = pcall(objectbuf.decode, body)
    if not ok then return nil, nil, decoded end
    return decoded, 4 + len
end

-- ---- Slave runtime (in the child process) ---------------------------------
local function slave_main(funcs, master_url)
    -- child branch: fork already reinit'd the event loop for us. Connect
    -- back to the master, then serve requests forever.
    local connector = require("fan.connector")

    fan.spawn(function()
        local scheme, rest = master_url:match("^(%w+)://(.*)$")
        local cli
        for _ = 1, 50 do
            local c, err = connector.connect(master_url)
            if c then cli = c; break end
            fan.sleep(0.05)
        end
        if not cli then
            io.stderr:write("worker slave: cannot connect: " .. master_url .. "\n")
            fan.loopbreak()
            return
        end

        local buf = ""
        while true do
            local chunk = cli:receive(1)  -- any bytes available; blocks otherwise
            if not chunk then break end
            buf = buf .. chunk
            while true do
                local args, consumed_or_need, derr =
                    try_decode_from_buf(buf)
                if derr then
                    io.stderr:write("worker slave: decode err " .. tostring(derr) .. "\n")
                    cli:close(); fan.loopbreak(); return
                end
                if not args then break end   -- need more bytes
                buf = buf:sub(consumed_or_need + 1)

                local task_key = args[1]
                local fname    = args[2]
                local fn       = funcs[fname]
                local reply
                if not fn then
                    reply = { task_key, false,
                              "no such function: " .. tostring(fname) }
                else
                    local nargs = #args
                    local ok, r1, r2, r3 =
                        pcall(fn, table.unpack(args, 3, nargs))
                    if ok then reply = { task_key, true, r1, r2, r3 }
                    else       reply = { task_key, false, r1 } end
                end
                cli:send(encode_frame(reply))
            end
        end
        if cli then cli:close() end
        fan.loopbreak()
    end)

    fan.loop()
    os.exit(0)
end

-- ---- Master runtime -------------------------------------------------------
local Master = {}
Master.__index = Master

local function _new_task_key(slave)
    slave.task_index = slave.task_index + 1
    return string.format("%d", slave.task_index)
end

function Master:_least_loaded_slave()
    -- pick a live slave with min pending, break ties by round-robin.
    local best, best_load
    for _, sl in ipairs(self.slaves) do
        if sl.status == "running" then
            local load = 0
            for _ in pairs(sl.task_map) do load = load + 1 end
            if not best or load < best_load then
                best, best_load = sl, load
            end
        end
    end
    return best
end

function Master:call(name, ...)
    -- Wait until all slaves have handshaken back.
    if #self.slaves < self.slave_count then
        self:wait_slaves()
    end
    local slave = self:_least_loaded_slave()
    if not slave then return false, "no live slave" end

    local task_key = _new_task_key(slave)
    local frame = encode_frame{task_key, name, ...}

    -- park BEFORE send so a super-fast reply can find our co in task_map
    local co, is_main = coroutine.running()
    if is_main or not co then
        return false, "fan.worker:call() must be called from a coroutine"
    end
    slave.task_map[task_key] = co

    local ok, err = slave:send(frame)
    if not ok then
        slave.task_map[task_key] = nil
        slave.status = "dead"
        return false, "slave send failed: " .. tostring(err)
    end
    return coroutine.yield()
end

function Master:wait_slaves()
    if #self.slaves == self.slave_count then return end
    local co = coroutine.running()
    self._wait_cos = self._wait_cos or {}
    table.insert(self._wait_cos, co)
    return coroutine.yield()
end

function Master:size()
    local live = 0
    for _, s in ipairs(self.slaves) do
        if s.status == "running" then live = live + 1 end
    end
    return { slaves = self.slave_count, live = live, inline = false }
end

function Master:terminate()
    -- Signal every slave to exit. We use SIGTERM first (giving them a
    -- chance to close cleanly), then wait a bit and SIGKILL any that
    -- didn't reap. Reaping is done via WNOHANG-poll + fan.sleep so we
    -- don't block the event loop.
    for _, pid in ipairs(self.slave_pids) do
        pcall(posix.kill, pid, posix.signals.SIGTERM)
    end
    if self.serv and self.serv.close then pcall(self.serv.close, self.serv) end

    -- reap loop (non-blocking)
    local wnohang = posix.wait and posix.wait.WNOHANG or 1
    local pending = {}
    for _, pid in ipairs(self.slave_pids) do pending[pid] = true end
    local deadline = os.time() + 2
    while next(pending) and os.time() < deadline do
        for pid in pairs(pending) do
            local ok, rr = pcall(posix.waitpid, pid, wnohang)
            if ok and rr and rr > 0 then pending[pid] = nil end
        end
        if next(pending) then fan.sleep(0.02) end
    end
    -- last-resort SIGKILL for any survivor
    for pid in pairs(pending) do
        pcall(posix.kill, pid, posix.signals.SIGKILL, true)
        pcall(posix.waitpid, pid, wnohang)
    end
    self.slave_pids = {}

    -- close any live slave conn + fail parked callers
    for _, s in ipairs(self.slaves) do
        if s.status == "running" then s.status = "dead"; s:close() end
        for task_key, co in pairs(s.task_map) do
            s.task_map[task_key] = nil
            local ok, err = coroutine.resume(co, false, "slave terminated")
            if not ok then io.stderr:write("worker terminate resume: " .. tostring(err) .. "\n") end
        end
    end
end

-- Attach a newly-accepted slave connection.
local function _register_slave(master, apt)
    local slave = {
        conn       = apt,           -- fan.tcp.conn userdata
        status     = "running",
        task_map   = {},
        task_index = 0,
    }
    -- send() / close() convenience so master:call can do slave:send(...)
    function slave:send(data)
        local ok, err = self.conn:send(data)
        if not ok then return false, err end
        return true
    end
    function slave:close() pcall(self.conn.close, self.conn) end

    table.insert(master.slaves, slave)

    fan.spawn(function()
        local buf = ""
        while true do
            local chunk = apt:receive(1)
            if not chunk then break end
            buf = buf .. chunk
            while true do
                local reply, consumed_or_need, derr =
                    try_decode_from_buf(buf)
                if derr then
                    io.stderr:write("worker master decode err: " ..
                                    tostring(derr) .. "\n")
                    buf = ""; break
                end
                if not reply then break end
                buf = buf:sub(consumed_or_need + 1)

                local task_key = reply[1]
                local co = slave.task_map[task_key]
                if co then
                    slave.task_map[task_key] = nil
                    local ok, err = coroutine.resume(co,
                        reply[2], reply[3], reply[4], reply[5])
                    if not ok then
                        io.stderr:write("worker resume: " ..
                                        tostring(err) .. "\n")
                    end
                end
            end
        end
        slave.status = "dead"
        for task_key, co in pairs(slave.task_map) do
            slave.task_map[task_key] = nil
            coroutine.resume(co, false, "slave dead")
        end
    end)

    if master._wait_cos and #master.slaves == master.slave_count then
        local cos = master._wait_cos
        master._wait_cos = nil
        for _, co in ipairs(cos) do
            local ok, err = coroutine.resume(co)
            if not ok then
                io.stderr:write("worker wait_slaves resume: "
                              .. tostring(err) .. "\n")
            end
        end
    end
end

-- ---- Entry point ----------------------------------------------------------
function M.new(opts)
    opts = opts or {}
    local funcs = opts.funcs or {}
    assert(type(funcs) == "table", "worker.new: `funcs` must be a table")

    local n = opts.slaves
    if n == nil then
        local cc = posix.getcpucount and posix.getcpucount() or 1
        n = math.max(1, (cc or 1) - 1)
    end

    if n <= 0 then
        return setmetatable({ funcs = funcs }, Inline)
    end

    -- Master forks N slaves. To keep things predictable we bind the master
    -- socket BEFORE forking so slaves see the address in memory (and the
    -- listening fd, though slaves will close it below).
    local port = opts.port or 0
    local host = opts.host or "127.0.0.1"

    local master = setmetatable({
        funcs       = funcs,
        slave_count = n,
        slaves      = {},
        slave_pids  = {},
        _wait_cos   = nil,
    }, Master)

    -- Bind master listener; take note of the actual port before forking.
    local serv, serr = fan.tcp.bind(host, port, function(apt)
        _register_slave(master, apt)
    end)
    if not serv then error("worker master bind: " .. tostring(serr)) end
    master.serv = serv
    local actual_port = serv:getport()
    local url = string.format("tcp://%s:%d", host, actual_port)
    master.url = url

    for _ = 1, n do
        local pid, err = posix.fork()
        if pid == nil then error("worker fork: " .. tostring(err)) end
        if pid == 0 then
            -- child: close inherited master listener (best-effort), run slave loop
            if master.serv and master.serv.close then master.serv:close() end
            slave_main(funcs, url)
            os.exit(0)  -- defensive: slave_main should already have exited
        else
            table.insert(master.slave_pids, pid)
        end
    end

    return master
end

return M
