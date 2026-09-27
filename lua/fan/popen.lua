-- fan/popen.lua — v1-style callback shim over the pull-based fan.popen C
-- module. The C layer is `fan.popen`; this file only exists so
-- `require "fan.popen"` returns a table with a `spawn` that mirrors v1's
-- API (onread/onstderr/ondisconnected callbacks).
--
-- If you want the modern pull API (:recv() yields), use `fan.popen`
-- directly. This shim is here for a) migration of v1 code and b) the
-- v1 examples/popen_basic.lua that we run as an integration test.

local fan  = require "fan"
local popen_c = fan.popen

local M = {}

-- Forward the C-level metatable operations users might touch on the raw
-- process handle. We DON'T wrap the handle: callback-mode users only
-- need send/close_stdin/close/getpid/is_alive/set_winsize, which are all
-- methods on the raw C userdata.

function M.spawn(opts)
  assert(type(opts) == "table", "popen.spawn: opts must be a table")
  -- Peel off the v1-only callbacks; everything else passes through.
  local onread         = opts.onread
  local onstderr       = opts.onstderr
  local ondisconnected = opts.ondisconnected

  -- Build a fresh opts table so we don't mutate the caller's.
  local c_opts = {}
  for k, v in pairs(opts) do c_opts[k] = v end
  c_opts.onread         = nil
  c_opts.onstderr       = nil
  c_opts.ondisconnected = nil

  local proc, err = popen_c.spawn(c_opts)
  if not proc then return nil, err end

  -- Only spawn the pump coroutine when at least one callback is set;
  -- otherwise return the raw handle for pull-mode use.
  if onread or onstderr or ondisconnected then
    fan.spawn(function()
      while true do
        local data, which, code = proc:recv()
        if data == nil then
          -- (nil, exit_reason, exit_code)
          if ondisconnected then
            -- v1 signature: ondisconnected(reason_message, exit_code).
            local ok, cberr = pcall(ondisconnected, which or "exit", code)
            if not ok then
              io.stderr:write("[fan.popen] ondisconnected error: ",
                              tostring(cberr), "\n")
            end
          end
          return
        end
        local cb = (which == "stdout") and onread or onstderr
        if cb then
          local ok, cberr = pcall(cb, data)
          if not ok then
            io.stderr:write("[fan.popen] ", which,
                            " callback error: ", tostring(cberr), "\n")
          end
        end
      end
    end)
  end

  return proc
end

-- Passthroughs for anything else callers might touch.
M.spawn_raw = popen_c.spawn  -- unwrapped for tests that want the C shape

return M
