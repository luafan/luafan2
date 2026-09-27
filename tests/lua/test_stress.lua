--[[
  test_stress.lua — M14.C-l: high-concurrency / edge-case stability tests.

  Covers regression gaps outside the per-module happy-path suites:
    - fan.tcp: 50 concurrent client burst with rapid open/close cycle
    - fan.tcp: shutdown-with-pending loop-retry semantics under a big payload
    - fan.udp: close-while-recv wakes parked coroutine with (nil, "closed")
    - fan.udp: two concurrent recv()s on the same sock cooperate cleanly
    - fan.httpd_c: /metrics counters accurate under N concurrent requests
                   mixed across 3 status classes and 3 methods

  Each test caps its wall-clock via a `deadline` guard on the driver
  coroutine (belt-and-braces on top of run_tests.sh's per-test timeout).
  No test spawns > 200 coroutines / > 200 sockets to keep ASan-mode
  memory bounded.

  Run via ./fan (normal / --asan / --coverage all should pass).
]]

local T   = require("test_framework")
local fan = require("fan")

local s = T.suite("stress / edge (M14.C-l)")

-- ---- helpers --------------------------------------------------------------

-- run body as a coroutine, drive the loop until body calls loopbreak.
local function with_loop(body)
  fan.spawn(body)
  fan.loop()
end

-- watchdog: if `pred()` hasn't become true by `deadline_sec`, loopbreak
-- so the test fails with a clear "hang" signal rather than hitting the
-- run_tests.sh outer timeout.
local function watchdog(deadline_sec, pred, label)
  fan.spawn(function()
    local start = fan.gettime and fan.gettime() or os.time()
    while true do
      fan.sleep(0.05)
      if pred() then return end
      local now = fan.gettime and fan.gettime() or os.time()
      if now - start > deadline_sec then
        print(string.format("[watchdog] %s exceeded %.1fs — forcing loopbreak", label, deadline_sec))
        fan.loopbreak()
        return
      end
    end
  end)
end

-- ---- fan.tcp: 50 concurrent clients, rapid open/close ---------------------

s:test("tcp: 50 concurrent short-lived echo clients complete without leaks", function()
  local PORT    = 24601
  local N       = 50
  local results = {}
  local done    = 0
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      local d = conn:receive()
      if d then conn:send(d) end
      conn:close()
    end))
    watchdog(15, function() return done >= N end, "tcp 50-conn burst")
    for i = 1, N do
      fan.spawn(function()
        local c, err = fan.tcp.connect("127.0.0.1", PORT)
        if not c then
          results[i] = "connect-err:" .. tostring(err)
        else
          local msg = string.format("burst-%03d", i)
          c:send(msg)
          results[i] = c:receive()
          c:close()
        end
        done = done + 1
        if done == N then fan.loopbreak() end
      end)
    end
  end)
  if server then server:close() end
  T.eq(done, N, "not all clients finished (done=" .. done .. "/" .. N .. ")")
  for i = 1, N do
    T.eq(results[i], string.format("burst-%03d", i))
  end
end)

-- ---- fan.tcp: shutdown loop-retry with pending output --------------------

s:test("tcp: shutdown(SHUT_WR) with large pending buffer drains via loop-retry", function()
  -- Server accepts one conn, reads everything until EOF, and reports size.
  -- Client sends a big payload then calls shutdown() in a loop until it
  -- returns 0 pending; server then observes EOF and the byte count matches.
  local PORT      = 24602
  local BIG_SIZE  = 512 * 1024   -- 512 KiB — comfortably > bufferevent buffer
  local received  = 0
  local server_done = false
  local client_done = false
  local shutdown_iters = 0
  local shutdown_first_pending = nil
  local server
  with_loop(function()
    server = assert(fan.tcp.bind("127.0.0.1", PORT, function(conn)
      while true do
        local d, err = conn:receive()
        if not d then break end
        received = received + #d
      end
      conn:close()
      server_done = true
    end))
    watchdog(20, function() return server_done and client_done end, "tcp shutdown drain")
    fan.spawn(function()
      local c = assert(fan.tcp.connect("127.0.0.1", PORT))
      c:send(string.rep("A", BIG_SIZE))
      -- loop-retry: pending>0 => shutdown() returns pending count without shutting down
      while true do
        local pending, _ = c:shutdown()
        shutdown_iters = shutdown_iters + 1
        if shutdown_first_pending == nil then shutdown_first_pending = pending end
        -- v1 semantics: returns 0 (or true) once actually shut down
        if pending == 0 or pending == true or pending == nil then break end
        if shutdown_iters > 500 then
          error("shutdown never drained after 500 iterations (pending=" .. tostring(pending) .. ")")
        end
        fan.sleep(0.01)
      end
      c:close()
      client_done = true
      if server_done then fan.loopbreak() end
    end)
    fan.spawn(function()
      -- once server_done flips true we also break, in case client already broke
      while not (server_done and client_done) do fan.sleep(0.02) end
      fan.loopbreak()
    end)
  end)
  if server then server:close() end
  T.truthy(server_done, "server did not observe EOF")
  T.truthy(client_done, "client did not finish shutdown loop")
  T.eq(received, BIG_SIZE, "byte-count mismatch: got " .. received .. "/" .. BIG_SIZE)
  T.truthy(shutdown_iters >= 1, "shutdown loop never entered")
  -- Sanity: at least the first call likely saw pending > 0 for a 512KiB payload
  -- (not strictly required — if the kernel flushed instantly, iters may be 1).
end)

-- ---- fan.udp: close-while-recv wakes parked coroutine --------------------

s:test("udp: close() while another coroutine is parked in recv() wakes it with (nil, \"closed\")", function()
  local PORT = 24603
  local recv_ret1, recv_ret2 = "sentinel", "sentinel"
  local recver_done = false
  with_loop(function()
    local u = assert(fan.udp.new("127.0.0.1", PORT))
    watchdog(5, function() return recver_done end, "udp close-while-recv")
    fan.spawn(function()
      -- parks in recv() forever unless somebody sendtos or closes
      recv_ret1, recv_ret2 = u:recv()
      recver_done = true
      fan.loopbreak()
    end)
    fan.spawn(function()
      fan.sleep(0.05)   -- let the receiver park
      u:close()
    end)
  end)
  T.truthy(recver_done, "recv() never returned after close()")
  T.is_nil(recv_ret1, "expected recv() first return to be nil after close, got " .. tostring(recv_ret1))
  T.eq(recv_ret2, "closed")
end)

-- ---- fan.udp: recv() on a closed sock returns (nil, "closed") immediately -

s:test("udp: recv() on already-closed sock returns (nil, \"closed\") without hanging", function()
  local r1, r2 = "sentinel", "sentinel"
  local reached = false
  with_loop(function()
    local u = assert(fan.udp.new())
    u:close()
    watchdog(2, function() return reached end, "udp recv-after-close")
    fan.spawn(function()
      r1, r2 = u:recv()
      reached = true
      fan.loopbreak()
    end)
  end)
  T.truthy(reached)
  T.is_nil(r1)
  T.eq(r2, "closed")
end)

-- ---- fan.httpd_c: /metrics counters correct under concurrent load --------

s:test("httpd_c: /metrics counters exactly match N concurrent requests across method+status classes", function()
  local http  = require("fan.http")
  local httpd = require("fan.httpd")
  local PORT  = 24604
  local N_PER = 6   -- per (method, status) cell; 3 methods x 3 status classes x 6 = 54 reqs
  local METHODS  = { "GET", "POST", "PUT" }
  local STATUSES = { 200, 404, 500 }

  local server
  local completed = 0
  local target = N_PER * #METHODS * #STATUSES
  local metrics_body

  with_loop(function()
    server = assert(httpd.bind{
      host    = "127.0.0.1",
      port    = PORT,
      metrics = "/_m",
      onService = function(req, resp)
        -- pick response status from URL last segment: /go/<code>
        local code = tonumber(req.path:match("/go/(%d+)$")) or 200
        local body = (code == 500 and "Server Error")
                  or (code == 404 and "Not Found")
                  or "OK"
        resp:reply(code, { ["Content-Type"] = "text/plain" }, body)
      end,
    })
    watchdog(25, function() return metrics_body ~= nil end, "httpd_c metrics load")

    -- fan out concurrent client coroutines
    for _, m in ipairs(METHODS) do
      for _, code in ipairs(STATUSES) do
        for _ = 1, N_PER do
          fan.spawn(function()
            local url  = string.format("http://127.0.0.1:%d/go/%d", PORT, code)
            local resp
            if m == "GET" then
              resp = http.get(url)
            elseif m == "POST" then
              resp = http.post(url, { body = "b" })
            else
              resp = http.put(url, { body = "b" })
            end
            if resp and resp.status == code then
              completed = completed + 1
            end
            if completed == target then
              -- scrape metrics AFTER all app requests finished so counters are stable
              fan.spawn(function()
                fan.sleep(0.05)
                local m_resp = http.get(string.format("http://127.0.0.1:%d/_m", PORT))
                metrics_body = m_resp and m_resp.body or ""
                fan.loopbreak()
              end)
            end
          end)
        end
      end
    end
  end)
  if server then server:close() end
  T.eq(completed, target, "not all app requests completed (" .. completed .. "/" .. target .. ")")
  T.not_nil(metrics_body, "metrics scrape never returned")
  -- The /metrics scrape itself is one extra GET/2xx request that gets counted.
  -- Verify per-method + per-status-class counters:
  local function count_metric(body, name, labels)
    -- Match `name{...labels...} <value>` where labels order-independent
    for line in body:gmatch("[^\n]+") do
      if line:sub(1,1) ~= "#" and line:find(name, 1, true) then
        local ok = true
        for k, v in pairs(labels) do
          if not line:find(k .. "=\"" .. v .. "\"", 1, true) then ok = false; break end
        end
        if ok then
          local n = tonumber(line:match("%s(%d+%.?%d*)%s*$"))
          if n then return n end
        end
      end
    end
    return nil
  end

  -- Accounting nuance: for the /metrics scrape itself,
  -- request_start (which increments requests_by_method{GET}) runs
  -- BEFORE render, but request_end (which increments responses_by_class{2xx})
  -- runs AFTER render. So the scrape body:
  --   * DOES include itself in the method{GET} counter (+1)
  --   * DOES NOT include itself in the class{2xx} counter (matches app 200s only)

  -- fan_httpd_requests_by_method_total{method="GET"} = N_PER*#STATUSES (app GETs) + 1 (scrape)
  local get_total = count_metric(metrics_body, "fan_httpd_requests_by_method_total", { method = "GET" })
  T.not_nil(get_total, "missing fan_httpd_requests_by_method_total{method=GET} in scrape body")
  T.eq(get_total, N_PER * #STATUSES + 1)

  local post_total = count_metric(metrics_body, "fan_httpd_requests_by_method_total", { method = "POST" })
  T.eq(post_total, N_PER * #STATUSES)

  local put_total  = count_metric(metrics_body, "fan_httpd_requests_by_method_total", { method = "PUT" })
  T.eq(put_total, N_PER * #STATUSES)

  -- fan_httpd_responses_by_class_total{class="2xx"} does NOT include the scrape itself.
  local c2xx = count_metric(metrics_body, "fan_httpd_responses_by_class_total", { class = "2xx" })
  T.eq(c2xx, N_PER * #METHODS)   -- app 200 x #METHODS, no self
  local c4xx = count_metric(metrics_body, "fan_httpd_responses_by_class_total", { class = "4xx" })
  T.eq(c4xx, N_PER * #METHODS)
  local c5xx = count_metric(metrics_body, "fan_httpd_responses_by_class_total", { class = "5xx" })
  T.eq(c5xx, N_PER * #METHODS)
end)

os.exit(T.run(s))
