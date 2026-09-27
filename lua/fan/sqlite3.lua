-- fan/sqlite3.lua — entry point shim. Implementation lives in
-- src/db/sqlite3.c and is registered under fan.sqlite3 when the module
-- was built with -DFAN_WITH_SQLITE3=ON.
local fan = require("fan")
if type(fan.sqlite3) ~= "table" then
  error("fan.sqlite3 not available (rebuild with -DFAN_WITH_SQLITE3=ON)", 2)
end
return fan.sqlite3
