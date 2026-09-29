-- Test fixture config: reads the same env vars as the shipped
-- webase/config.d/service.lua. Kept here so `require "config"` finds
-- something when CWD=fixtures.
service_host = os.getenv("SERVICE_HOST") or "127.0.0.1"
service_port = tonumber(os.getenv("SERVICE_PORT") or 2201)
debug = os.getenv("DEBUG") == "true"
