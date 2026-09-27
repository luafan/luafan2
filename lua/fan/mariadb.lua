-- fan/mariadb.lua — entry point shim. Implementation lives in
-- src/db/mariadb.c and is registered under fan.mariadb when the module
-- was built with -DFAN_WITH_MARIADB=ON.
local fan = require("fan")
if type(fan.mariadb) ~= "table" then
  error("fan.mariadb not available (rebuild with -DFAN_WITH_MARIADB=ON)", 2)
end
return fan.mariadb
