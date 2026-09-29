-- webase/mimetypes.lua — parse nginx-style mime.types into ext -> content-type.
--
-- Ported from webase v1 with no behavioural change: reads
-- `(WORKDIR or "") .. "mime.types"` (or the local file relative to CWD),
-- splits every `type ext1 ext2 ...;` line, appends `; charset=utf-8` for
-- text/JS/JSON/XML entries. The returned map is what webfile.lua looks
-- up by lowercased extension.
--
-- Design note: we keep the on-disk mime.types file even though luafan2
-- ships fan.zlib, mime data, etc. from the C layer — the same file is
-- what the nginx configs and every v1 app know. Baking the map into
-- Lua would fork the source of truth.

local map = {}

local function split(str, pat)
  local t = {}
  if str then
    local fpat = "(.-)" .. pat
    local last_end = 1
    local s, e, cap = str:find(fpat, 1)
    while s do
      if s ~= 1 or cap ~= "" then
        table.insert(t,cap)
      end
      last_end = e+1
      s, e, cap = str:find(fpat, last_end)
    end
    if last_end <= #str then
        cap = str:sub(last_end)
        table.insert(t, cap)
    end
  end
  return t
end

-- Search order for mime.types:
--   1. `(WORKDIR or "") .. "mime.types"`     (v1 convention when WORKDIR is set)
--   2. `./mime.types`                        (CWD-relative — v1 Docker image)
--   3. Path derived from this module's file: `<webase_dir>/mime.types`
--
-- (3) covers the "app CWD is elsewhere but webase/ is on LUA_PATH" case,
-- which is how the luafan2 integration tests drive the port.
local function find_mime_types()
  local candidates = {
    (WORKDIR or "") .. "mime.types",
    "mime.types",
  }
  local info = debug.getinfo(1, "S")
  local src = info and info.source
  if src and src:sub(1,1) == "@" then
    local dir = src:sub(2):match("^(.-)/[^/]+$")
    if dir and #dir > 0 then
      candidates[#candidates + 1] = dir .. "/mime.types"
    end
  end
  for _, p in ipairs(candidates) do
    local f = io.open(p, "r")
    if f then return f, p end
  end
  error("webase/mimetypes: cannot open mime.types (tried " ..
        table.concat(candidates, ", ") .. ")")
end
local f, _mime_path = find_mime_types()
while true do
  local line = f:read("*line")
  if line then
    local result = line:match("([^;]+);")
    if result then
      local list = split(result, "[\r\n\t ]+")
      local out = {}
      for i,v in ipairs(list) do
        if #(v) > 0 then
          table.insert(out, v)
        end
      end

      --     video/mpeg                            mpeg mpg;
      local is_text = out[1]:match("^text/") or out[1]:match("^application/javascript") or out[1]:match("^application/json") or out[1]:match("^application/xml")
      for i=#(out),2,-1 do
        if is_text then
          map[out[i]] = string.format("%s; charset=utf-8", out[1])
        else
          map[out[i]] = out[1]
        end
      end
    end
  else
    break
  end
end
f:close()

return map
