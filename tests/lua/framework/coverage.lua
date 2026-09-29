--[[
  coverage.lua — luacov activation hook for Lua tests.

  Loaded via `./fan -e 'require "coverage"' test_xxx.lua` when
  run_tests.sh --coverage is active. The hook is a no-op unless
  LUAFAN_COVERAGE=1 is set, so it's safe to always preload.

  Design:
    - each test process (./fan test_xxx.lua) writes its stats to the
      same luacov.stats.out file (luacov appends, and the file lives
      under build-coverage/); run_tests.sh clears it before the run.
    - .luacov (project root) supplies include/exclude patterns.
    - luacov's default `require "luacov"` installs a debug hook that
      records executed lines; it flushes stats at process exit via
      an atexit-like mechanism (its `runner`).

  Coroutine caveat:
    luacov patches coroutine.create so that new coroutines start with
    the debug hook set. luafan2's fan.spawn() is a C-level
    lua_newthread + immediate resume that never goes through Lua's
    coroutine.create, so we additionally wrap fan.spawn to install
    the hook on the thread it creates. Without this wrap, code that
    runs inside fan.spawn (which is most of the interesting async
    logic) is invisible to luacov.
]]

if os.getenv("LUAFAN_COVERAGE") == "1" then
  local ok, runner_or_err = pcall(require, "luacov.runner")
  if not ok then
    io.stderr:write("[coverage] luacov.runner not available: "
                    .. tostring(runner_or_err) .. "\n")
    return
  end
  local runner = runner_or_err
  -- Child processes launched by tests (e.g. test_webase spawns a webase
  -- server in a fixtures dir with chdir) don't have `.luacov` in CWD, so
  -- luacov's default lookup would silently use defaults and write stats
  -- to CWD/luacov.stats.out — invisible to the aggregator that only reads
  -- /work/build-coverage/luacov.stats.out. LUAFAN_LUACOV_CONFIG points
  -- luacov at the shared config file (absolute path); it takes an absolute
  -- path to the config file and returns a config table `runner.init`
  -- accepts. When unset, fall back to luacov's built-in .luacov search.
  local cfg_path = os.getenv("LUAFAN_LUACOV_CONFIG")
  local init_arg
  if cfg_path and cfg_path ~= "" then
    -- The config is a Lua chunk that `returns` a settings table; dofile
    -- gives us the table directly, which runner.init accepts.
    local dof_ok, dof = pcall(dofile, cfg_path)
    if dof_ok and type(dof) == "table" then
      init_arg = dof
      -- .luacov's statsfile is written as a project-root-relative path
      -- ("build-coverage/luacov.stats.out"). When a child test process
      -- chdirs before exec (test_webase enters its fixtures directory
      -- so webase can resolve handle/, service/, web/ relative to CWD),
      -- luacov would write stats under CWD/build-coverage/, invisible
      -- to the aggregator that only reads /work/build-coverage/. Rewrite
      -- the relative statsfile to an absolute path anchored at the
      -- config file's directory (which is always /work in-container).
      if type(init_arg.statsfile) == "string"
         and init_arg.statsfile:sub(1, 1) ~= "/" then
        local cfg_dir = cfg_path:match("(.*)/[^/]+$") or "."
        init_arg.statsfile = cfg_dir .. "/" .. init_arg.statsfile
      end
    else
      io.stderr:write("[coverage] failed to load LUAFAN_LUACOV_CONFIG="
                      .. tostring(cfg_path) .. ": "
                      .. tostring(dof) .. "\n")
    end
  end
  local cfg_ok, cfg_err = pcall(runner.init, init_arg)
  if not cfg_ok then
    io.stderr:write("[coverage] luacov init failed: "
                    .. tostring(cfg_err) .. "\n")
    return
  end

  -- Wrap fan.spawn so coroutines it creates carry the luacov hook.
  -- runner.with_luacov(fn) returns a function that sets debug.sethook
  -- on its own thread before calling fn.
  local ok_fan, fan = pcall(require, "fan")
  if ok_fan and type(fan) == "table" and type(fan.spawn) == "function" then
    local raw_spawn = fan.spawn
    fan.spawn = function(fn, ...)
      return raw_spawn(runner.with_luacov(fn), ...)
    end
  end

  -- fan.tcp.bind's accept callback runs in a fresh coroutine created by
  -- the C layer (lua_newthread inside server_accept_cb, see src/net/tcp.c),
  -- which — unlike coroutine.create / fan.spawn — never gets the luacov
  -- debug hook installed. Everything the accept callback triggers (fan.httpd
  -- request parsing, response building, WebSocket framing, …) is therefore
  -- invisible to luacov. Wrap the callback here so it runs with the hook.
  if ok_fan and type(fan) == "table" and type(fan.tcp) == "table"
     and type(fan.tcp.bind) == "function" then
    local raw_bind = fan.tcp.bind
    fan.tcp.bind = function(host, port, cb, opts)
      if type(cb) == "function" then cb = runner.with_luacov(cb) end
      return raw_bind(host, port, cb, opts)
    end
  end

  -- Same story for the C-backed fan.httpd (M14.C-*): httpd_gencb inside
  -- src/net/httpd.c creates a fresh coroutine per request with a raw
  -- lua_newthread. Without wrapping the handler here, every line of the
  -- fan.httpd shim's C-backend proxy (wrap_c_handler + ws_proxy_methods)
  -- and everything the user handler calls disappears from luacov.
  if ok_fan and type(fan) == "table" and type(fan.httpd_c) == "table"
     and type(fan.httpd_c.bind) == "function" then
    local raw_httpd_c_bind = fan.httpd_c.bind
    fan.httpd_c.bind = function(opts)
      if type(opts) == "table" and type(opts.handler) == "function" then
        opts = {
          host = opts.host, port = opts.port,
          ssl = opts.ssl, cert = opts.cert, key = opts.key,
          -- M14.C-j: forward the metrics scrape path option, otherwise
          -- coverage runs of test_httpd_metrics would silently strip
          -- opts.metrics and every /metrics case would fall through to
          -- the user handler.
          metrics = opts.metrics,
          handler = runner.with_luacov(opts.handler),
        }
      end
      return raw_httpd_c_bind(opts)
    end
  end
end
