--[[
  test_webase_webfile.lua — M16.4 unit tests for webase/webfile.lua gzip
  fallback path.

  Runs in-process without a real webase server: instantiates webfile with
  a mocked req/resp pair and a monkey-patched `fan.zlib.gzip_compress` to
  force the "compression failed" branch, then asserts:

    (a) the response body is the plain (identity) content, not empty
    (b) the Content-Encoding header is NOT set to "gzip"
    (c) Content-Length matches the identity body's length

  The end-to-end happy path (real gzip compression + gzip header +
  gzip cache) is already covered by test_webase.lua's
  "Accept-Encoding: gzip yields Content-Encoding: gzip" test running
  against a subprocess-hosted webase server.
]]
local T   = require("test_framework")
local fan = require("fan")

-- webase/ is not on package.path by default (it's picked up when spawning
-- a webase server via LUA_PATH).  Add it here relative to this file.
local this_dir = (debug.getinfo(1, "S").source:gsub("^@", ""):match("(.*)/[^/]+$")) or "."
local ROOT = this_dir:gsub("/tests/lua$", "")
package.path = ROOT .. "/webase/?.lua;" .. package.path

-- webfile does `require "config"`; provide a minimal one before it loads.
package.preload["config"] = function()
  return { webroot = "/tmp/luafan2-webfile-test-webroot",
           directory_index = "index.html" }
end

-- Create the webroot + a small static file to serve.
local WEBROOT = "/tmp/luafan2-webfile-test-webroot"
os.execute("rm -rf "  .. WEBROOT)
os.execute("mkdir -p " .. WEBROOT)
local PAYLOAD = string.rep("HELLO M16.4\n", 32)   -- ~380 bytes, compressible
local fh = io.open(WEBROOT .. "/hello.txt", "wb")
fh:write(PAYLOAD); fh:close()

-- Mock req / resp objects.  We record every addheader() + the final
-- reply() args into simple tables the test can inspect.
local function make_req(headers)
  return {
    method  = "GET",
    path    = "/hello.txt",
    headers = headers,
    remoteip = "127.0.0.1",
  }
end

local function make_resp()
  local r = { headers = {}, replied = false }
  function r:addheader(k, v) self.headers[k] = v end
  function r:reply(code, reason, body)
    self.code, self.reason, self.body = code, reason, body
    self.replied = true
  end
  function r:reply_start(code, reason) self.code, self.reason = code, reason end
  function r:reply_chunk(s)  self.body = (self.body or "") .. s end
  function r:reply_end() self.replied = true end
  return r
end

local webfile = require("webfile")
local s = T.suite("webase.webfile gzip fallback (M16.4)")

s:test("happy path: gzip compression success sets Content-Encoding: gzip", function()
  webfile.reset_cache()
  local req  = make_req{ ["Accept-Encoding"] = "gzip" }
  local resp = make_resp()
  webfile.web(req, resp)
  T.eq(resp.code, 200)
  T.eq(resp.headers["Content-Encoding"], "gzip")
  T.truthy(resp.body)                     -- non-empty compressed body
  T.truthy(#resp.body > 0)
  T.truthy(#resp.body < #PAYLOAD)          -- compressed is smaller than plain
  T.eq(resp.headers["Content-Length"], #resp.body)
end)

s:test("M16.4: gzip failure falls back to identity body + no gzip header", function()
  webfile.reset_cache()
  -- Monkey-patch fan.zlib.gzip_compress to return nil, simulating a
  -- compressor unavailable / broken input scenario.  Restored in the
  -- test's tail so following suites still see the real function.
  local orig = fan.zlib.gzip_compress
  fan.zlib.gzip_compress = function() return nil end

  local req  = make_req{ ["Accept-Encoding"] = "gzip" }
  local resp = make_resp()
  local ok, err = pcall(webfile.web, req, resp)

  fan.zlib.gzip_compress = orig  -- restore before any assertion may fail

  T.eq(ok, true, "webfile must not error when gzip fails: " .. tostring(err))
  T.eq(resp.code, 200)
  T.eq(resp.headers["Content-Encoding"], nil)   -- no gzip header
  T.eq(resp.body, PAYLOAD)                       -- identity body served
  T.eq(resp.headers["Content-Length"], #PAYLOAD)
end)

s:test("M16.4: client without Accept-Encoding gets identity body (no gzip attempted)", function()
  webfile.reset_cache()
  local req  = make_req{}    -- no Accept-Encoding at all
  local resp = make_resp()
  webfile.web(req, resp)
  T.eq(resp.code, 200)
  T.eq(resp.headers["Content-Encoding"], nil)
  T.eq(resp.body, PAYLOAD)
end)

-- Cleanup runs as the suite's final test so it happens AFTER the earlier
-- tests actually execute (T.run below).  A bare top-level `os.execute`
-- here would tear the webroot down BEFORE any test body runs and make
-- every earlier case 404.
s:test("cleanup: remove throwaway webroot", function()
    os.execute("rm -rf " .. WEBROOT)
    T.eq(true, true)
end)

os.exit(T.run(s))
