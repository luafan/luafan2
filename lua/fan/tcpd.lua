-- fan/tcpd.lua — v1 fan.tcpd compatibility shim (M17).
--
-- v1 code depended on `require("fan.tcpd")` for a callback-based TCP client
-- and server API (immediate handle return; onconnected / onread /
-- onsendready / ondisconnected callbacks; pre-connect send queue).  luafan2
-- restores that behaviour under fan.tcp.connect_async / fan.tcp.bind_async;
-- this module is a thin alias so legacy code that reads
--
--     local tcpd = require("fan.tcpd")
--     tcpd.connect{ host = ..., onconnected = ..., ... }
--     tcpd.bind{ host = ..., onaccept = ..., ... }
--
-- keeps working without a source change.
--
-- Two v1 keys are intentionally NOT supported in v2 and will raise from
-- fan.tcp.connect_async (see src/net/tcp.c):
--
--   * `worker` — v2 has no in-process worker event bases; fan.worker is a
--     multi-process model.  Legacy callers that set `worker = N` must drop
--     the field (the loop the caller is running on is where the callbacks
--     fire).
--
--   * `callback_self_first` — callbacks always receive `self` as the first
--     argument in v2; the v1 legacy "no self" signature is not offered.
--
-- Anything else the v1 API took (host, port, ssl, ssl_host,
-- ssl_verifyhost, ssl_verifypeer, cainfo, capath, pkcs12{path,password},
-- read_timeout, write_timeout, evdns, onconnected, onread, onsendready,
-- ondisconnected, and for bind: cert, key, onsslhostname,
-- send_buffer_size, receive_buffer_size, onaccept) is forwarded verbatim
-- to the native async entries.  Not every one of those is wired in the
-- earliest M17-1 milestone; unrecognised fields are silently ignored on
-- the C side.

local fan = require("fan")

return {
    connect = fan.tcp.connect_async,
    -- bind_async lands in M17-3; until then require("fan.tcpd").bind exposes
    -- the positional fan.tcp.bind for callers that only need the coroutine-
    -- style listener.  M17-3 will swap this to tcp.bind_async.
    bind    = fan.tcp.bind,
}
