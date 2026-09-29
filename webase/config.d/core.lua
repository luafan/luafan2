-- Static file root. Kept as WORKDIR-relative for parity with v1: apps
-- typically mount `./web` next to `core.lua` and let WORKDIR resolve to
-- the project directory. Setting an absolute WEBROOT overrides it.
webroot = (WORKDIR or "") .. (os.getenv("WEBROOT") or "web")
