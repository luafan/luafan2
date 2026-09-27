-- fan/stream.lua — entry point shim. Implementation lives in src/codec/stream.c
-- and is registered under fan.stream.
--
-- The v1 module layered a few convenience helpers on top of the core codec
-- (mark/reset for rollback, a line reader, and GetBytes() with no argument
-- meaning "everything available"). luafan2 keeps only the core codec, so the
-- helpers are rebuilt here using methods the core does provide.
local stream = require("fan").stream

local probe = stream.new()
local mt = getmetatable(probe)
local core_get_bytes = mt.GetBytes

-- Rollback bookmarks, keyed weakly by stream so abandoned streams can be
-- collected.
local marks = setmetatable({}, { __mode = "k" })

function mt:mark()
  marks[self] = self:pos()
  return true
end

function mt:reset()
  self:pos(marks[self] or 0)
  return true
end

function mt:GetBytes(n)
  if n == nil then
    n = self:available()
  end
  return core_get_bytes(self, n)
end

-- Returns the next buffered line and its terminator. When no complete line is
-- buffered the cursor is restored and nil is returned.
function mt:readline()
  if self:available() <= 0 then
    return
  end

  local parts = {}
  local start_pos = self:pos()
  local breakflag

  while true do
    local b = self:GetBytes(1)
    if not b then
      self:pos(start_pos)
      return
    elseif b == "\r" then
      local after_cr = self:pos()
      if self:GetBytes(1) == "\n" then
        breakflag = "\r\n"
      else
        self:pos(after_cr)
        breakflag = "\r"
      end
      break
    elseif b == "\n" then
      breakflag = "\n"
      break
    else
      table.insert(parts, b)
    end
  end

  return table.concat(parts), breakflag
end

return stream
