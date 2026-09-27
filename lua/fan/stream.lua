-- fan/stream.lua — entry point shim. Implementation lives in
-- src/codec/stream.c and is registered under fan.stream (with all v1
-- helpers — mark/reset/TestBytes/empty/readline/D64 — implemented in C).
return require("fan").stream
