-- webase/mapping.lua — URL rewrite table loaded from `mapping/*.lua`.
--
-- Each file in `(WORKDIR or "") .. mapping` is executed in a locked-down
-- environment (no full _G, no io, no require) and any globals it assigns
-- become entries on the returned table. core.lua consults it as
--   req.path = mapping[req.path] or req.path
-- before dispatching, so a mapping file that writes
--   env["/legacy"] = "/api/v2/users"
-- rewrites `/legacy` at request time.
--
-- v1 change: v1 used `lfs.dir` + `lfs.attributes`. v2 has neither, so we
-- read directory entries via fan.posix.readdir (an all-at-once name
-- listing). A missing/broken directory is a print + no-op, same as v1.

local fan   = require "fan"
local posix = fan.posix

local MODULE_EXT = MODULE_EXT or ".lua"
local MODULE_LOAD_MODE = MODULE_LOAD_MODE or "bt"

local mapping_dir = (WORKDIR or "") .. "mapping"

-- Sandbox env exposed to mapping/*.lua chunks. We deliberately do NOT
-- forward `require`, `io`, `os.execute`, `debug`, etc. — mapping files
-- are declarative rewrite tables, not general scripts.
local safe_os = {
    getenv = os.getenv,
    time = os.time,
    date = os.date,
    clock = os.clock,
    difftime = os.difftime,
}
local env = {os = safe_os, tonumber = tonumber, tostring = tostring}
env._ENV = env
env._G = env

local function load_config(dir)
    local entries, err = posix.readdir(dir)
    if not entries then
        print(string.format("config [%s] not found, ignored.", dir))
        return
    end
    for _, name in ipairs(entries) do
        if name:sub(1,1) ~= "." and name:sub(-#MODULE_EXT) == MODULE_EXT then
            local filepath = string.format("%s/%s", dir, name)
            local chunk, load_err = loadfile(filepath, MODULE_LOAD_MODE, env)
            if not chunk then
                print("[mapping] load error: " .. filepath .. ": " .. tostring(load_err))
            else
                local ok, exec_err = pcall(chunk)
                if not ok then
                    print("[mapping] exec error: " .. filepath .. ": " .. tostring(exec_err))
                end
            end
        end
    end
end

if not _DATABASE_REGISTRY then
    -- Scan mapping/ on disk when no in-memory registry is set. Kept as
    -- an escape hatch for embedders that bundle scripts, matching v1's
    -- `_DATABASE_REGISTRY` sentinel exactly.
    load_config(mapping_dir)
end

return env
