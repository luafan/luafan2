-- fan/json.lua — entry point shim. The JSON codec is implemented in C
-- (src/codec/json.c) and registered under fan.json by luaopen_fan.
return require("fan").json
