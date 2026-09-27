-- config — v1 compatibility shim (M14.E).
-- v1 code writes `require "config"`; v2 places the loader under
-- `fan.config` for namespace consistency. This top-level shim keeps
-- unmodified v1 apps working. All state lives in fan.config; requiring
-- either name returns the same result thanks to `package.loaded` caching.
return require("fan.config")
