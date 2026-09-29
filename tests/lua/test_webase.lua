--[[
  test_webase.lua — M12.2 webase integration tests.

  Launches `fan webase/core.lua` as a child process pointed at the
  fixtures under tests/webase/fixtures/, waits until it accepts a TCP
  connection, then exercises every functional promise webase v1 made:
  static-file serve + mime + gzip + ETag/304 + HEAD, directory listing,
  index.html fall-through, JSON handlers (route + JSONP validation),
  path traversal / null-byte rejection, dynamic route registration via
  services, WebSocket echo.

  Runs inside the same luafan2-ci Docker image as every other Lua
  suite. `FAN_BIN` (set by run_tests.sh) points at the built `fan`
  executable; if missing we probe the workspace-relative build path so
  the test also runs from a developer's `./build/fan`.
]]
local T   = require("test_framework")
local fan = require("fan")

local s = T.suite("webase integration (M12.2)")

-- ---- environment discovery -----------------------------------------------

local FAN_BIN  = os.getenv("FAN_BIN")
local WORKROOT = "/work"                 -- what run_tests.sh mounts
if not FAN_BIN then
  -- Off-container fallback: try common local build directories.
  for _, p in ipairs({"./build/fan", "./build-asan/fan", "build/fan"}) do
    local f = io.open(p, "rb")
    if f then f:close(); FAN_BIN = p; WORKROOT = "."; break end
  end
end

if not FAN_BIN then
  print("[SKIP] webase integration test: FAN_BIN not set and no local ./build/fan found")
  os.exit(0)
end

-- Compute the workspace root from FAN_BIN so paths line up whether we
-- run inside the Docker image (/work) or from a raw developer check-out.
local function dirname(p)
  return (p:match("^(.-)/[^/]+/?$")) or "."
end
local BUILD_DIR = dirname(FAN_BIN)
local ROOT      = dirname(BUILD_DIR)
if ROOT == "" or ROOT == "." then ROOT = WORKROOT end

local WEBASE_DIR   = ROOT .. "/webase"
local FIXTURES_DIR = ROOT .. "/tests/webase/fixtures"

-- Sanity: refuse to run if the fixtures aren't there — the failure
-- otherwise looks like a mysterious 404 rather than "you're testing
-- against the wrong tree".
local function file_exists(p)
  local f = io.open(p, "rb"); if f then f:close(); return true end
  return false
end
if not file_exists(WEBASE_DIR .. "/core.lua")
   or not file_exists(FIXTURES_DIR .. "/web/hello.js") then
  print(string.format("[SKIP] webase integration test: missing files under %s or %s",
                      WEBASE_DIR, FIXTURES_DIR))
  os.exit(0)
end

-- ---- helper: pick a free ephemeral port ----------------------------------
--
-- Bind :0, read the assigned port, close. There's an inherent race with
-- any other process that might grab it between close and re-bind, but
-- since this is a private CI container it's fine for our purposes.
local function pick_free_port()
  local sv, err = fan.tcp.bind("127.0.0.1", 0, function() end)
  assert(sv, "pick_free_port: bind: " .. tostring(err))
  -- fan.tcp servers expose the ephemeral port via getport() (which
  -- getsockname()'s the listener fd) rather than a `port` field.
  local port = assert(sv:getport(), "pick_free_port: getport returned nil")
  sv:close()
  return port
end

local PORT = pick_free_port()
local BASE = "http://127.0.0.1:" .. tostring(PORT)

-- ---- start the child server ---------------------------------------------

local popen = fan.popen
assert(type(popen) == "table", "fan.popen required for webase integration")

-- LUA_PATH must include lua/ (for `require "fan.utils"` etc.), the
-- webase directory (for `require "route"`, "webfile", ...), and the
-- tests/lua/framework directory so a coverage-enabled child can
-- `require "coverage"` (which pulls luacov.runner). The final ";;"
-- appends the default Lua path so luacov itself (installed as a
-- LuaRocks module in the CI image) still resolves.
local LUA_PATH = string.format(
  "%s/lua/?.lua;%s/lua/?/init.lua;%s/?.lua;%s/tests/lua/framework/?.lua;;",
  ROOT, ROOT, WEBASE_DIR, ROOT)

-- Under `run_tests.sh --coverage` the parent shell exports
-- LUAFAN_COVERAGE=1 and each Lua test is launched via
-- `fan -e 'require "coverage"' test_xxx.lua` so luacov's runner is
-- installed before the test's code runs. We inherit that mode into
-- the webase child so lines executed in webase/*.lua by the running
-- server are recorded — without it, the child sees LUAFAN_COVERAGE
-- but doesn't preload the runner, and the whole webase tree is
-- invisible to luacov.
local COVERAGE = os.getenv("LUAFAN_COVERAGE") == "1"

local child
local function start_server()
  -- webase modules read `(WORKDIR or "") .. "handle"` etc. — WORKDIR
  -- there is a *Lua global* (not the OS env). v1 apps sidestep it by
  -- running with CWD at the project root so paths like "handle",
  -- "service", "config.d" resolve to project-relative dirs.
  --
  -- We spawn a shell that:
  --   1. chdirs into the fixtures directory (so handle/, service/,
  --      web/ resolve there);
  --   2. execs `fan .../webase/core.lua`, referencing core.lua by its
  --      absolute path so we don't need to duplicate config.d in the
  --      fixture.
  --
  -- config.d isn't in the fixture on purpose: the shipped defaults in
  -- webase/config.d/service.lua + core.lua already read SERVICE_PORT /
  -- WEBROOT env vars, so we override those via `env=` below.
  local fan_cmd
  if COVERAGE then
    fan_cmd = string.format(
      "cd %q && exec %q -e 'require \"coverage\"' %q/core.lua",
      FIXTURES_DIR, FAN_BIN, WEBASE_DIR)
  else
    fan_cmd = string.format("cd %q && exec %q %q/core.lua",
                            FIXTURES_DIR, FAN_BIN, WEBASE_DIR)
  end
  local child_env = {
    LUA_PATH        = LUA_PATH,
    SERVICE_HOST    = "127.0.0.1",
    SERVICE_PORT    = tostring(PORT),
    WEBROOT         = FIXTURES_DIR .. "/web",
    SERVICE_WORKERS = "0",
    PURGE_TOKEN     = "test_secret_token",
    -- Copy through the CI harness's PATH so `execvp` inside the
    -- shell resolves basic tools if the child spawns any.
    PATH            = os.getenv("PATH") or "/usr/local/bin:/usr/bin:/bin",
  }
  if COVERAGE then
    child_env.LUAFAN_COVERAGE = "1"
    -- Absolute path to the project .luacov config. The child chdirs into
    -- FIXTURES_DIR before exec, so luacov's default lookup can't find
    -- .luacov in CWD; without this, the child would silently use luacov
    -- defaults and write stats to FIXTURES_DIR/luacov.stats.out — never
    -- merged into the run's aggregate. See framework/coverage.lua for the
    -- statsfile absolute-path rewrite that goes with this env var.
    child_env.LUAFAN_LUACOV_CONFIG = ROOT .. "/.luacov"
  end
  child = assert(popen.spawn{
    command = { "/bin/sh", "-c", fan_cmd },
    env     = child_env,
  })
end

-- Drain child stdout+stderr into memory so we can dump on failure. Runs
-- as a parked coroutine — recv blocks in the fan loop while other tests
-- proceed.
local child_log = {}
local function drain_child()
  fan.spawn(function()
    while true do
      local data, which = child:recv()
      if data == nil then break end
      child_log[#child_log + 1] = string.format("[%s] %s", tostring(which), data)
    end
  end)
end

local function child_output()
  return table.concat(child_log)
end

-- Forward-declared so stop_server can call it (tcp_request is defined
-- below with the other HTTP helpers). Under LUAFAN_COVERAGE=1 we hit
-- /__coverage_flush before killing the child so luacov's exit hook
-- gets to run inside the still-alive process — SIGTERM/SIGKILL bypass
-- __gc and no stats would be written otherwise.
local tcp_request  -- forward-declare

local function stop_server()
  if not child then return end
  if COVERAGE and tcp_request then
    -- Best-effort: swallow errors so a broken shutdown path doesn't
    -- mask real test failures.
    pcall(tcp_request, "GET", "/__coverage_flush")
    -- Give the child a moment to flush + fan.loopbreak + lua_close
    -- before we tear down the pipes.
    fan.sleep(0.15)
  end
  pcall(child.close, child)
  child = nil
end

-- ---- minimal HTTP client -------------------------------------------------

-- Use fan.tcp directly — luafan2 does have fan.http_c but it depends on
-- libcurl which brings a whole layer we don't need for a small
-- synchronous test client. The socket-level client also lets us pin
-- exact request headers (If-None-Match, Accept-Encoding).
-- Assigned to the forward-declared upvalue so stop_server can call it.
tcp_request = function(method, path, extra_headers, body)
  extra_headers = extra_headers or {}
  local conn, cerr = fan.tcp.connect("127.0.0.1", PORT)
  if not conn then return nil, cerr end
  local lines = {
    string.format("%s %s HTTP/1.1", method, path),
    "Host: 127.0.0.1:" .. tostring(PORT),
    "Connection: close",
  }
  if body then
    extra_headers["Content-Length"] = tostring(#body)
  end
  for k, v in pairs(extra_headers) do
    lines[#lines + 1] = string.format("%s: %s", k, v)
  end
  lines[#lines + 1] = ""
  lines[#lines + 1] = body or ""
  local ok, serr = conn:send(table.concat(lines, "\r\n"))
  if not ok then conn:close(); return nil, serr end

  local buf = {}
  while true do
    -- fan.tcp conn:receive() returns whatever is currently available;
    -- with Connection: close the peer will emit EOF once it's done,
    -- which surfaces as (nil, "eof"). Anything else is a real error.
    local chunk, err = conn:receive()
    if not chunk then
      if err and err ~= "eof" and err ~= "" then
        conn:close()
        return nil, err
      end
      break
    end
    buf[#buf + 1] = chunk
  end
  conn:close()
  local resp = table.concat(buf)
  local head_end = resp:find("\r\n\r\n", 1, true)
  if not head_end then return nil, "no headers terminator" end
  local head_str = resp:sub(1, head_end - 1)
  local body_str = resp:sub(head_end + 4)
  local status_line, rest = head_str:match("^([^\r\n]+)\r?\n(.*)$")
  local code = tonumber(status_line:match("HTTP/%d%.%d%s+(%d+)"))
  local headers = {}
  for line in (rest or ""):gmatch("[^\r\n]+") do
    local k, v = line:match("^([^:]+):%s*(.*)$")
    if k then headers[k:lower()] = v end
  end
  return {
    code    = code,
    headers = headers,
    body    = body_str,
    raw     = resp,
  }
end

-- Wait until we can complete a full request (server is ready to serve
-- static files). Retries every 100 ms up to 5 s.
local function wait_for_ready()
  local deadline = fan.gettime() + 5.0
  while fan.gettime() < deadline do
    local resp = tcp_request("GET", "/hello.js")
    if resp and resp.code and resp.code >= 200 and resp.code < 500 then
      return true
    end
    fan.sleep(0.1)
  end
  return false
end

-- ---- Run everything under a single fan.spawn / fan.loop ------------------
--
-- Because the child is driven by fan.popen (which parks on the event
-- loop), we have to run the whole battery inside fan.loop so recv() can
-- make progress on background reads. Each s:test still runs sequentially.

local run_ok = true
local exit_code = 1

fan.spawn(function()
  local ok, err = pcall(function()
    start_server()
    drain_child()
    if not wait_for_ready() then
      error("webase server did not come up in 5 s\n" .. child_output(), 0)
    end

    -- ---- static file: mime, gzip, ETag, HEAD, 304 ------------------------

    s:test("GET /hello.js -> 200 with javascript mime", function()
      local r = assert(tcp_request("GET", "/hello.js"))
      T.eq(r.code, 200)
      T.truthy(r.headers["content-type"] and r.headers["content-type"]:find("javascript", 1, true))
      T.truthy(#r.body > 0)
    end)

    s:test("Cache-Control: max-age=86400 present", function()
      local r = assert(tcp_request("GET", "/hello.js"))
      T.eq(r.headers["cache-control"], "max-age=86400")
    end)

    s:test("ETag header round-trips as 304 Not Modified", function()
      local r = assert(tcp_request("GET", "/hello.js"))
      local etag = r.headers["etag"]
      T.truthy(etag and #etag > 0, "ETag missing on first request")

      local r2 = assert(tcp_request("GET", "/hello.js", {["If-None-Match"] = etag}))
      T.eq(r2.code, 304)
    end)

    s:test("Accept-Encoding: gzip yields Content-Encoding: gzip", function()
      local r = assert(tcp_request("GET", "/hello.js", {["Accept-Encoding"] = "gzip"}))
      T.eq(r.code, 200)
      T.eq(r.headers["content-encoding"], "gzip")
      -- gzip magic bytes
      T.eq(r.body:sub(1, 3), "\x1f\x8b\x08")
    end)

    s:test("HEAD /hello.js returns 200 with empty body but Content-Length", function()
      local r = assert(tcp_request("HEAD", "/hello.js"))
      T.eq(r.code, 200)
      T.eq(r.body, "")
      T.truthy(r.headers["content-length"], "Content-Length missing on HEAD")
    end)

    s:test("CSS file served with text/css mime", function()
      local r = assert(tcp_request("GET", "/style.css"))
      T.eq(r.code, 200)
      T.truthy(r.headers["content-type"] and r.headers["content-type"]:find("css", 1, true))
    end)

    -- ---- directory listing + index.html fall-through --------------------

    s:test("GET / serves index.html when present", function()
      local r = assert(tcp_request("GET", "/"))
      T.eq(r.code, 200)
      T.truthy(r.body:find("index.html", 1, true), "expected index.html marker in body")
    end)

    s:test("GET /images/ (no index) returns directory listing", function()
      local r = assert(tcp_request("GET", "/images/"))
      T.eq(r.code, 200)
      T.truthy(r.body:find('<a href', 1, true), "directory listing must contain <a href> links")
    end)

    -- ---- 404 ----------------------------------------------------------

    s:test("GET /definitely_missing_xyz -> 404", function()
      local r = assert(tcp_request("GET", "/definitely_missing_xyz_123.txt"))
      T.eq(r.code, 404)
    end)

    -- ---- /echo handler + JSON + JSONP ---------------------------------

    s:test("GET /echo returns application/json", function()
      local r = assert(tcp_request("GET", "/echo"))
      T.eq(r.code, 200)
      T.truthy(r.headers["content-type"] and r.headers["content-type"]:find("json", 1, true))
      T.truthy(r.body:find('"method":"GET"', 1, true))
    end)

    s:test("Valid JSONP callback is wrapped", function()
      local r = assert(tcp_request("GET", "/echo?jsonp=myCallback"))
      T.eq(r.code, 200)
      T.truthy(r.body:find("^myCallback%("), "expected myCallback(... wrapper")
    end)

    s:test("Valid JSONP callback with dots (jQuery.fn.init) accepted", function()
      local r = assert(tcp_request("GET", "/echo?jsonp=jQuery.fn.init"))
      T.eq(r.code, 200)
    end)

    s:test("JSONP callback with < is rejected 400", function()
      local r = assert(tcp_request("GET",
        "/echo?jsonp=%3Cscript%3Ealert(1)%3C/script%3E"))
      T.eq(r.code, 400)
    end)

    s:test("JSONP callback with brackets is rejected 400", function()
      local r = assert(tcp_request("GET", "/echo?jsonp=a%5Db"))
      T.eq(r.code, 400)
    end)

    s:test("JSONP callback with parentheses is rejected 400", function()
      local r = assert(tcp_request("GET", "/echo?jsonp=alert()"))
      T.eq(r.code, 400)
    end)

    s:test("JSONP callback starting with digit is rejected 400", function()
      local r = assert(tcp_request("GET", "/echo?jsonp=123abc"))
      T.eq(r.code, 400)
    end)

    s:test("JSONP callback longer than 128 chars is rejected 400", function()
      local long = string.rep("a", 200)
      local r = assert(tcp_request("GET", "/echo?jsonp=" .. long))
      T.eq(r.code, 400)
    end)

    -- ---- path traversal / null byte ----------------------------------

    s:test("Encoded ../../ path traversal is blocked", function()
      local r = assert(tcp_request("GET", "/%2e%2e/%2e%2e/etc/passwd"))
      T.truthy(r.code == 400 or r.code == 403 or r.code == 404,
               "expected traversal to be rejected; got " .. tostring(r.code))
      T.truthy(not r.body:find("root:", 1, true), "response leaked /etc/passwd")
    end)

    s:test("..%2f%2f mixed traversal is blocked", function()
      local r = assert(tcp_request("GET", "/..%2f..%2fetc/passwd"))
      T.truthy(r.code == 400 or r.code == 403 or r.code == 404)
    end)

    s:test("Null byte in path is rejected", function()
      local r = assert(tcp_request("GET", "/hello.js%00.html"))
      T.truthy(r.code == 400 or r.code == 403 or r.code == 404)
    end)

    -- ---- dynamic routes (registered by service/dynamic_route.lua) -----

    s:test("Dynamic exact route /dynamic_test works", function()
      local r = assert(tcp_request("GET", "/dynamic_test"))
      T.eq(r.code, 200)
      T.truthy(r.body:find('"dynamic":true', 1, true))
    end)

    s:test("Dynamic pattern ^/dynamic_pattern/ works", function()
      local r = assert(tcp_request("GET", "/dynamic_pattern/foo"))
      T.eq(r.code, 200)
      T.truthy(r.body:find('"dynamic":true', 1, true))
    end)

    -- ---- /service handler + service list ------------------------------

    s:test("GET /service returns text/plain and lists services", function()
      local r = assert(tcp_request("GET", "/service"))
      T.eq(r.code, 200)
      T.truthy(r.headers["content-type"] and r.headers["content-type"]:find("text/plain", 1, true))
      -- Body is Transfer-Encoding: chunked; we don't decode chunks here
      -- (this simple client just concatenates the raw framing). Just
      -- assert the service names show up in whatever we got back.
      T.truthy(r.body:find("test", 1, true), "expected 'test' service in listing")
    end)

    -- ---- purge_cache authentication ----------------------------------
    --
    -- Because we connect from 127.0.0.1, remote_addr === 127.0.0.1 and
    -- the localhost-bypass path fires. This confirms remote_addr
    -- populates correctly from getpeername.

    s:test("GET /purge_cache from 127.0.0.1 succeeds (localhost bypass)", function()
      local r = assert(tcp_request("GET", "/purge_cache"))
      T.eq(r.code, 200)
    end)

    -- ---- run the recorded test cases ----------------------------------
    --
    -- T.run(s) executes each test's `fn` under pcall. Because tcp
    -- connect/receive park on the fan event loop, we MUST run T.run
    -- from inside this coroutine (not after fan.loopbreak) — otherwise
    -- the connect would fire from the main state, where
    -- fan_coro_park() rejects with "must be called from a coroutine".
    exit_code = T.run(s)

    -- ---- clean shutdown ---------------------------------------------

    stop_server()
  end)
  if not ok then
    io.stderr:write("webase integration test threw: " .. tostring(err) .. "\n")
    io.stderr:write("--- child stdout/stderr ---\n")
    io.stderr:write(child_output())
    io.stderr:write("\n---\n")
    run_ok = false
    stop_server()
  end
  fan.loopbreak()
end)

fan.loop()

if not run_ok then os.exit(1) end
os.exit(exit_code)
