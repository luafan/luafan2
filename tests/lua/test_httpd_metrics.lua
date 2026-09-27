--[[
  test_httpd_metrics.lua — M14.C-j: fan.httpd_c Prometheus /metrics endpoint
  and fan.httpd.metrics() Lua-side snapshot.

  These tests exercise the C-backend httpd's process-global counter set.
  We drive traffic with fan.http.get / .post from the same process so
  the numbers are deterministic. Counters are process-wide, so we snapshot
  before + after and assert deltas rather than absolute values (other
  test suites in the same fan invocation may have already bumped counts,
  though every fan invocation is a fresh process so we happen to start
  clean; the delta-check style is future-proof for combined runs).
]]
local T    = require("test_framework")
local fan  = require("fan")
local httpd = require("fan.httpd")
local http  = require("fan.http")

local s = T.suite("fan.httpd_c /metrics (M14.C-j)")

local function with_loop(body)
  fan.spawn(body)
  fan.loop()
end

s:test("fan.httpd.metrics returns a full snapshot table", function()
  local m = httpd.metrics()
  T.is_type(m, "table")
  -- All the documented fields must be present as numbers.
  local fields = {
    "uptime_seconds",
    "requests_total", "requests_active",
    "bytes_sent", "bytes_received", "errors_total",
    "connections_total", "keepalive_reused",
    "requests_get", "requests_post", "requests_put",
    "requests_delete", "requests_other",
    "responses_2xx", "responses_3xx", "responses_4xx", "responses_5xx",
  }
  for _, k in ipairs(fields) do
    T.is_type(m[k], "number", "field " .. k)
  end
end)

s:test("counters advance across a request", function()
  local PORT = 25801
  local server
  local before, after
  with_loop(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req) req:reply(200, {}, "hello") end,
    })
    before = httpd.metrics()
    assert(http.get("http://127.0.0.1:" .. PORT .. "/p"))
    assert(http.get("http://127.0.0.1:" .. PORT .. "/q"))
    -- give the C-side reply completion callbacks a tick to run
    fan.sleep(0.02)
    after = httpd.metrics()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.truthy(after.requests_total >= before.requests_total + 2,
    "requests_total delta " ..
    (after.requests_total - before.requests_total))
  T.truthy(after.requests_get   >= before.requests_get   + 2)
  T.truthy(after.responses_2xx  >= before.responses_2xx  + 2)
  T.truthy(after.bytes_sent     >= before.bytes_sent     + 5 + 5)  -- "hello"*2
end)

s:test("errors_total advances on 4xx / 5xx and per-class counters bump", function()
  local PORT = 25802
  local server
  local before, after
  with_loop(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req)
        if req.path == "/notfound" then
          req:reply(404, {}, "no")
        else
          req:reply(500, {}, "kaboom")
        end
      end,
    })
    before = httpd.metrics()
    assert(http.get("http://127.0.0.1:" .. PORT .. "/notfound"))
    assert(http.get("http://127.0.0.1:" .. PORT .. "/oops"))
    fan.sleep(0.02)
    after = httpd.metrics()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.truthy(after.responses_4xx >= before.responses_4xx + 1)
  T.truthy(after.responses_5xx >= before.responses_5xx + 1)
  T.truthy(after.errors_total  >= before.errors_total  + 2)
end)

s:test("per-method counters bump for POST / PUT / DELETE", function()
  local PORT = 25803
  local server
  local before, after
  with_loop(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      handler = function(req) req:reply(200, {}, "ok") end,
    })
    before = httpd.metrics()
    -- POST via verb helper: signature is (url, {body=..., headers=...})
    assert(http.post("http://127.0.0.1:" .. PORT .. "/x", { body = "hi" }))
    -- PUT (kept inside libevent's default allowed-methods set;
    -- OPTIONS/PATCH would be rejected at the wire level as 501 without
    -- an evhttp_set_allowed_methods call, which we intentionally don't
    -- make: the point of this case is to prove requests_put / _delete
    -- classify away from requests_get.
    assert(http.put("http://127.0.0.1:" .. PORT .. "/x", { body = "hi" }))
    assert(http.delete("http://127.0.0.1:" .. PORT .. "/x"))
    fan.sleep(0.02)
    after = httpd.metrics()
    fan.loopbreak()
  end)
  if server then server:close() end
  T.truthy(after.requests_post   >= before.requests_post   + 1,
    "requests_post delta " ..
    (after.requests_post - before.requests_post))
  T.truthy(after.requests_put    >= before.requests_put    + 1)
  T.truthy(after.requests_delete >= before.requests_delete + 1)
end)

s:test("opts.metrics exposes a Prometheus scrape endpoint", function()
  local PORT = 25804
  local server
  local body, status
  with_loop(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      metrics = "/metrics",
      handler = function(req) req:reply(200, {}, "app") end,
    })
    -- Fire an app request first so the scrape shows non-zero counters.
    assert(http.get("http://127.0.0.1:" .. PORT .. "/app"))
    fan.sleep(0.02)
    local resp = assert(http.get("http://127.0.0.1:" .. PORT .. "/metrics"))
    status = resp.status
    body   = resp.body
    fan.loopbreak()
  end)
  if server then server:close() end
  T.eq(status, 200)
  T.is_type(body, "string")
  -- Prometheus HELP/TYPE header lines
  T.truthy(body:find("# HELP fan_httpd_requests_total", 1, true), body)
  T.truthy(body:find("# TYPE fan_httpd_requests_total counter", 1, true))
  T.truthy(body:find("fan_httpd_requests_total %d") ~= nil)
  T.truthy(body:find("fan_httpd_requests_by_method_total{method=\"GET\"}", 1, true))
  T.truthy(body:find("fan_httpd_responses_by_class_total{class=\"2xx\"}", 1, true))
end)

s:test("opts.metrics rejects non-absolute paths", function()
  local server, err
  server, err = httpd.bind{ port = 25805, backend = "c",
                            metrics = "metrics",   -- missing leading '/'
                            handler = function() end }
  T.is_nil(server)
  T.is_type(err, "string")
  T.truthy(err:find("opts.metrics", 1, true), err)
end)

s:test("opts.metrics not set -> /metrics reaches user handler as 404 or app data", function()
  local PORT = 25806
  local server
  local status, body
  with_loop(function()
    server = assert(httpd.bind{
      port = PORT, backend = "c",
      -- no metrics option here
      handler = function(req)
        if req.path == "/metrics" then
          req:reply(418, {}, "im-a-teapot")
        else
          req:reply(200, {}, "app")
        end
      end,
    })
    local resp = assert(http.get("http://127.0.0.1:" .. PORT .. "/metrics"))
    status = resp.status
    body   = resp.body
    fan.loopbreak()
  end)
  if server then server:close() end
  -- The scrape should have hit the *user* handler, not the metrics
  -- endpoint (which is not registered). Proves we haven't accidentally
  -- hard-wired /metrics like v1 did.
  T.eq(status, 418)
  T.eq(body, "im-a-teapot")
end)

os.exit(T.run(s))
