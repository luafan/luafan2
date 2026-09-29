-- webase/json.lua — v1 compatibility shim.
--
-- webase v1 code writes `require "json"`. luafan2's JSON codec is at
-- fan.json (thin shim over the built-in C codec). Both names return
-- the exact same table (Lua's `package.loaded` cache is shared), so
-- an app can require either.
return require("fan").json
