--[[
  test_posix.lua — M6 fan.posix contract tests.

  Covers: getpid, fork+waitpid, kill (with dangerous-PID guardrails),
  setpgid/getpgid, setsid (inside a forked child; we don't strand the
  test runner), getcpucount, getaffinity/setaffinity (Linux only),
  getinterfaces (IPv4 + IPv6 netmask sanity), setprogname (Linux only,
  no crash / doesn't destroy the environment).
]]
local T   = require("test_framework")
local fan = require("fan")

local posix = fan.posix
if type(posix) ~= "table" then
  print("[SKIP] fan.posix not available on this platform")
  os.exit(0)
end

local s = T.suite("fan.posix (M6)")

s:test("getpid returns a positive integer", function()
  local p = posix.getpid()
  T.is_type(p, "number")
  T.truthy(p > 0)
end)

s:test("getcpucount returns >=1", function()
  local n = posix.getcpucount()
  T.is_type(n, "number")
  T.truthy(n >= 1)
end)

s:test("fork + waitpid: child exits with expected status", function()
  local pid, err = posix.fork()
  T.not_nil(pid, "fork: " .. tostring(err))
  if pid == 0 then
    -- child: exit immediately with code 42
    os.exit(42)
  end
  -- parent: wait
  local r, stat = posix.waitpid(pid, 0)
  T.eq(r, pid)
  T.is_type(stat, "number")
  -- WEXITSTATUS: on Linux, low 8 bits after >> 8
  local exit_code = math.floor(stat / 256) % 256
  T.eq(exit_code, 42)
end)

s:test("kill refuses pid=-1 without force=true", function()
  local ok, err = posix.kill(-1, posix.signals.SIGTERM)
  T.is_nil(ok)
  T.truthy(err); T.truthy(err:find("refused"))
end)

s:test("kill refuses pid=0 without force=true", function()
  local ok, err = posix.kill(0, posix.signals.SIGTERM)
  T.is_nil(ok)
  T.truthy(err); T.truthy(err:find("refused"))
end)

s:test("kill refuses pid=1 (init) without force=true", function()
  local ok, err = posix.kill(1, posix.signals.SIGTERM)
  T.is_nil(ok)
  T.truthy(err); T.truthy(err:find("refused"))
end)

s:test("kill accepts pid=1 with force=true (but we send signal 0 to avoid harm)",
function()
  -- signal 0 is a permission probe; if EPERM, we accept that as "reached the
  -- syscall" (the guardrail did not intercept). The point is kill() no longer
  -- refuses pid=1 when force=true.
  local ok, err = posix.kill(1, 0, {force = true})
  if not ok then
    -- Expected on non-root: EPERM. What matters: NOT the "refused:" guard.
    T.falsy(err:find("refused"), "guard should be bypassed: " .. tostring(err))
  end
end)

s:test("kill of our own process with signal 0 succeeds (permission probe)", function()
  local ok, err = posix.kill(posix.getpid(), 0)
  T.truthy(ok, "kill(self, 0): " .. tostring(err))
end)

s:test("kill child with SIGTERM: waitpid observes signal", function()
  local pid, err = posix.fork()
  T.not_nil(pid, "fork: " .. tostring(err))
  if pid == 0 then
    -- child: sleep for a while, then exit
    while true do os.execute("sleep 10") end
  end
  -- parent
  local ok, kerr = posix.kill(pid, posix.signals.SIGTERM)
  T.truthy(ok, "kill: " .. tostring(kerr))
  local r, stat = posix.waitpid(pid, 0)
  T.eq(r, pid)
  -- WIFSIGNALED: low 7 bits of stat is the signal, no exit code
  local termsig = stat % 128
  -- SIGTERM=15; child may exit via 'sleep' being killed
  T.truthy(termsig == posix.signals.SIGTERM or termsig == posix.signals.SIGKILL,
    "unexpected termsig=" .. tostring(termsig))
end)

s:test("setpgid + getpgid round-trip on the current process", function()
  -- setpgid(0,0) makes the current process the leader of its own group; we
  -- don't do it in the test runner (would detach us from the CI job control
  -- terminal). Instead do it in a forked child and observe via waitpid.
  local pid, err = posix.fork()
  T.not_nil(pid, "fork: " .. tostring(err))
  if pid == 0 then
    local ok = posix.setpgid(0, 0)
    if ok ~= 0 then os.exit(1) end
    local g, gerr = posix.getpgid(0)
    if g ~= posix.getpid() then os.exit(2) end
    os.exit(0)
  end
  local _, stat = posix.waitpid(pid, 0)
  local ec = math.floor(stat / 256) % 256
  T.eq(ec, 0)
end)

s:test("setsid works in a child (creates a new session)", function()
  local pid = posix.fork()
  T.not_nil(pid)
  if pid == 0 then
    -- child needs to setpgid(0,0) first? no: setsid() fails if we're already
    -- the pgroup leader. In the child we're not the leader, so setsid should
    -- succeed and return our own PID.
    local sid = posix.setsid()
    if not sid or sid ~= posix.getpid() then os.exit(3) end
    os.exit(0)
  end
  local _, stat = posix.waitpid(pid, 0)
  T.eq(math.floor(stat / 256) % 256, 0)
end)

s:test("getinterfaces returns loopback with IPv4 host + IPv4 netmask", function()
  local ifs, err = posix.getinterfaces()
  T.not_nil(ifs, "getinterfaces: " .. tostring(err))
  local seen_lo4 = false
  for _, e in ipairs(ifs) do
    if e.type == "inet" and (e.host == "127.0.0.1") then
      seen_lo4 = true
      T.truthy(e.netmask and e.netmask:find("255%.")) -- e.g. 255.0.0.0
    end
  end
  T.truthy(seen_lo4, "loopback IPv4 not found in getinterfaces")
end)

s:test("getinterfaces IPv6 netmask uses IPv6 sockaddr length (contains ':')",
function()
  local ifs = posix.getinterfaces()
  local any6 = false
  for _, e in ipairs(ifs) do
    if e.type == "inet6" then
      any6 = true
      if e.netmask then
        -- IPv6 netmask string always contains ':' (e.g. "ffff:ffff::" or "::")
        T.truthy(e.netmask:find(":"), "IPv6 netmask malformed: " .. e.netmask)
      end
    end
  end
  -- environments without any IPv6 iface: skip silently
  if not any6 then print("    (no IPv6 interface present; skipped shape check)") end
end)

-- Affinity: Linux/Android only.
local uname_h = io.popen and io.popen("uname -s 2>/dev/null")
local uname = uname_h and uname_h:read("*l") or ""
if uname_h then uname_h:close() end

if uname == "Linux" then
  s:test("getaffinity returns a non-zero mask (Linux)", function()
    local m, err = posix.getaffinity()
    T.not_nil(m, "getaffinity: " .. tostring(err))
    T.truthy(m > 0, "affinity mask should be non-zero")
  end)

  s:test("setaffinity round-trip: restrict to CPU0 then restore", function()
    local orig = posix.getaffinity()
    T.not_nil(orig)
    local ok, err = posix.setaffinity(1)   -- CPU0 only
    T.truthy(ok, "set to 1: " .. tostring(err))
    T.eq(posix.getaffinity(), 1)
    -- restore
    T.truthy(posix.setaffinity(orig))
  end)

  s:test("setaffinity rejects empty mask", function()
    local ok, err = posix.setaffinity(0)
    T.is_nil(ok); T.truthy(err); T.truthy(err:find("empty"))
  end)
else
  print("  (skipping affinity tests: not Linux)")
end

s:test("setprogname does not crash and preserves the environment", function()
  local before = os.getenv("PATH")
  posix.setprogname("fan-v2-test")
  local after = os.getenv("PATH")
  T.eq(after, before, "PATH must not be corrupted by setprogname")
end)

s:test("signals table has SIGTERM=15, SIGKILL=9, SIGINT=2", function()
  T.eq(posix.signals.SIGTERM, 15)
  T.eq(posix.signals.SIGKILL, 9)
  T.eq(posix.signals.SIGINT,  2)
end)

os.exit(T.run(s))
