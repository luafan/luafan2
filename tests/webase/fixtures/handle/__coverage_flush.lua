-- __coverage_flush.lua — test-fixture handle that lets test_webase.lua
-- flush luacov stats and gracefully stop the webase child before its
-- popen wrapper hits it with SIGTERM.
--
-- Rationale: fan.popen's close() sends SIGTERM and (after ~10 ms)
-- SIGKILL. luacov flushes stats via an anchor's __gc plus an os.exit
-- hook, neither of which run when a signal terminates the process.
-- Without an in-band shutdown path, every webase-child line executed
-- during the run is lost to coverage. This handle:
--   1. calls luacov.runner.save_stats() to persist what we have,
--   2. schedules fan.loopbreak() so fan.loop() returns and core.lua's
--      shutdown hooks + lua_close (which would also flush via __gc)
--      run naturally.
-- Only enabled when the child was launched with LUAFAN_COVERAGE=1 so a
-- non-coverage run doesn't accidentally expose a shutdown endpoint.

local fan = require "fan"

route = "/__coverage_flush"

local ENABLED = os.getenv("LUAFAN_COVERAGE") == "1"

function onGet(req, resp)
  if not ENABLED then
    return resp:reply(404, "Not Found", "not enabled")
  end
  local remote = req.remote_addr or req.headers["X-Real-IP"] or ""
  if remote ~= "127.0.0.1" and remote ~= "::1" then
    return resp:reply(403, "Forbidden", "localhost only")
  end

  -- Flush now, in case fan.loop teardown or lua_close skips the hook
  -- (belt-and-braces: luacov's __gc will run again on exit but calling
  -- save_stats explicitly here means we still get data even if the
  -- child is killed between the reply and its exit).
  local ok, runner = pcall(require, "luacov.runner")
  if ok and type(runner.save_stats) == "function" then
    pcall(runner.save_stats)
  end

  -- Reply first so the test client sees 200 before we break the loop.
  resp:reply(200, "OK", "flushed")

  -- Then schedule the loop break. Doing it inside spawn so this
  -- coroutine can return cleanly before the loop tears down.
  fan.spawn(function()
    fan.sleep(0.05)  -- let the response drain to the client
    fan.loopbreak()
  end)
end
